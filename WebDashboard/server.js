'use strict';

const express  = require('express');
const net      = require('net');
const http     = require('http');
const WebSocket = require('ws');
const multer   = require('multer');
const crypto   = require('crypto');
const fs       = require('fs');
const path     = require('path');
let nodemailer = null;
try { nodemailer = require('nodemailer'); } catch (_) { console.warn('[EMAIL] nodemailer not installed — email alerts disabled'); }

// ─── Config ────────────────────────────────────────────────────
const HTTP_PORT      = 3000;
const TCP_PORT       = 3001;
const MAX_HISTORY    = 120;   // rolling telemetry window (points)
const MAX_FAULT_LOG  = 100;   // keep last N fault events
const MAX_DEVICE_LOG = 200;   // device event log entries

// ─── Auth ──────────────────────────────────────────────────────
const CREDENTIALS   = { admin: 'predictiv@2024' }; // change password here
const DEVICE_SECRET = 'stm32device2024';            // ESP32 uses this to fetch firmware
const sessions      = new Map();                    // token → expiry ms

function createToken() {
    const t = crypto.randomBytes(32).toString('hex');
    sessions.set(t, Date.now() + 24 * 60 * 60 * 1000); // 24 h
    return t;
}

function isValidToken(token) {
    if (!token) return false;
    const exp = sessions.get(token);
    if (!exp) return false;
    if (Date.now() > exp) { sessions.delete(token); return false; }
    return true;
}

function requireAuth(req, res, next) {
    const token = req.headers['x-auth-token'] || req.query.token;
    if (!isValidToken(token)) return res.status(401).json({ error: 'Unauthorized' });
    next();
}

// ─── Config paths ──────────────────────────────────────────────
const EMAIL_CONFIG_FILE      = path.join(__dirname, 'email_config.json');
const THRESHOLDS_CONFIG_FILE = path.join(__dirname, 'thresholds_config.json');

// ─── State ─────────────────────────────────────────────────────
let stm32Socket      = null;  // active TCP connection from STM32/ESP32
let stm32ConnectedAt = null;
let lastTelemetry    = null;  // latest telemetry packet
let telemetryHistory = [];    // circular buffer of recent packets
let faultLog         = [];    // [{ts, peak, freq}]
let deviceLog        = [];    // [{ts, level, msg}] — device event log
let messageCount     = 0;
let messageRateBuffer= [];    // timestamps for rate calculation
let serverStartTime  = Date.now();
let lastTelemetryAt  = null;  // for data-loss watchdog
let deviceDataLive   = false; // tracks whether data is actively flowing

// ─── Condition Indicators rolling buffer ───────────────────────
// We track raw az samples (same axis as DSP FFT), then DC-remove by
// subtracting the running mean.  This gives the true vibration signal
// free of the 1 g gravity offset, so CF and Kurtosis respond to actual
// bearing/motor excitation rather than being dominated by gravity.
const CI_WINDOW = 64;
let   azBuffer  = [];   // raw az samples (g units)

function computeCI(azBuf) {
    const n = azBuf.length;
    if (n < 4) return { cf: 1.0, kurt: 3.0 };

    // 1. DC-remove (subtract mean) → pure vibration signal
    const mu  = azBuf.reduce((s, x) => s + x, 0) / n;
    const vib = azBuf.map(x => x - mu);

    // 2. RMS of vibration signal
    const rms = Math.sqrt(vib.reduce((s, x) => s + x * x, 0) / n);

    // 3. Crest Factor = Peak / RMS  (uses |x| so negative swings count)
    const pk = Math.max(...vib.map(Math.abs));
    // For pure Gaussian noise CF ≈ 3; impulsive faults push it higher.
    // When rms is below quantisation noise floor return 1.0 as neutral value.
    const cf = rms > 1e-5 ? +(pk / rms).toFixed(3) : 1.0;

    // 4. Kurtosis = 4th standardised moment of the vibration signal
    //    Healthy (Gaussian) ≈ 3.  Impulsive faults → 5–20+.
    const s2   = vib.reduce((s, x) => s + (x - 0) ** 2, 0) / n;  // mu of vib = 0 after DC removal
    const s4   = vib.reduce((s, x) => s + x ** 4, 0) / n;
    const kurt = s2 > 1e-10 ? +(s4 / (s2 * s2)).toFixed(3) : 3.0;

    return { cf, kurt };
}

// ─── Email Alert state ─────────────────────────────────────────
let emailConfig = {
    recipient: '',
    sender:    '',
    password:  '',
    cooldown:  5   // minutes
};
let lastAlertAt = 0;

function loadEmailConfig() {
    try {
        const raw = JSON.parse(fs.readFileSync(EMAIL_CONFIG_FILE));
        emailConfig = { ...emailConfig, ...raw };
        console.log('[EMAIL] Config loaded');
    } catch (_) { /* first run — no file yet */ }
}

function saveEmailConfig() {
    fs.writeFileSync(EMAIL_CONFIG_FILE, JSON.stringify(emailConfig, null, 2));
}

async function sendFaultAlert(peak, freq) {
    if (!nodemailer) return;
    if (!emailConfig.recipient || !emailConfig.sender || !emailConfig.password) return;
    const now = Date.now();
    if (now - lastAlertAt < emailConfig.cooldown * 60 * 1000) return;
    lastAlertAt = now;
    try {
        const transporter = nodemailer.createTransport({
            service: 'gmail',
            auth: { user: emailConfig.sender, pass: emailConfig.password }
        });
        await transporter.sendMail({
            from: `"PredictivEdge" <${emailConfig.sender}>`,
            to:   emailConfig.recipient,
            subject: '⚠ Bearing Fault Detected — PredictivEdge',
            html: `<h2 style="color:#ef4444">⚠ Bearing Fault Detected</h2>
                   <p><b>Peak:</b> ${peak.toFixed(3)} G &nbsp;|&nbsp; <b>Freq:</b> ${freq.toFixed(1)} Hz</p>
                   <p><b>Time:</b> ${new Date().toLocaleString()}</p>
                   <hr><p style="color:#64748b;font-size:12px">PredictivEdge Predictive Maintenance Monitor</p>`
        });
        console.log('[EMAIL] Fault alert sent');
    } catch (err) {
        console.error('[EMAIL] Send error:', err.message);
    }
}

loadEmailConfig();

// ─── Fault Thresholds ──────────────────────────────────────────
// Thresholds are checked server-side on every CI computation.
// When any indicator crosses its threshold the server fires a
// ci_fault broadcast AND sends an email alert (same cooldown as
// the FFT-based fault path).
let thresholds = {
    peak_g : 1.0,  // peak vibration in g (from DSP FFT)
    cf     : 3.5,  // Crest Factor — healthy Gaussian ≈ 1.4; >3.5 = suspect
    kurt   : 5.0   // Kurtosis     — healthy ≈ 3; >5 = impulsive, >10 = severe
};

function loadThresholds() {
    try {
        const raw = JSON.parse(fs.readFileSync(THRESHOLDS_CONFIG_FILE));
        thresholds = { ...thresholds, ...raw };
        console.log('[THRESH] Config loaded:', thresholds);
    } catch (_) { /* first run — use defaults */ }
}

function saveThresholds() {
    fs.writeFileSync(THRESHOLDS_CONFIG_FILE, JSON.stringify(thresholds, null, 2));
}

loadThresholds();

function appendDeviceLog(level, msg) {
    const entry = { ts: Date.now(), level, msg };
    deviceLog.push(entry);
    if (deviceLog.length > MAX_DEVICE_LOG) deviceLog.shift();
    broadcast({ type: 'device_log', ...entry });
    console.log(`[DEVLOG] [${level}] ${msg}`);
}

// ─── Express / HTTP ────────────────────────────────────────────
const app    = express();
const server = http.createServer(app);
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ─── Firmware directory ────────────────────────────────────────
const FIRMWARE_DIR = path.join(__dirname, 'firmware');
if (!fs.existsSync(FIRMWARE_DIR)) fs.mkdirSync(FIRMWARE_DIR);

const storage = multer.diskStorage({
    destination: FIRMWARE_DIR,
    filename: (_req, _file, cb) => cb(null, 'update.bin')
});
const upload = multer({ storage, limits: { fileSize: 2 * 1024 * 1024 } });

// Firmware download — ESP32 uses device secret (no user token needed)
app.get('/firmware/update.bin', (req, res) => {
    if (req.query.secret !== DEVICE_SECRET) return res.status(403).send('Forbidden');
    const file = path.join(FIRMWARE_DIR, 'update.bin');
    if (!fs.existsSync(file)) return res.status(404).send('No firmware uploaded yet');
    res.download(file, 'update.bin');
});

// ─── Public routes (no auth) ───────────────────────────────────
app.post('/api/login', (req, res) => {
    const { username, password } = req.body || {};
    if (username && CREDENTIALS[username] === password) {
        return res.json({ token: createToken(), username });
    }
    res.status(401).json({ error: 'Invalid username or password' });
});

// ─── WebSocket (server → browser) ─────────────────────────────
const wss = new WebSocket.Server({ server });

function broadcast(data) {
    const msg = JSON.stringify(data);
    wss.clients.forEach(client => {
        if (client.readyState === WebSocket.OPEN) client.send(msg);
    });
}

wss.on('connection', (ws, req) => {
    // Validate token from query string: ws://host?token=xxx
    const urlParams = new URLSearchParams(req.url.split('?')[1] || '');
    const token = urlParams.get('token');
    if (!isValidToken(token)) {
        ws.close(4001, 'Unauthorized');
        return;
    }

    console.log('[WS] Browser connected');

    // Send current state to the new client immediately
    ws.send(JSON.stringify({
        type: 'init',
        device_connected: stm32Socket !== null,
        connected_since: stm32ConnectedAt,
        server_uptime: Date.now() - serverStartTime,
        history: telemetryHistory,
        fault_log: faultLog,
        device_log: deviceLog,
        firmware: loadFirmwareMeta(),
        thresholds,
        email_configured: !!(emailConfig.recipient && emailConfig.sender && emailConfig.password),
        email_recipient: emailConfig.recipient,
        email_sender: emailConfig.sender,
        email_cooldown: emailConfig.cooldown
    }));
});

// ─── REST API ──────────────────────────────────────────────────

// Upload new firmware binary
app.post('/api/upload', requireAuth, upload.single('firmware'), (req, res) => {
    if (!req.file) return res.status(400).json({ error: 'No file uploaded' });

    const filePath   = path.join(FIRMWARE_DIR, 'update.bin');
    const fileBuffer = fs.readFileSync(filePath);
    const hash       = crypto.createHash('sha256').update(fileBuffer).digest('hex').toUpperCase();
    const versionInfo = {
        version   : Date.now(),
        size      : fileBuffer.length,
        sha256    : hash,
        uploaded_at: new Date().toISOString()
    };

    fs.writeFileSync(path.join(FIRMWARE_DIR, 'version.json'), JSON.stringify(versionInfo, null, 2));
    console.log(`[OTA] Firmware uploaded — ${fileBuffer.length} bytes  SHA-256: ${hash}`);

    broadcast({ type: 'ota_update', ...versionInfo });
    res.json({ success: true, ...versionInfo });
});

// Trigger OTA on device — sends "UPDATE\n" back through the live TCP socket
app.post('/api/ota/trigger', requireAuth, (_req, res) => {
    if (!stm32Socket || stm32Socket.destroyed) {
        return res.status(503).json({ error: 'Device not connected' });
    }
    stm32Socket.write('UPDATE\n');
    console.log('[OTA] UPDATE command sent to device');
    broadcast({ type: 'ota_triggered', ts: Date.now() });
    res.json({ success: true });
});

// Reboot device into bootloader
app.post('/api/device/reboot', requireAuth, (_req, res) => {
    if (!stm32Socket || stm32Socket.destroyed) {
        return res.status(503).json({ error: 'Device not connected' });
    }
    stm32Socket.write('REBOOT\n');
    console.log('[CMD] REBOOT command sent to device');
    broadcast({ type: 'device_reboot', ts: Date.now() });
    res.json({ success: true });
});

// Set STM32 DSP Parameter
app.post('/api/device/set-param', requireAuth, (req, res) => {
    if (!stm32Socket || stm32Socket.destroyed) {
        return res.status(503).json({ error: 'Device not connected' });
    }
    const { name, value } = req.body || {};
    if (!name || value === undefined) return res.status(400).json({ error: 'Missing name or value' });

    let cmd = '';
    if (name === 'threshold') cmd = `PARAM:THR:${Number(value).toFixed(3)}\n`;
    else return res.status(400).json({ error: 'Unsupported parameter' });

    stm32Socket.write(cmd);
    console.log(`[CMD] ${cmd.trim()} sent to device`);
    res.json({ success: true });
});

// Request Device Logs/Status
app.post('/api/device/request-logs', requireAuth, (_req, res) => {
    if (!stm32Socket || stm32Socket.destroyed) {
        return res.status(503).json({ error: 'Device not connected' });
    }
    stm32Socket.write('GET_LOGS\n');
    console.log('[CMD] GET_LOGS command sent to device');
    res.json({ success: true });
});

// Server + device status snapshot
app.get('/api/status', requireAuth, (_req, res) => {
    res.json({
        server_uptime   : Date.now() - serverStartTime,
        device_connected: stm32Socket !== null && !stm32Socket.destroyed,
        connected_since : stm32ConnectedAt,
        message_rate    : calcRate(),
        last_telemetry  : lastTelemetry,
        fault_count     : faultLog.length,
        firmware        : loadFirmwareMeta()
    });
});

// Historical telemetry (last N points)
app.get('/api/history', requireAuth, (_req, res) => {
    res.json(telemetryHistory);
});

// Fault log
app.get('/api/faults', requireAuth, (_req, res) => {
    res.json(faultLog);
});

// Device event log
app.get('/api/device/log', requireAuth, (_req, res) => {
    res.json(deviceLog);
});

// ─── Email config API ──────────────────────────────────────────
app.get('/api/email/config', requireAuth, (_req, res) => {
    res.json({ ...emailConfig, password: emailConfig.password ? '********' : '', configured: !!(emailConfig.recipient && emailConfig.sender && emailConfig.password) });
});

app.post('/api/email/config', requireAuth, (req, res) => {
    const { recipient, sender, password, cooldown } = req.body || {};
    if (recipient !== undefined) emailConfig.recipient = recipient;
    if (sender    !== undefined) emailConfig.sender    = sender;
    if (password  && password !== '********') emailConfig.password = password;
    if (cooldown  !== undefined) emailConfig.cooldown  = Number(cooldown) || 5;
    saveEmailConfig();
    broadcast({ type: 'email_config', configured: !!(emailConfig.recipient && emailConfig.sender && emailConfig.password) });
    res.json({ success: true });
});

app.post('/api/email/test', requireAuth, async (_req, res) => {
    if (!nodemailer) return res.status(503).json({ error: 'nodemailer not installed' });
    if (!emailConfig.recipient || !emailConfig.sender || !emailConfig.password)
        return res.status(400).json({ error: 'Email not configured' });
    try {
        const transporter = nodemailer.createTransport({ service: 'gmail', auth: { user: emailConfig.sender, pass: emailConfig.password } });
        await transporter.sendMail({
            from: `"PredictivEdge" <${emailConfig.sender}>`,
            to:   emailConfig.recipient,
            subject: '✅ PredictivEdge — Test Alert',
            html: '<h2>Test Alert</h2><p>Email alerts are configured correctly.</p>'
        });
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

// ─── Thresholds API ────────────────────────────────────────────
app.get('/api/thresholds', requireAuth, (_req, res) => {
    res.json(thresholds);
});

app.post('/api/thresholds', requireAuth, (req, res) => {
    const { peak_g, cf, kurt } = req.body || {};
    if (peak_g !== undefined) thresholds.peak_g = +peak_g || thresholds.peak_g;
    if (cf     !== undefined) thresholds.cf     = +cf     || thresholds.cf;
    if (kurt   !== undefined) thresholds.kurt   = +kurt   || thresholds.kurt;
    saveThresholds();
    broadcast({ type: 'thresholds', ...thresholds });
    console.log('[THRESH] Updated:', thresholds);
    res.json({ success: true, thresholds });
});

// ─── CSV Export ────────────────────────────────────────────────
app.get('/api/export/telemetry.csv', requireAuth, (_req, res) => {
    const header = 'timestamp,ax,ay,az,temp,peak_hz,fault,cf,kurtosis,overflows,mx,my,mz,heading,press,alt\n';
    const rows = telemetryHistory.map(t =>
        [new Date(t.ts).toISOString(),
         (t.ax||0).toFixed(4), (t.ay||0).toFixed(4), (t.az||0).toFixed(4),
         (t.temp||0).toFixed(1), (t.freq||0).toFixed(1),
         t.fault||0,
         (t.cf||0).toFixed(3), (t.kurt||0).toFixed(3),
         t.overflows||0,
         (t.mx||0).toFixed(1), (t.my||0).toFixed(1), (t.mz||0).toFixed(1),
         (t.heading||0).toFixed(1), (t.press||0).toFixed(0), (t.alt||0).toFixed(1)
        ].join(',')
    ).join('\n');
    res.setHeader('Content-Type', 'text/csv');
    res.setHeader('Content-Disposition', 'attachment; filename="predictivEdge_telemetry.csv"');
    res.send(header + rows);
});

app.get('/api/export/faults.csv', requireAuth, (_req, res) => {
    const header = 'timestamp,peak_g,freq_hz\n';
    const rows = faultLog.map(f =>
        [new Date(f.ts).toISOString(),
         (f.peak||0).toFixed(3), (f.freq||0).toFixed(1)
        ].join(',')
    ).join('\n');
    res.setHeader('Content-Type', 'text/csv');
    res.setHeader('Content-Disposition', 'attachment; filename="predictivEdge_faults.csv"');
    res.send(header + rows);
});

// ─── TCP Server (STM32/ESP32 → Node) ──────────────────────────
const tcpServer = net.createServer((socket) => {
    const remote = `${socket.remoteAddress}:${socket.remotePort}`;
    console.log(`[TCP] Device connected: ${remote}`);

    // Only one device at a time
    if (stm32Socket && !stm32Socket.destroyed) {
        console.log('[TCP] Replacing previous connection');
        stm32Socket.destroy();
    }
    stm32Socket      = socket;
    stm32ConnectedAt = Date.now();

    appendDeviceLog('CONNECT', `Device connected from ${remote.split(':')[0]}`);
    broadcast({ type: 'device_status', connected: true, ts: stm32ConnectedAt, remote });

    let buffer = '';

    socket.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop(); // keep incomplete tail

        for (let line of lines) {
            line = line.trim();
            if (!line) continue;

            // ── Non-JSON device messages ───────────────────────
            if (line === 'FLASH_OK') {
                console.log('[OTA] Flash acknowledged: SUCCESS');
                appendDeviceLog('OTA', 'Firmware flashed successfully — STM32 rebooting');
                broadcast({ type: 'ota_flash_result', success: true,
                            message: 'Firmware flashed successfully. STM32 rebooting.', ts: Date.now() });
                continue;
            }
            if (line.startsWith('FLASH_FAIL:')) {
                const reason = line.slice(11);
                console.log(`[OTA] Flash acknowledged: FAIL — ${reason}`);
                appendDeviceLog('ERROR', `OTA flash failed: ${reason}`);
                broadcast({ type: 'ota_flash_result', success: false,
                            message: `Flash failed: ${reason}`, ts: Date.now() });
                continue;
            }
            if (line.startsWith('LOG:')) {
                // Format: LOG:LEVEL:message text
                const rest  = line.slice(4);
                const colon = rest.indexOf(':');
                const level = colon >= 0 ? rest.slice(0, colon) : 'INFO';
                const msg   = colon >= 0 ? rest.slice(colon + 1) : rest;
                appendDeviceLog(level, msg);

                // OTA progress: LOG:OTA:Progress: N% — extract and broadcast
                if (level === 'OTA' && msg.startsWith('Progress: ')) {
                    const pct = parseInt(msg.slice(10));
                    if (!isNaN(pct)) broadcast({ type: 'ota_progress', pct, msg });
                }
                continue;
            }

            try {
                const telemetry  = JSON.parse(line);
                telemetry.type   = 'telemetry';
                telemetry.ts     = Date.now();

                // ── Condition Indicators (CF & Kurtosis) ──────────
                // Use raw az (same axis the DSP FFT uses).  DC removal
                // happens inside computeCI so gravity bias is cancelled.
                if (telemetry.az !== undefined) {
                    azBuffer.push(telemetry.az);
                    if (azBuffer.length > CI_WINDOW) azBuffer.shift();
                    const ci = computeCI(azBuffer);
                    telemetry.cf   = ci.cf;
                    telemetry.kurt = ci.kurt;
                }

                // Rolling history
                telemetryHistory.push(telemetry);
                if (telemetryHistory.length > MAX_HISTORY)
                    telemetryHistory.shift();

                lastTelemetry   = telemetry;
                lastTelemetryAt = Date.now();
                messageCount++;
                messageRateBuffer.push(Date.now());

                // If data just resumed after a stale period, re-announce online
                if (!deviceDataLive) {
                    deviceDataLive = true;
                    broadcast({ type: 'device_status', connected: true, ts: stm32ConnectedAt, remote });
                }

                // ── FFT-based fault (from STM32 DSP task) ────────────
                if (telemetry.fault === 1) {
                    const event = { ts: telemetry.ts, peak: telemetry.peak,
                                    freq: telemetry.freq, source: 'dsp' };
                    faultLog.push(event);
                    if (faultLog.length > MAX_FAULT_LOG) faultLog.shift();
                    broadcast({ type: 'fault_event', ...event });
                    sendFaultAlert(telemetry.peak, telemetry.freq).catch(() => {});
                }

                // ── CI-based fault (server-side threshold check) ──────
                // Checks Crest Factor and Kurtosis against user-defined
                // thresholds.  Fires independently of the STM32 fault flag
                // so early bearing wear (pre-FFT-detectable) is still caught.
                if (telemetry.cf !== undefined || telemetry.kurt !== undefined) {
                    const cfFault   = (telemetry.cf   || 0) > thresholds.cf;
                    const kurtFault = (telemetry.kurt || 0) > thresholds.kurt;
                    if (cfFault || kurtFault) {
                        const reason = [
                            cfFault   ? `CF=${(telemetry.cf||0).toFixed(2)} > ${thresholds.cf}`   : '',
                            kurtFault ? `Kurt=${(telemetry.kurt||0).toFixed(2)} > ${thresholds.kurt}` : ''
                        ].filter(Boolean).join(', ');
                        broadcast({ type: 'ci_fault', reason,
                                    cf: telemetry.cf, kurt: telemetry.kurt, ts: telemetry.ts });
                        sendFaultAlert(telemetry.peak || 0, telemetry.freq || 0).catch(() => {});
                    }
                }

                // Broadcast to dashboard
                telemetry.rate = calcRate();
                broadcast(telemetry);

            } catch (e) {
                console.warn('[TCP] Bad JSON:', line.substring(0, 80));
            }
        }
    });

    socket.on('close', () => {
        console.log(`[TCP] Device disconnected: ${remote}`);
        if (stm32Socket === socket) stm32Socket = null;
        deviceDataLive  = false;
        lastTelemetryAt = null;
        appendDeviceLog('DISCONNECT', `Device disconnected from ${remote.split(':')[0]}`);
        broadcast({ type: 'device_status', connected: false, ts: Date.now() });
    });

    socket.on('error', (err) => {
        console.error(`[TCP] Socket error: ${err.message}`);
    });
});

// ─── Helpers ───────────────────────────────────────────────────
function calcRate() {
    const now    = Date.now();
    const cutoff = now - 5000; // msgs in last 5 s
    messageRateBuffer = messageRateBuffer.filter(t => t > cutoff);
    return +(messageRateBuffer.length / 5).toFixed(1); // msgs/s
}

function loadFirmwareMeta() {
    try {
        return JSON.parse(fs.readFileSync(path.join(FIRMWARE_DIR, 'version.json')));
    } catch (_) { return null; }
}

// Broadcast rate ticker every 5 s so the UI stays fresh
setInterval(() => {
    broadcast({ type: 'rate_tick', rate: calcRate(), server_uptime: Date.now() - serverStartTime });
}, 5000);

// ─── Data watchdog — mark offline if no telemetry for 5 s ─────
setInterval(() => {
    if (!lastTelemetryAt) return;                          // never received data yet
    const stale = (Date.now() - lastTelemetryAt) > 5000;  // 5 s silence threshold
    if (stale && deviceDataLive) {
        deviceDataLive = false;
        console.log('[WATCHDOG] No telemetry for 5 s — marking device offline');
        broadcast({ type: 'device_status', connected: false });
    }
}, 2000); // check every 2 s

// ─── Start ─────────────────────────────────────────────────────
const HOST = process.env.HOST || '0.0.0.0';

server.listen(HTTP_PORT, HOST, () => {
    console.log(`[HTTP] Dashboard    → http://${HOST}:${HTTP_PORT}`);
    console.log(`[WS]  WebSocket     → ws://${HOST}:${HTTP_PORT}`);
});

tcpServer.listen(TCP_PORT, HOST, () => {
    console.log(`[TCP]  Device telemetry listener → ${HOST}:${TCP_PORT}`);
});

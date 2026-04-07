#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════
#  PredictivEdge — self-contained server install script
#  Run this directly on the Ubuntu server:
#    chmod +x install.sh && sudo bash install.sh
# ════════════════════════════════════════════════════════════════
set -euo pipefail

APP_DIR="/opt/predictivEdge"
NODE_MAJOR=20

echo ""
echo "╔══════════════════════════════════════╗"
echo "║  PredictivEdge — Server Install      ║"
echo "╚══════════════════════════════════════╝"
echo ""

# ── 1. System packages ────────────────────────────────────────
echo "[1/8] Installing system packages..."
apt-get update -qq
apt-get install -y -qq curl nginx ufw

# ── 2. Node.js 20 LTS ─────────────────────────────────────────
if ! command -v node &>/dev/null || [[ "$(node -e 'process.stdout.write(process.version.split(".")[0].slice(1))')" -lt "$NODE_MAJOR" ]]; then
    echo "[2/8] Installing Node.js $NODE_MAJOR LTS..."
    curl -fsSL https://deb.nodesource.com/setup_${NODE_MAJOR}.x | bash - >/dev/null 2>&1
    apt-get install -y nodejs >/dev/null 2>&1
else
    echo "[2/8] Node.js $(node --version) already present — skipping."
fi

# ── 3. PM2 ────────────────────────────────────────────────────
echo "[3/8] Installing PM2..."
npm install -g pm2 --quiet >/dev/null 2>&1

# ── 4. Create directory structure ─────────────────────────────
echo "[4/8] Creating app directory at $APP_DIR..."
mkdir -p "$APP_DIR/public"
mkdir -p "$APP_DIR/firmware"
mkdir -p "$APP_DIR/logs"

# ── 5. Write application files ────────────────────────────────
echo "[5/8] Writing application files..."

# ── package.json ──────────────────────────────────────────────
cat > "$APP_DIR/package.json" << 'PKGJSON'
{
  "name": "predictivEdge",
  "version": "2.0.0",
  "description": "MPU-6050 Predictive Maintenance Dashboard",
  "main": "server.js",
  "scripts": { "start": "node server.js" },
  "dependencies": {
    "express": "^4.18.2",
    "multer": "^1.4.5-lts.1",
    "ws": "^8.13.0"
  }
}
PKGJSON

# ── ecosystem.config.js ───────────────────────────────────────
cat > "$APP_DIR/ecosystem.config.js" << 'ECOSYSTEM'
module.exports = {
  apps: [{
    name        : 'predictivEdge',
    script      : 'server.js',
    cwd         : __dirname,
    instances   : 1,
    autorestart : true,
    watch       : false,
    max_memory_restart: '256M',
    env: {
      NODE_ENV  : 'production',
      HOST      : '0.0.0.0',
      HTTP_PORT : 3000,
      TCP_PORT  : 3001
    },
    error_file  : './logs/err.log',
    out_file    : './logs/out.log',
    log_date_format: 'YYYY-MM-DD HH:mm:ss'
  }]
};
ECOSYSTEM

# ── server.js ─────────────────────────────────────────────────
cat > "$APP_DIR/server.js" << 'SERVERJS'
'use strict';

const express   = require('express');
const net       = require('net');
const http      = require('http');
const WebSocket = require('ws');
const multer    = require('multer');
const crypto    = require('crypto');
const fs        = require('fs');
const path      = require('path');

const HTTP_PORT     = parseInt(process.env.HTTP_PORT) || 3000;
const TCP_PORT      = parseInt(process.env.TCP_PORT)  || 3001;
const HOST          = process.env.HOST || '0.0.0.0';
const MAX_HISTORY   = 120;
const MAX_FAULT_LOG = 100;

let stm32Socket      = null;
let stm32ConnectedAt = null;
let lastTelemetry    = null;
let telemetryHistory = [];
let faultLog         = [];
let messageRateBuffer = [];
let serverStartTime  = Date.now();

const app    = express();
const server = http.createServer(app);
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

const FIRMWARE_DIR = path.join(__dirname, 'firmware');
if (!fs.existsSync(FIRMWARE_DIR)) fs.mkdirSync(FIRMWARE_DIR);

const storage = multer.diskStorage({
    destination: FIRMWARE_DIR,
    filename: (_req, _file, cb) => cb(null, 'update.bin')
});
const upload = multer({ storage });
app.use('/firmware', express.static(FIRMWARE_DIR));

const wss = new WebSocket.Server({ server });

function broadcast(data) {
    const msg = JSON.stringify(data);
    wss.clients.forEach(c => { if (c.readyState === WebSocket.OPEN) c.send(msg); });
}

wss.on('connection', (ws) => {
    console.log('[WS] Browser connected');
    ws.send(JSON.stringify({
        type: 'init',
        device_connected: stm32Socket !== null,
        connected_since: stm32ConnectedAt,
        server_uptime: Date.now() - serverStartTime,
        history: telemetryHistory,
        fault_log: faultLog,
        firmware: loadFirmwareMeta()
    }));
});

app.post('/api/upload', upload.single('firmware'), (req, res) => {
    if (!req.file) return res.status(400).json({ error: 'No file uploaded' });
    const filePath   = path.join(FIRMWARE_DIR, 'update.bin');
    const fileBuffer = fs.readFileSync(filePath);
    const hash       = crypto.createHash('sha256').update(fileBuffer).digest('hex').toUpperCase();
    const versionInfo = { version: Date.now(), size: fileBuffer.length, sha256: hash, uploaded_at: new Date().toISOString() };
    fs.writeFileSync(path.join(FIRMWARE_DIR, 'version.json'), JSON.stringify(versionInfo, null, 2));
    console.log(`[OTA] Uploaded ${fileBuffer.length} bytes  SHA-256: ${hash}`);
    broadcast({ type: 'ota_update', ...versionInfo });
    res.json({ success: true, ...versionInfo });
});

app.post('/api/ota/trigger', (_req, res) => {
    if (!stm32Socket || stm32Socket.destroyed) return res.status(503).json({ error: 'Device not connected' });
    stm32Socket.write('UPDATE\n');
    broadcast({ type: 'ota_triggered', ts: Date.now() });
    res.json({ success: true });
});

app.post('/api/device/reboot', (_req, res) => {
    if (!stm32Socket || stm32Socket.destroyed) return res.status(503).json({ error: 'Device not connected' });
    stm32Socket.write('REBOOT\n');
    broadcast({ type: 'device_reboot', ts: Date.now() });
    res.json({ success: true });
});

app.get('/api/status', (_req, res) => {
    res.json({
        server_uptime: Date.now() - serverStartTime,
        device_connected: stm32Socket !== null && !stm32Socket.destroyed,
        connected_since: stm32ConnectedAt,
        message_rate: calcRate(),
        last_telemetry: lastTelemetry,
        fault_count: faultLog.length,
        firmware: loadFirmwareMeta()
    });
});

app.get('/api/history', (_req, res) => res.json(telemetryHistory));
app.get('/api/faults',  (_req, res) => res.json(faultLog));

const tcpServer = net.createServer((socket) => {
    const remote = `${socket.remoteAddress}:${socket.remotePort}`;
    console.log(`[TCP] Device connected: ${remote}`);
    if (stm32Socket && !stm32Socket.destroyed) stm32Socket.destroy();
    stm32Socket      = socket;
    stm32ConnectedAt = Date.now();
    broadcast({ type: 'device_status', connected: true, ts: stm32ConnectedAt, remote });

    let buffer = '';
    socket.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop();
        for (let line of lines) {
            line = line.trim();
            if (!line) continue;
            try {
                const t = JSON.parse(line);
                t.type = 'telemetry';
                t.ts   = Date.now();
                telemetryHistory.push(t);
                if (telemetryHistory.length > MAX_HISTORY) telemetryHistory.shift();
                lastTelemetry = t;
                messageRateBuffer.push(Date.now());
                if (t.fault === 1) {
                    const ev = { ts: t.ts, peak: t.peak, freq: t.freq };
                    faultLog.push(ev);
                    if (faultLog.length > MAX_FAULT_LOG) faultLog.shift();
                    broadcast({ type: 'fault_event', ...ev });
                }
                t.rate = calcRate();
                broadcast(t);
            } catch (e) { console.warn('[TCP] Bad JSON:', line.substring(0, 80)); }
        }
    });
    socket.on('close', () => {
        console.log(`[TCP] Device disconnected: ${remote}`);
        if (stm32Socket === socket) stm32Socket = null;
        broadcast({ type: 'device_status', connected: false, ts: Date.now() });
    });
    socket.on('error', (err) => console.error(`[TCP] Error: ${err.message}`));
});

function calcRate() {
    const now = Date.now();
    messageRateBuffer = messageRateBuffer.filter(t => t > now - 5000);
    return +(messageRateBuffer.length / 5).toFixed(1);
}

function loadFirmwareMeta() {
    try { return JSON.parse(fs.readFileSync(path.join(FIRMWARE_DIR, 'version.json'))); }
    catch (_) { return null; }
}

setInterval(() => broadcast({ type: 'rate_tick', rate: calcRate(), server_uptime: Date.now() - serverStartTime }), 5000);

server.listen(HTTP_PORT, HOST, () => {
    console.log(`[HTTP] Dashboard → http://${HOST}:${HTTP_PORT}`);
    console.log(`[WS]  WebSocket  → ws://${HOST}:${HTTP_PORT}`);
});
tcpServer.listen(TCP_PORT, HOST, () => {
    console.log(`[TCP]  Device listener → ${HOST}:${TCP_PORT}`);
});
SERVERJS

# ── public/index.html ─────────────────────────────────────────
cat > "$APP_DIR/public/index.html" << 'INDEXHTML'
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>PredictivEdge — MPU-6050 DSP Monitor</title>
  <link rel="preconnect" href="https://fonts.googleapis.com" />
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700;800&family=JetBrains+Mono:wght@400;600&display=swap" rel="stylesheet" />
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <link rel="stylesheet" href="style.css" />
</head>
<body>
<header class="header">
  <div class="header-left">
    <div class="brand">
      <svg class="brand-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/>
      </svg>
      <div>
        <div class="brand-name">PredictivEdge</div>
        <div class="brand-sub">MPU-6050 DSP Predictive Maintenance</div>
      </div>
    </div>
  </div>
  <div class="header-right">
    <div class="conn-badge" id="device-badge">
      <span class="dot" id="device-dot"></span>
      <span id="device-label">Device Offline</span>
    </div>
    <div class="conn-badge" id="ws-badge">
      <span class="dot" id="ws-dot"></span>
      <span id="ws-label">Server Disconnected</span>
    </div>
    <div class="rate-pill"><span id="msg-rate">0.0</span> msg/s</div>
    <div class="rate-pill">&#8593; <span id="uptime">0s</span></div>
  </div>
</header>

<div class="kpi-row">
  <div class="kpi-card">
    <div class="kpi-label">Motor Health</div>
    <div class="kpi-value" id="kpi-health">&#8212;</div>
    <div class="kpi-sub" id="kpi-health-sub">Awaiting data</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Peak Vibration</div>
    <div class="kpi-value accent-blue" id="kpi-peak">&#8212; G</div>
    <div class="kpi-sub">Accel magnitude</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Fault Frequency</div>
    <div class="kpi-value accent-purple" id="kpi-freq">&#8212; Hz</div>
    <div class="kpi-sub">Dominant DSP bin</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Temperature</div>
    <div class="kpi-value accent-amber" id="kpi-temp">&#8212; &#176;C</div>
    <div class="kpi-sub">MPU-6050 die</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Total Faults</div>
    <div class="kpi-value accent-red" id="kpi-faults">0</div>
    <div class="kpi-sub">Session events</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Last Update</div>
    <div class="kpi-value mono" id="kpi-ts">&#8212;</div>
    <div class="kpi-sub">Timestamp</div>
  </div>
</div>

<div class="main-grid">
  <section class="panel span-full">
    <div class="panel-header">
      <div class="panel-title"><span class="panel-icon">&#126;</span> IMU Acceleration &#8212; Live</div>
      <div class="panel-controls"><span class="badge-chip">500 Hz sensor</span></div>
    </div>
    <div class="chart-wrap" style="height:220px"><canvas id="accelChart"></canvas></div>
  </section>

  <section class="panel">
    <div class="panel-header">
      <div class="panel-title"><span class="panel-icon">&#9672;</span> Peak Vibration Trend</div>
    </div>
    <div class="chart-wrap" style="height:180px"><canvas id="peakChart"></canvas></div>
  </section>

  <section class="panel">
    <div class="panel-header">
      <div class="panel-title"><span class="panel-icon">&#11041;</span> DSP Analysis</div>
    </div>
    <div class="dsp-grid">
      <div class="gauge-wrap">
        <canvas id="gaugeChart" width="140" height="140"></canvas>
        <div class="gauge-label"><span id="gauge-val">0</span><small>Hz</small></div>
      </div>
      <div class="dsp-stats">
        <div class="dsp-row"><span class="dsp-key">FAULT BIN</span><span class="dsp-val mono" id="dsp-freq">&#8212;</span></div>
        <div class="dsp-row"><span class="dsp-key">MAGNITUDE</span><span class="dsp-val mono" id="dsp-peak">&#8212;</span></div>
        <div class="dsp-row"><span class="dsp-key">TEMP</span><span class="dsp-val mono" id="dsp-temp">&#8212;</span></div>
        <div class="dsp-row"><span class="dsp-key">STATUS</span><span class="dsp-val" id="dsp-status">&#8212;</span></div>
      </div>
    </div>
  </section>

  <section class="panel">
    <div class="panel-header">
      <div class="panel-title"><span class="panel-icon">&#9888;</span> Fault Event Log</div>
      <div class="panel-controls"><button class="ctrl-btn" id="clear-faults-btn">Clear</button></div>
    </div>
    <div class="table-scroll">
      <table class="fault-table">
        <thead><tr><th>Time</th><th>Peak (G)</th><th>Freq (Hz)</th></tr></thead>
        <tbody id="fault-tbody"><tr class="empty-row"><td colspan="3">No faults recorded</td></tr></tbody>
      </table>
    </div>
  </section>

  <section class="panel">
    <div class="panel-header">
      <div class="panel-title"><span class="panel-icon">&#8593;</span> Secure OTA Delivery</div>
    </div>
    <div class="upload-zone" id="drop-zone">
      <div class="upload-icon">&#8593;</div>
      <p>Drop <strong>MPU_6050DSP.bin</strong> here or click to browse</p>
      <span class="upload-hint">SHA-256 computed server-side</span>
      <input type="file" id="file-input" accept=".bin" />
    </div>
    <div class="progress-bar-wrap" id="upload-progress" style="display:none">
      <div class="progress-bar" id="upload-bar"></div>
    </div>
    <div class="firmware-info" id="firmware-info" style="display:none">
      <div class="fi-row"><span class="fi-key">SHA-256</span><span class="fi-val mono hash-val" id="fw-hash"></span></div>
      <div class="fi-row"><span class="fi-key">Size</span><span class="fi-val mono" id="fw-size"></span></div>
      <div class="fi-row"><span class="fi-key">Uploaded</span><span class="fi-val" id="fw-time"></span></div>
    </div>
    <div class="ota-actions">
      <button class="action-btn primary" id="ota-trigger-btn" disabled>&#9889; Push to Device</button>
      <button class="action-btn danger"  id="reboot-btn"      disabled>&#8634; Reboot Device</button>
    </div>
    <div class="ota-status-msg" id="ota-status-msg"></div>
  </section>
</div>
<script src="script.js"></script>
</body>
</html>
INDEXHTML

# ── public/script.js ──────────────────────────────────────────
cat > "$APP_DIR/public/script.js" << 'SCRIPTJS'
'use strict';
const deviceDot=document.getElementById('device-dot'),deviceLabel=document.getElementById('device-label'),deviceBadge=document.getElementById('device-badge'),wsDot=document.getElementById('ws-dot'),wsLabel=document.getElementById('ws-label'),msgRate=document.getElementById('msg-rate'),uptimeEl=document.getElementById('uptime');
const kpiHealth=document.getElementById('kpi-health'),kpiHealthSub=document.getElementById('kpi-health-sub'),kpiPeak=document.getElementById('kpi-peak'),kpiFreq=document.getElementById('kpi-freq'),kpiTemp=document.getElementById('kpi-temp'),kpiFaults=document.getElementById('kpi-faults'),kpiTs=document.getElementById('kpi-ts');
const dspFreq=document.getElementById('dsp-freq'),dspPeak=document.getElementById('dsp-peak'),dspTemp=document.getElementById('dsp-temp'),dspStatus=document.getElementById('dsp-status'),gaugeVal=document.getElementById('gauge-val');
const faultTbody=document.getElementById('fault-tbody'),clearFaultsBtn=document.getElementById('clear-faults-btn');
const dropZone=document.getElementById('drop-zone'),fileInput=document.getElementById('file-input'),firmwareInfo=document.getElementById('firmware-info'),fwHash=document.getElementById('fw-hash'),fwSize=document.getElementById('fw-size'),fwTime=document.getElementById('fw-time'),uploadProgress=document.getElementById('upload-progress'),uploadBar=document.getElementById('upload-bar'),otaTriggerBtn=document.getElementById('ota-trigger-btn'),rebootBtn=document.getElementById('reboot-btn'),otaStatusMsg=document.getElementById('ota-status-msg');
const MAX_POINTS=120,FREQ_MAX=200;
Chart.defaults.color='#94a3b8';Chart.defaults.font.family='Inter, sans-serif';Chart.defaults.font.size=11;Chart.defaults.animation=false;
const gridColor='rgba(255,255,255,0.06)',tickColor='#64748b';
function ea(n,f=0){return Array(n).fill(f);}
const accelChart=new Chart(document.getElementById('accelChart').getContext('2d'),{type:'line',data:{labels:ea(MAX_POINTS,''),datasets:[{label:'Ax (g)',borderColor:'#f87171',data:ea(MAX_POINTS),borderWidth:1.5,tension:0.2,pointRadius:0,fill:false},{label:'Ay (g)',borderColor:'#34d399',data:ea(MAX_POINTS),borderWidth:1.5,tension:0.2,pointRadius:0,fill:false},{label:'Az (g)',borderColor:'#60a5fa',data:ea(MAX_POINTS,1),borderWidth:1.5,tension:0.2,pointRadius:0,fill:false}]},options:{responsive:true,maintainAspectRatio:false,interaction:{intersect:false,mode:'index'},scales:{x:{display:false},y:{min:-2,max:2,grid:{color:gridColor},ticks:{color:tickColor,stepSize:1}}},plugins:{legend:{labels:{color:'#cbd5e1',boxWidth:12,padding:16}},tooltip:{backgroundColor:'#1e293b',borderColor:'#334155',borderWidth:1}}}});
const peakChart=new Chart(document.getElementById('peakChart').getContext('2d'),{type:'line',data:{labels:ea(MAX_POINTS,''),datasets:[{label:'Peak (g)',borderColor:'#a78bfa',backgroundColor:'rgba(167,139,250,0.08)',data:ea(MAX_POINTS),borderWidth:1.5,tension:0.3,pointRadius:0,fill:true}]},options:{responsive:true,maintainAspectRatio:false,scales:{x:{display:false},y:{min:0,grid:{color:gridColor},ticks:{color:tickColor}}},plugins:{legend:{display:false},tooltip:{backgroundColor:'#1e293b',borderColor:'#334155',borderWidth:1}}}});
const gaugeChart=new Chart(document.getElementById('gaugeChart').getContext('2d'),{type:'doughnut',data:{datasets:[{data:[0,FREQ_MAX],backgroundColor:['#3b82f6','rgba(255,255,255,0.04)'],borderWidth:0,circumference:240,rotation:-120}]},options:{responsive:false,cutout:'72%',plugins:{legend:{display:false},tooltip:{enabled:false}}}});
function updateGauge(hz){const v=Math.min(hz,FREQ_MAX);gaugeChart.data.datasets[0].data=[v,FREQ_MAX-v];const r=v/FREQ_MAX;gaugeChart.data.datasets[0].backgroundColor[0]=r>0.7?'#f87171':r>0.4?'#fbbf24':'#3b82f6';gaugeChart.update();gaugeVal.textContent=hz.toFixed(1);}
function push(chart,i,val){chart.data.datasets[i].data.shift();chart.data.datasets[i].data.push(val);}
let serverStart=null,sessionFaults=0;
function formatUptime(ms){const s=Math.floor(ms/1000);if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m '+s%60+'s';return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';}
function tickUptime(){if(serverStart)uptimeEl.textContent=formatUptime(Date.now()-serverStart);}
setInterval(tickUptime,1000);
function fmtTime(ts){return new Date(ts).toLocaleTimeString([],{hour12:false});}
function appendFaultRow(ev){const empty=faultTbody.querySelector('.empty-row');if(empty)empty.remove();const tr=document.createElement('tr');tr.className='fault-row new-fault';tr.innerHTML=`<td class="mono">${fmtTime(ev.ts)}</td><td class="accent-red">${ev.peak.toFixed(3)}</td><td class="accent-amber">${ev.freq.toFixed(1)}</td>`;faultTbody.prepend(tr);setTimeout(()=>tr.classList.remove('new-fault'),1000);const rows=faultTbody.querySelectorAll('tr:not(.empty-row)');if(rows.length>50)rows[rows.length-1].remove();}
function populateFaultLog(events){faultTbody.innerHTML='';if(!events||!events.length){faultTbody.innerHTML='<tr class="empty-row"><td colspan="3">No faults recorded</td></tr>';return;}[...events].reverse().forEach(e=>appendFaultRow(e));sessionFaults=events.length;kpiFaults.textContent=sessionFaults;}
clearFaultsBtn.addEventListener('click',()=>{sessionFaults=0;kpiFaults.textContent='0';faultTbody.innerHTML='<tr class="empty-row"><td colspan="3">No faults recorded</td></tr>';});
function onTelemetry(d){if(d.ax!==undefined){push(accelChart,0,d.ax);push(accelChart,1,d.ay);push(accelChart,2,d.az);accelChart.update('none');}if(d.peak!==undefined){push(peakChart,0,d.peak);peakChart.update('none');}if(d.peak!==undefined)kpiPeak.textContent=d.peak.toFixed(3)+' G';if(d.freq!==undefined)kpiFreq.textContent=d.freq.toFixed(1)+' Hz';if(d.temp!==undefined)kpiTemp.textContent=d.temp.toFixed(1)+' \u00b0C';if(d.ts!==undefined)kpiTs.textContent=fmtTime(d.ts);if(d.rate!==undefined)msgRate.textContent=d.rate;if(d.freq!==undefined){dspFreq.textContent=d.freq.toFixed(1)+' Hz';updateGauge(d.freq);}if(d.peak!==undefined)dspPeak.textContent=d.peak.toFixed(3)+' G';if(d.temp!==undefined)dspTemp.textContent=d.temp.toFixed(1)+' \u00b0C';if(d.fault!==undefined){const ok=d.fault===0;kpiHealth.textContent=ok?'HEALTHY':'FAULT';kpiHealth.className='kpi-value '+(ok?'accent-green':'accent-red blink');kpiHealthSub.textContent=ok?'All bands nominal':'Anomaly detected';dspStatus.textContent=ok?'\u2713 Nominal':'\u26a0 FAULT';dspStatus.className='dsp-val '+(ok?'accent-green':'accent-red');}}
function replayHistory(history){if(!history||!history.length)return;const slice=history.slice(-MAX_POINTS);slice.forEach((d,i)=>{const idx=MAX_POINTS-slice.length+i;if(d.ax!==undefined){accelChart.data.datasets[0].data[idx]=d.ax;accelChart.data.datasets[1].data[idx]=d.ay;accelChart.data.datasets[2].data[idx]=d.az;}if(d.peak!==undefined)peakChart.data.datasets[0].data[idx]=d.peak;});accelChart.update('none');peakChart.update('none');const last=slice[slice.length-1];if(last)onTelemetry(last);}
function setDeviceStatus(connected,remote){deviceDot.className='dot '+(connected?'dot-online':'dot-offline');deviceLabel.textContent=connected?'Device Online'+(remote?' \u00b7 '+remote.split(':')[0]:''):'Device Offline';deviceBadge.className='conn-badge '+(connected?'badge-online':'badge-offline');otaTriggerBtn.disabled=!connected;rebootBtn.disabled=!connected;}
function showFirmware(fw){if(!fw)return;firmwareInfo.style.display='block';fwHash.textContent=fw.sha256;fwSize.textContent=fw.size.toLocaleString()+' bytes';fwTime.textContent=fw.uploaded_at?new Date(fw.uploaded_at).toLocaleString():'--';}
function setOtaMsg(msg,type='info'){otaStatusMsg.textContent=msg;otaStatusMsg.className='ota-status-msg ota-msg-'+type;if(msg)setTimeout(()=>{otaStatusMsg.textContent='';otaStatusMsg.className='ota-status-msg';},5000);}
dropZone.addEventListener('click',()=>fileInput.click());
fileInput.addEventListener('change',e=>{if(e.target.files[0])doUpload(e.target.files[0]);});
['dragenter','dragover','dragleave','drop'].forEach(ev=>dropZone.addEventListener(ev,e=>{e.preventDefault();e.stopPropagation();}));
['dragenter','dragover'].forEach(ev=>dropZone.addEventListener(ev,()=>dropZone.classList.add('drag-over')));
['dragleave','drop'].forEach(ev=>dropZone.addEventListener(ev,()=>dropZone.classList.remove('drag-over')));
dropZone.addEventListener('drop',e=>{if(e.dataTransfer.files[0])doUpload(e.dataTransfer.files[0]);});
function doUpload(file){if(!file.name.endsWith('.bin')){setOtaMsg('Only .bin firmware files accepted.','error');return;}uploadProgress.style.display='block';uploadBar.style.width='0%';setOtaMsg('Uploading and hashing firmware\u2026','info');let w=0;const tick=setInterval(()=>{w=Math.min(w+2,90);uploadBar.style.width=w+'%';},60);const fd=new FormData();fd.append('firmware',file);fetch('/api/upload',{method:'POST',body:fd}).then(r=>r.json()).then(data=>{clearInterval(tick);uploadBar.style.width='100%';if(data.success){setOtaMsg('Firmware stored \u2014 '+data.size.toLocaleString()+' bytes hashed.','success');showFirmware(data);setTimeout(()=>{uploadProgress.style.display='none';uploadBar.style.width='0%';},1500);}else{setOtaMsg('Upload failed: '+(data.error||'unknown'),'error');uploadProgress.style.display='none';}}).catch(err=>{clearInterval(tick);uploadProgress.style.display='none';setOtaMsg('Upload error: '+err.message,'error');});}
otaTriggerBtn.addEventListener('click',()=>{if(!confirm('Push OTA update to device now?'))return;otaTriggerBtn.disabled=true;fetch('/api/ota/trigger',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.success)setOtaMsg('OTA triggered \u2014 device will download and apply update.','success');else setOtaMsg('Trigger failed: '+(d.error||'unknown'),'error');}).catch(e=>setOtaMsg('Error: '+e.message,'error')).finally(()=>{setTimeout(()=>{otaTriggerBtn.disabled=false;},3000);});});
rebootBtn.addEventListener('click',()=>{if(!confirm('Reboot device into bootloader?'))return;fetch('/api/device/reboot',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.success)setOtaMsg('Reboot command sent.','success');else setOtaMsg('Reboot failed: '+(d.error||'unknown'),'error');}).catch(e=>setOtaMsg('Error: '+e.message,'error'));});
function connect(){const ws=new WebSocket('ws://'+window.location.host);ws.onopen=()=>{wsDot.className='dot dot-online';wsLabel.textContent='Server Connected';};ws.onclose=()=>{wsDot.className='dot dot-offline';wsLabel.textContent='Reconnecting\u2026';setTimeout(connect,2000);};ws.onerror=()=>ws.close();ws.onmessage=({data})=>{let msg;try{msg=JSON.parse(data);}catch{return;}switch(msg.type){case 'init':serverStart=Date.now()-(msg.server_uptime||0);tickUptime();setDeviceStatus(msg.device_connected,msg.connected_since);replayHistory(msg.history);populateFaultLog(msg.fault_log);showFirmware(msg.firmware);break;case 'telemetry':onTelemetry(msg);break;case 'fault_event':sessionFaults++;kpiFaults.textContent=sessionFaults;appendFaultRow(msg);break;case 'device_status':setDeviceStatus(msg.connected,msg.remote);break;case 'ota_update':showFirmware(msg);setOtaMsg('New firmware deployed.','success');break;case 'ota_triggered':setOtaMsg('OTA push acknowledged.','success');break;case 'device_reboot':setOtaMsg('Device reboot initiated.','info');break;case 'rate_tick':msgRate.textContent=msg.rate;if(msg.server_uptime)uptimeEl.textContent=formatUptime(msg.server_uptime);break;}};}
connect();
SCRIPTJS

# ── public/style.css ──────────────────────────────────────────
cat > "$APP_DIR/public/style.css" << 'STYLECSS'
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#080c14;--surface-1:#0f1623;--surface-2:#151d2e;--surface-3:#1c2640;--border:rgba(255,255,255,0.07);--border-hi:rgba(255,255,255,0.13);--txt-primary:#e2e8f0;--txt-secondary:#64748b;--txt-muted:#334155;--blue:#3b82f6;--green:#22c55e;--amber:#f59e0b;--red:#ef4444;--purple:#a78bfa;--radius:12px;--radius-sm:8px;--gap:1.25rem;--hdr-h:60px}
html{font-size:14px}
body{font-family:'Inter',system-ui,sans-serif;background:var(--bg);background-image:radial-gradient(ellipse 60% 40% at 10% 0%,rgba(59,130,246,0.08) 0%,transparent 60%),radial-gradient(ellipse 50% 30% at 90% 100%,rgba(167,139,250,0.06) 0%,transparent 60%);color:var(--txt-primary);min-height:100vh;overflow-x:hidden}
.mono{font-family:'JetBrains Mono',monospace}
.accent-blue{color:#3b82f6}.accent-green{color:#22c55e}.accent-amber{color:#f59e0b}.accent-red{color:#ef4444}.accent-purple{color:#a78bfa}
.header{position:sticky;top:0;z-index:100;height:var(--hdr-h);display:flex;align-items:center;justify-content:space-between;padding:0 1.5rem;background:rgba(8,12,20,0.85);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);border-bottom:1px solid var(--border)}
.brand{display:flex;align-items:center;gap:.75rem}
.brand-icon{width:28px;height:28px;color:#3b82f6;flex-shrink:0}
.brand-name{font-size:1rem;font-weight:800;letter-spacing:-.02em;background:linear-gradient(90deg,#60a5fa,#a78bfa);-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.brand-sub{font-size:.7rem;color:var(--txt-secondary);margin-top:1px}
.header-right{display:flex;align-items:center;gap:.75rem;flex-wrap:wrap}
.conn-badge{display:flex;align-items:center;gap:.4rem;padding:.3rem .7rem;border-radius:999px;font-size:.72rem;font-weight:500;border:1px solid var(--border);background:var(--surface-1);transition:border-color .3s}
.badge-online{border-color:rgba(34,197,94,0.3)}.badge-offline{border-color:rgba(239,68,68,0.3)}
.dot{width:8px;height:8px;border-radius:50%;flex-shrink:0}
.dot-online{background:#22c55e;box-shadow:0 0 8px #22c55e}.dot-offline{background:#ef4444;box-shadow:0 0 8px #ef4444}
.rate-pill{padding:.3rem .7rem;border-radius:999px;font-size:.72rem;font-weight:600;font-family:'JetBrains Mono',monospace;background:var(--surface-2);border:1px solid var(--border);color:var(--txt-secondary);white-space:nowrap}
.kpi-row{display:grid;grid-template-columns:repeat(6,1fr);gap:var(--gap);padding:var(--gap) 1.5rem}
@media(max-width:1100px){.kpi-row{grid-template-columns:repeat(3,1fr)}}
@media(max-width:640px){.kpi-row{grid-template-columns:repeat(2,1fr)}}
.kpi-card{background:var(--surface-1);border:1px solid var(--border);border-radius:var(--radius);padding:1rem 1.1rem;transition:border-color .3s,transform .2s}
.kpi-card:hover{border-color:var(--border-hi);transform:translateY(-1px)}
.kpi-label{font-size:.65rem;font-weight:600;text-transform:uppercase;letter-spacing:.1em;color:var(--txt-secondary);margin-bottom:.4rem}
.kpi-value{font-size:1.4rem;font-weight:700;letter-spacing:-.03em;line-height:1;margin-bottom:.3rem}
.kpi-value.mono{font-size:1rem}
.kpi-sub{font-size:.65rem;color:var(--txt-muted)}
@keyframes blink-pulse{0%,100%{opacity:1}50%{opacity:.5}}
.blink{animation:blink-pulse 1s ease infinite}
.main-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:var(--gap);padding:0 1.5rem 2rem}
@media(max-width:1024px){.main-grid{grid-template-columns:1fr 1fr}}
@media(max-width:640px){.main-grid{grid-template-columns:1fr}}
.span-full{grid-column:1/-1}
.panel{background:var(--surface-1);border:1px solid var(--border);border-radius:var(--radius);padding:1.1rem 1.25rem;display:flex;flex-direction:column;gap:1rem;transition:border-color .3s}
.panel:hover{border-color:var(--border-hi)}
.panel-header{display:flex;align-items:center;justify-content:space-between;gap:.5rem}
.panel-title{font-size:.8rem;font-weight:600;text-transform:uppercase;letter-spacing:.08em;color:var(--txt-secondary);display:flex;align-items:center;gap:.4rem}
.panel-controls{display:flex;align-items:center;gap:.5rem}
.badge-chip{font-size:.65rem;font-weight:600;padding:.2rem .5rem;border-radius:999px;background:rgba(59,130,246,0.12);color:#3b82f6;border:1px solid rgba(59,130,246,0.25)}
.ctrl-btn{font-size:.7rem;font-weight:600;padding:.25rem .6rem;border-radius:var(--radius-sm);border:1px solid var(--border);background:transparent;color:var(--txt-secondary);cursor:pointer;transition:all .2s}
.ctrl-btn:hover{border-color:#ef4444;color:#ef4444}
.chart-wrap{position:relative;width:100%}
.dsp-grid{display:flex;gap:1rem;align-items:center}
.gauge-wrap{position:relative;flex-shrink:0;width:120px;height:120px;display:flex;align-items:center;justify-content:center}
.gauge-label{position:absolute;text-align:center;font-family:'JetBrains Mono',monospace;font-size:1.1rem;font-weight:700;color:var(--txt-primary);pointer-events:none}
.gauge-label small{display:block;font-size:.6rem;color:var(--txt-secondary);margin-top:2px}
.dsp-stats{flex:1;display:flex;flex-direction:column;gap:.6rem}
.dsp-row{display:flex;justify-content:space-between;align-items:center;padding:.4rem .6rem;background:var(--surface-2);border-radius:var(--radius-sm);border:1px solid var(--border)}
.dsp-key{font-size:.62rem;font-weight:600;text-transform:uppercase;letter-spacing:.08em;color:var(--txt-secondary)}
.dsp-val{font-size:.8rem;font-weight:600;font-family:'JetBrains Mono',monospace}
.table-scroll{overflow-y:auto;max-height:220px;border-radius:var(--radius-sm);border:1px solid var(--border)}
.table-scroll::-webkit-scrollbar{width:4px}.table-scroll::-webkit-scrollbar-track{background:transparent}.table-scroll::-webkit-scrollbar-thumb{background:var(--surface-3);border-radius:2px}
.fault-table{width:100%;border-collapse:collapse;font-size:.75rem}
.fault-table thead{position:sticky;top:0;background:var(--surface-2);z-index:1}
.fault-table th{padding:.5rem .75rem;text-align:left;font-size:.65rem;font-weight:600;text-transform:uppercase;letter-spacing:.08em;color:var(--txt-secondary);border-bottom:1px solid var(--border)}
.fault-table td{padding:.4rem .75rem;border-bottom:1px solid rgba(255,255,255,0.04);font-family:'JetBrains Mono',monospace}
.empty-row td{text-align:center;color:var(--txt-muted);padding:1.5rem;font-family:'Inter',sans-serif;font-size:.8rem}
.fault-row{transition:background .3s}.fault-row:hover{background:var(--surface-2)}
@keyframes fault-flash{0%{background:rgba(239,68,68,0.15)}100%{background:transparent}}
.new-fault{animation:fault-flash 1s ease}
.upload-zone{position:relative;border:2px dashed rgba(59,130,246,0.35);border-radius:var(--radius);padding:1.5rem 1rem;text-align:center;cursor:pointer;background:rgba(59,130,246,0.04);transition:all .25s}
.upload-zone:hover,.upload-zone.drag-over{border-color:#3b82f6;background:rgba(59,130,246,0.09);box-shadow:0 0 20px rgba(59,130,246,0.12)}
.upload-zone input[type="file"]{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}
.upload-icon{font-size:1.8rem;margin-bottom:.5rem;color:#3b82f6;opacity:.7}
.upload-zone p{font-size:.8rem;color:var(--txt-secondary);margin-bottom:.25rem}
.upload-hint{font-size:.65rem;color:var(--txt-muted)}
.progress-bar-wrap{height:4px;background:var(--surface-3);border-radius:2px;overflow:hidden}
.progress-bar{height:100%;background:linear-gradient(90deg,#3b82f6,#a78bfa);border-radius:2px;width:0%;transition:width .1s linear}
.firmware-info{background:var(--surface-2);border:1px solid var(--border);border-radius:var(--radius-sm);padding:.75rem;display:flex;flex-direction:column;gap:.4rem}
.fi-row{display:flex;gap:.5rem;align-items:flex-start;font-size:.72rem}
.fi-key{flex-shrink:0;width:60px;font-weight:600;text-transform:uppercase;font-size:.62rem;letter-spacing:.06em;color:var(--txt-secondary);padding-top:1px}
.fi-val{color:var(--txt-primary)}
.hash-val{font-size:.62rem;word-break:break-all;color:#a78bfa}
.ota-actions{display:flex;gap:.5rem}
.action-btn{flex:1;padding:.55rem .75rem;border-radius:var(--radius-sm);border:none;font-size:.75rem;font-weight:600;cursor:pointer;transition:all .2s;font-family:'Inter',sans-serif}
.action-btn:disabled{opacity:.35;cursor:not-allowed}
.action-btn.primary{background:#3b82f6;color:#fff}
.action-btn.primary:hover:not(:disabled){background:#2563eb;box-shadow:0 0 16px rgba(59,130,246,0.4)}
.action-btn.danger{background:rgba(239,68,68,0.12);color:#ef4444;border:1px solid rgba(239,68,68,0.3)}
.action-btn.danger:hover:not(:disabled){background:rgba(239,68,68,0.2)}
.ota-status-msg{font-size:.72rem;min-height:1rem;transition:color .3s}
.ota-msg-success{color:#22c55e}.ota-msg-error{color:#ef4444}.ota-msg-info{color:var(--txt-secondary)}
::-webkit-scrollbar{width:6px;height:6px}::-webkit-scrollbar-track{background:transparent}::-webkit-scrollbar-thumb{background:var(--surface-3);border-radius:3px}
STYLECSS

# ── 6. Install Node dependencies ──────────────────────────────
echo "[6/8] Installing Node.js dependencies..."
cd "$APP_DIR"
npm install --omit=dev --quiet

# ── 7. Start with PM2 ─────────────────────────────────────────
echo "[7/8] Starting with PM2..."
pm2 delete predictivEdge 2>/dev/null || true
pm2 start "$APP_DIR/ecosystem.config.js"
pm2 save
env PATH="$PATH:/usr/bin" pm2 startup systemd -u root --hp /root 2>/dev/null | grep "sudo\|systemctl" | bash || true

# ── 8. nginx + firewall ───────────────────────────────────────
echo "[8/8] Configuring nginx and firewall..."

cat > /etc/nginx/sites-available/predictivEdge << 'NGINXCONF'
limit_req_zone $binary_remote_addr zone=upload_limit:10m rate=2r/s;
server {
    listen 80;
    server_name _;
    client_max_body_size 2M;
    add_header X-Frame-Options "SAMEORIGIN" always;
    add_header X-Content-Type-Options "nosniff" always;

    location /ws {
        proxy_pass         http://127.0.0.1:3000;
        proxy_http_version 1.1;
        proxy_set_header   Upgrade $http_upgrade;
        proxy_set_header   Connection "upgrade";
        proxy_set_header   Host $host;
        proxy_read_timeout 3600s;
    }
    location /api/upload {
        limit_req zone=upload_limit burst=3 nodelay;
        proxy_pass http://127.0.0.1:3000;
        proxy_set_header Host $host;
    }
    location / {
        proxy_pass       http://127.0.0.1:3000;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
NGINXCONF

ln -sf /etc/nginx/sites-available/predictivEdge /etc/nginx/sites-enabled/predictivEdge
rm -f /etc/nginx/sites-enabled/default
nginx -t && systemctl enable nginx --quiet && systemctl reload nginx

# Firewall
ufw allow 22/tcp   comment "SSH"        2>/dev/null || true
ufw allow 80/tcp   comment "HTTP"       2>/dev/null || true
ufw allow 3001/tcp comment "Device TCP" 2>/dev/null || true
ufw --force enable 2>/dev/null || true

# ── Done ──────────────────────────────────────────────────────
PUBLIC_IP=$(curl -s ifconfig.me 2>/dev/null || echo "YOUR_IP")
echo ""
echo "╔══════════════════════════════════════════╗"
echo "║  Install complete!                        ║"
echo "║                                           ║"
echo "║  Dashboard  →  http://$PUBLIC_IP          ║"
echo "║  Device TCP →  $PUBLIC_IP:3001            ║"
echo "║                                           ║"
echo "║  pm2 logs predictivEdge   (live logs)     ║"
echo "║  pm2 status               (health)        ║"
echo "║  pm2 restart predictivEdge               ║"
echo "╚══════════════════════════════════════════╝"

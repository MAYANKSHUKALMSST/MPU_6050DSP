'use strict';

// ─── Auth ──────────────────────────────────────────────────────
const loginOverlay = document.getElementById('login-overlay');
const loginForm    = document.getElementById('login-form');
const loginUser    = document.getElementById('login-user');
const loginPass    = document.getElementById('login-pass');
const loginError   = document.getElementById('login-error');
const loginSubmit  = document.getElementById('login-submit');
const logoutBtn    = document.getElementById('logout-btn');

function getToken()        { return localStorage.getItem('pe_token'); }
function setToken(t)       { localStorage.setItem('pe_token', t); }
function clearToken()      { localStorage.removeItem('pe_token'); }

function authHeaders() {
    return { 'Content-Type': 'application/json', 'X-Auth-Token': getToken() || '' };
}

function showLogin(msg) {
    loginError.style.display = msg ? 'block' : 'none';
    loginError.textContent   = msg || '';
    loginOverlay.style.display = 'flex';
}

function hideLogin() {
    loginOverlay.style.display = 'none';
}

loginForm.addEventListener('submit', async (e) => {
    e.preventDefault();
    loginSubmit.disabled = true;
    loginSubmit.textContent = 'Signing in…';
    try {
        const r = await fetch('/api/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ username: loginUser.value.trim(), password: loginPass.value })
        });
        const d = await r.json();
        if (r.ok && d.token) {
            setToken(d.token);
            hideLogin();
            connect();        // start WS after login
        } else {
            showLogin(d.error || 'Login failed');
        }
    } catch (err) {
        showLogin('Server unreachable');
    } finally {
        loginSubmit.disabled = false;
        loginSubmit.textContent = 'Sign In';
    }
});

logoutBtn.addEventListener('click', () => {
    clearToken();
    showLogin();
    loginPass.value = '';
});

// Check token on page load — show login if missing or server rejects it
(async () => {
    const token = getToken();
    if (!token) { showLogin(); return; }
    try {
        const r = await fetch('/api/status', { headers: { 'X-Auth-Token': token } });
        if (r.status === 401) { clearToken(); showLogin(); return; }
        hideLogin();
        connect();
    } catch { showLogin(); }
})();

// ─── DOM refs ──────────────────────────────────────────────────
const deviceDot      = document.getElementById('device-dot');
const deviceLabel    = document.getElementById('device-label');
const deviceBadge    = document.getElementById('device-badge');
const wsDot          = document.getElementById('ws-dot');
const wsLabel        = document.getElementById('ws-label');
const msgRate        = document.getElementById('msg-rate');
const uptimeEl       = document.getElementById('uptime');

const kpiHealth      = document.getElementById('kpi-health');
const kpiHealthSub   = document.getElementById('kpi-health-sub');
const kpiPeak        = document.getElementById('kpi-peak');
const kpiFreq        = document.getElementById('kpi-freq');
const kpiTemp        = document.getElementById('kpi-temp');
const kpiFaults      = document.getElementById('kpi-faults');
const kpiTs          = document.getElementById('kpi-ts');

const dspFreq        = document.getElementById('dsp-freq');
const dspPeak        = document.getElementById('dsp-peak');
const dspTemp        = document.getElementById('dsp-temp');
const dspStatus      = document.getElementById('dsp-status');
const gaugeVal       = document.getElementById('gauge-val');

const faultTbody     = document.getElementById('fault-tbody');
const clearFaultsBtn = document.getElementById('clear-faults-btn');

const devlogTbody    = document.getElementById('devlog-tbody');
const clearDevlogBtn = document.getElementById('clear-devlog-btn');
const flashToast     = document.getElementById('flash-toast');
const flashToastIcon = document.getElementById('flash-toast-icon');
const flashToastMsg  = document.getElementById('flash-toast-msg');

// ─── Email Alert DOM refs ───────────────────────────────────────
const emailRecipient  = document.getElementById('email-recipient');
const emailSender     = document.getElementById('email-sender');
const emailPassword   = document.getElementById('email-password');
const emailCooldown   = document.getElementById('email-cooldown');
const emailSaveBtn    = document.getElementById('email-save-btn');
const emailTestBtn    = document.getElementById('email-test-btn');
const emailStatusMsg  = document.getElementById('email-status-msg');
const emailStatusChip = document.getElementById('email-status-chip');

const dropZone       = document.getElementById('drop-zone');
const fileInput      = document.getElementById('file-input');
const firmwareInfo   = document.getElementById('firmware-info');
const fwHash         = document.getElementById('fw-hash');
const fwSize         = document.getElementById('fw-size');
const fwTime         = document.getElementById('fw-time');
const uploadProgress = document.getElementById('upload-progress');
const uploadBar      = document.getElementById('upload-bar');
const otaTriggerBtn  = document.getElementById('ota-trigger-btn');
const rebootBtn      = document.getElementById('reboot-btn');
const otaStatusMsg   = document.getElementById('ota-status-msg');

// ─── Constants ─────────────────────────────────────────────────
let MAX_POINTS   = 120;   // chart rolling window
const FREQ_MAX     = 200;   // Hz ceiling for gauge

// ─── Chart defaults ────────────────────────────────────────────
Chart.defaults.color          = '#94a3b8';
Chart.defaults.font.family    = 'Inter, sans-serif';
Chart.defaults.font.size      = 11;
Chart.defaults.animation      = false;

const gridColor  = 'rgba(255,255,255,0.06)';
const tickColor  = '#64748b';

function emptyArray(n, fill = 0) { return Array(n).fill(fill); }

// ─── Accel Chart ───────────────────────────────────────────────
const accelCtx = document.getElementById('accelChart').getContext('2d');
const accelChart = new Chart(accelCtx, {
    type: 'line',
    data: {
        labels: emptyArray(MAX_POINTS, ''),
        datasets: [
            { label: 'Ax (g)', borderColor: '#f87171', data: emptyArray(MAX_POINTS), borderWidth: 1.5, tension: 0.2, pointRadius: 0, fill: false },
            { label: 'Ay (g)', borderColor: '#34d399', data: emptyArray(MAX_POINTS), borderWidth: 1.5, tension: 0.2, pointRadius: 0, fill: false },
            { label: 'Az (g)', borderColor: '#60a5fa', data: emptyArray(MAX_POINTS, 1), borderWidth: 1.5, tension: 0.2, pointRadius: 0, fill: false }
        ]
    },
    options: {
        responsive: true, maintainAspectRatio: false,
        interaction: { intersect: false, mode: 'index' },
        scales: {
            x: { display: false },
            y: { min: -2, max: 2, grid: { color: gridColor }, ticks: { color: tickColor, stepSize: 1 } }
        },
        plugins: {
            legend: { labels: { color: '#cbd5e1', boxWidth: 12, padding: 16 } },
            tooltip: { backgroundColor: '#1e293b', borderColor: '#334155', borderWidth: 1 }
        }
    }
});

// ─── Peak Trend Chart ──────────────────────────────────────────
const peakCtx = document.getElementById('peakChart').getContext('2d');
const peakChart = new Chart(peakCtx, {
    type: 'line',
    data: {
        labels: emptyArray(MAX_POINTS, ''),
        datasets: [
            {
                label: 'Peak (g)',
                borderColor: '#a78bfa',
                backgroundColor: 'rgba(167,139,250,0.08)',
                data: emptyArray(MAX_POINTS),
                borderWidth: 1.5, tension: 0.3, pointRadius: 0, fill: true
            }
        ]
    },
    options: {
        responsive: true, maintainAspectRatio: false,
        scales: {
            x: { display: false },
            y: { min: 0, grid: { color: gridColor }, ticks: { color: tickColor } }
        },
        plugins: {
            legend: { display: false },
            tooltip: { backgroundColor: '#1e293b', borderColor: '#334155', borderWidth: 1 }
        }
    }
});

// ─── Condition Indicators Chart (CF & Kurtosis) ────────────────
const condCtx = document.getElementById('condChart').getContext('2d');
const condChart = new Chart(condCtx, {
    type: 'line',
    data: {
        labels: emptyArray(MAX_POINTS, ''),
        datasets: [
            {
                label: 'Crest Factor',
                borderColor: '#fbbf24',
                backgroundColor: 'rgba(251,191,36,0.08)',
                data: emptyArray(MAX_POINTS, 1),   // 1.0 = neutral baseline
                borderWidth: 2, tension: 0.3, pointRadius: 0, fill: false,
                yAxisID: 'yCF'
            },
            {
                label: 'Kurtosis',
                borderColor: '#34d399',
                backgroundColor: 'rgba(52,211,153,0.08)',
                data: emptyArray(MAX_POINTS, 3),   // 3.0 = Gaussian baseline
                borderWidth: 2, tension: 0.3, pointRadius: 0, fill: false,
                yAxisID: 'yKurt'
            }
        ]
    },
    options: {
        responsive: true, maintainAspectRatio: false,
        interaction: { intersect: false, mode: 'index' },
        scales: {
            x: { display: false },
            yCF: {
                type: 'linear', position: 'left',
                min: 0, suggestedMax: 6,      // CF baseline ~1–3; faults push to 5+
                grid: { color: gridColor },
                ticks: { color: '#fbbf24', font: { size: 10 }, maxTicksLimit: 6 },
                title: { display: true, text: 'Crest Factor', color: '#fbbf24', font: { size: 10 } }
            },
            yKurt: {
                type: 'linear', position: 'right',
                min: 0, suggestedMax: 12,     // Kurtosis baseline ~3; faults push to 10+
                grid: { drawOnChartArea: false },
                ticks: { color: '#34d399', font: { size: 10 }, maxTicksLimit: 6 },
                title: { display: true, text: 'Kurtosis', color: '#34d399', font: { size: 10 } }
            }
        },
        plugins: {
            legend: { labels: { color: '#cbd5e1', boxWidth: 12, padding: 16 } },
            tooltip: {
                backgroundColor: '#1e293b', borderColor: '#334155', borderWidth: 1,
                callbacks: {
                    label: ctx => {
                        const unit = ctx.datasetIndex === 0 ? '' : '';
                        return ` ${ctx.dataset.label}: ${ctx.parsed.y.toFixed(2)}${unit}`;
                    }
                }
            }
        }
    }
});

// ─── Frequency Gauge (doughnut) ────────────────────────────────
const gaugeCtx = document.getElementById('gaugeChart').getContext('2d');
const gaugeChart = new Chart(gaugeCtx, {
    type: 'doughnut',
    data: {
        datasets: [{
            data: [0, FREQ_MAX],
            backgroundColor: ['#3b82f6', 'rgba(255,255,255,0.04)'],
            borderWidth: 0,
            circumference: 240,
            rotation: -120
        }]
    },
    options: {
        responsive: false, cutout: '72%',
        plugins: { legend: { display: false }, tooltip: { enabled: false } }
    }
});

function updateGauge(hz) {
    const val = Math.min(hz, FREQ_MAX);
    gaugeChart.data.datasets[0].data = [val, FREQ_MAX - val];
    const ratio = val / FREQ_MAX;
    gaugeChart.data.datasets[0].backgroundColor[0] =
        ratio > 0.7 ? '#f87171' : ratio > 0.4 ? '#fbbf24' : '#3b82f6';
    gaugeChart.update();
    gaugeVal.textContent = hz.toFixed(1);
}

// ─── Fault session counter ─────────────────────────────────────
let sessionFaults = 0;

// ─── Chart push helper ─────────────────────────────────────────
function pushValue(chart, dsIndex, value) {
    const data = chart.data.datasets[dsIndex].data;
    data.push(value);
    while (data.length > MAX_POINTS) data.shift();
    if (dsIndex === 0) {
        const labels = chart.data.labels;
        labels.push('');
        while (labels.length > MAX_POINTS) labels.shift();
    }
}

// ─── Uptime formatter ──────────────────────────────────────────
let serverStart = null;

function formatUptime(ms) {
    const s = Math.floor(ms / 1000);
    if (s < 60)   return `${s}s`;
    if (s < 3600) return `${Math.floor(s/60)}m ${s%60}s`;
    return `${Math.floor(s/3600)}h ${Math.floor((s%3600)/60)}m`;
}

function tickUptime() {
    if (serverStart) uptimeEl.textContent = formatUptime(Date.now() - serverStart);
}
setInterval(tickUptime, 1000);

// ─── Timestamp formatter ───────────────────────────────────────
function fmtTime(ts) {
    return new Date(ts).toLocaleTimeString([], { hour12: false });
}

// ─── Fault log ─────────────────────────────────────────────────
function appendFaultRow(event) {
    // Remove empty row placeholder if present
    const empty = faultTbody.querySelector('.empty-row');
    if (empty) empty.remove();

    const tr = document.createElement('tr');
    tr.className = 'fault-row new-fault';
    tr.innerHTML = `
        <td class="mono">${fmtTime(event.ts)}</td>
        <td class="accent-red">${event.peak.toFixed(3)}</td>
        <td class="accent-amber">${event.freq.toFixed(1)}</td>
    `;
    faultTbody.prepend(tr);
    setTimeout(() => tr.classList.remove('new-fault'), 1000);

    // Cap rows at 50 visible
    const rows = faultTbody.querySelectorAll('tr:not(.empty-row)');
    if (rows.length > 50) rows[rows.length - 1].remove();
}

function populateFaultLog(events) {
    faultTbody.innerHTML = '';
    if (!events || events.length === 0) {
        faultTbody.innerHTML = '<tr class="empty-row"><td colspan="3">No faults recorded</td></tr>';
        return;
    }
    [...events].reverse().forEach(e => appendFaultRow(e));
    sessionFaults = events.length;
    kpiFaults.textContent = sessionFaults;
}

clearFaultsBtn.addEventListener('click', () => {
    sessionFaults = 0;
    kpiFaults.textContent = '0';
    faultTbody.innerHTML = '<tr class="empty-row"><td colspan="3">No faults recorded</td></tr>';
});

// ─── OTA Flash Toast ───────────────────────────────────────────
let toastTimer = null;
function showFlashToast(success, message) {
    flashToastIcon.textContent = success ? '✔' : '✖';
    flashToastIcon.style.color = success ? 'var(--green)' : 'var(--red)';
    flashToastMsg.textContent  = message;
    flashToast.className = `flash-toast ${success ? 'toast-success' : 'toast-error'}`;
    flashToast.style.display = 'flex';
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { flashToast.style.display = 'none'; }, 8000);
}

// ─── Device Event Log ──────────────────────────────────────────
function appendDevlogRow(entry) {
    const empty = devlogTbody.querySelector('.empty-row');
    if (empty) empty.remove();

    const tr = document.createElement('tr');
    tr.className = 'fault-row new-fault';
    const lvlClass = `log-level-${entry.level}`;
    tr.innerHTML = `
        <td class="mono">${fmtTime(entry.ts)}</td>
        <td class="${lvlClass}">${entry.level}</td>
        <td>${entry.msg}</td>
    `;
    devlogTbody.prepend(tr);
    setTimeout(() => tr.classList.remove('new-fault'), 1000);

    const rows = devlogTbody.querySelectorAll('tr:not(.empty-row)');
    if (rows.length > 100) rows[rows.length - 1].remove();
}

function populateDevlog(entries) {
    devlogTbody.innerHTML = '';
    if (!entries || entries.length === 0) {
        devlogTbody.innerHTML = '<tr class="empty-row"><td colspan="3">No events recorded</td></tr>';
        return;
    }
    [...entries].reverse().forEach(e => appendDevlogRow(e));
}

clearDevlogBtn.addEventListener('click', () => {
    devlogTbody.innerHTML = '<tr class="empty-row"><td colspan="3">No events recorded</td></tr>';
});

// ─── Telemetry update ──────────────────────────────────────────
function onTelemetry(d) {
    // Accel chart
    if (d.ax !== undefined) {
        pushValue(accelChart, 0, d.ax);
        pushValue(accelChart, 1, d.ay);
        pushValue(accelChart, 2, d.az);
        accelChart.update('none');
        updateCube(d.ax, d.ay, d.az);
    }

    // Peak trend chart
    if (d.peak !== undefined) {
        pushValue(peakChart, 0, d.peak);
        peakChart.update('none');
    }

    // Condition Indicators chart (CF & Kurtosis)
    if (d.cf !== undefined || d.kurt !== undefined) {
        if (d.cf   !== undefined) pushValue(condChart, 0, d.cf);
        if (d.kurt !== undefined) pushValue(condChart, 1, d.kurt);
        condChart.update('none');
    }

    // KPI cards
    if (d.peak !== undefined)  kpiPeak.textContent  = `${d.peak.toFixed(3)} G`;
    if (d.freq !== undefined)  kpiFreq.textContent  = `${d.freq.toFixed(1)} Hz`;
    if (d.temp !== undefined)  kpiTemp.textContent  = `${d.temp.toFixed(1)} °C`;
    if (d.ts   !== undefined)  kpiTs.textContent    = fmtTime(d.ts);
    if (d.rate !== undefined)  msgRate.textContent  = d.rate;

    // DSP panel
    if (d.freq !== undefined) { dspFreq.textContent = `${d.freq.toFixed(1)} Hz`; updateGauge(d.freq); }
    if (d.peak !== undefined)   dspPeak.textContent = `${d.peak.toFixed(3)} G`;
    if (d.temp !== undefined)   dspTemp.textContent = `${d.temp.toFixed(1)} °C`;

    // Health
    if (d.fault !== undefined) {
        const ok = d.fault === 0;
        kpiHealth.textContent     = ok ? 'HEALTHY' : 'FAULT';
        kpiHealth.className       = `kpi-value ${ok ? 'accent-green' : 'accent-red blink'}`;
        kpiHealthSub.textContent  = ok ? 'All bands nominal' : 'Anomaly detected';
        dspStatus.textContent     = ok ? '✓ Nominal' : '⚠ FAULT';
        dspStatus.className       = `dsp-val ${ok ? 'accent-green' : 'accent-red'}`;
    }
}

// ─── History replay ────────────────────────────────────────────
function replayHistory(history) {
    if (!history || history.length === 0) return;
    // Take last MAX_POINTS entries only
    const slice = history.slice(-MAX_POINTS);
    slice.forEach((d, i) => {
        const idx = MAX_POINTS - slice.length + i;
        if (d.ax !== undefined) {
            accelChart.data.datasets[0].data[idx] = d.ax;
            accelChart.data.datasets[1].data[idx] = d.ay;
            accelChart.data.datasets[2].data[idx] = d.az;
        }
        if (d.peak !== undefined) peakChart.data.datasets[0].data[idx] = d.peak;
        if (d.cf   !== undefined) condChart.data.datasets[0].data[idx] = d.cf;
        if (d.kurt !== undefined) condChart.data.datasets[1].data[idx] = d.kurt;
    });
    accelChart.update('none');
    peakChart.update('none');
    condChart.update('none');

    // Apply latest values
    const last = slice[slice.length - 1];
    if (last) onTelemetry(last);
}

// ─── Device connection status ──────────────────────────────────
function setDeviceStatus(connected, remote) {
    deviceDot.className   = `dot ${connected ? 'dot-online' : 'dot-offline'}`;
    deviceLabel.textContent = connected ? `Device Online${remote ? ' · ' + remote.split(':')[0] : ''}` : 'Device Offline';
    deviceBadge.className = `conn-badge ${connected ? 'badge-online' : 'badge-offline'}`;
    otaTriggerBtn.disabled = !connected;
    rebootBtn.disabled     = !connected;
}

// ─── Firmware UI ───────────────────────────────────────────────
function showFirmware(fw) {
    if (!fw) return;
    firmwareInfo.style.display = 'block';
    fwHash.textContent = fw.sha256;
    fwSize.textContent = `${fw.size.toLocaleString()} bytes`;
    fwTime.textContent = fw.uploaded_at ? new Date(fw.uploaded_at).toLocaleString() : '—';
}

function setOtaMsg(msg, type = 'info') {
    otaStatusMsg.textContent  = msg;
    otaStatusMsg.className    = `ota-status-msg ota-msg-${type}`;
    if (msg) setTimeout(() => { otaStatusMsg.textContent = ''; otaStatusMsg.className = 'ota-status-msg'; }, 5000);
}

// ─── File Upload ───────────────────────────────────────────────
dropZone.addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', e => { if (e.target.files[0]) doUpload(e.target.files[0]); });

['dragenter','dragover','dragleave','drop'].forEach(ev =>
    dropZone.addEventListener(ev, e => { e.preventDefault(); e.stopPropagation(); }));
['dragenter','dragover'].forEach(ev => dropZone.addEventListener(ev, () => dropZone.classList.add('drag-over')));
['dragleave','drop'].forEach(ev => dropZone.addEventListener(ev, () => dropZone.classList.remove('drag-over')));
dropZone.addEventListener('drop', e => { if (e.dataTransfer.files[0]) doUpload(e.dataTransfer.files[0]); });

function doUpload(file) {
    if (!file.name.endsWith('.bin')) { setOtaMsg('Only .bin firmware files are accepted.', 'error'); return; }

    uploadProgress.style.display = 'block';
    uploadBar.style.width = '0%';
    setOtaMsg('Uploading and hashing firmware…', 'info');

    // Animate progress bar while uploading
    let w = 0;
    const tick = setInterval(() => { w = Math.min(w + 2, 90); uploadBar.style.width = w + '%'; }, 60);

    const fd = new FormData();
    fd.append('firmware', file);

    fetch('/api/upload', { method: 'POST', headers: { 'X-Auth-Token': getToken() || '' }, body: fd })
        .then(r => r.json())
        .then(data => {
            clearInterval(tick);
            uploadBar.style.width = '100%';
            if (data.success) {
                setOtaMsg(`Firmware stored — ${data.size.toLocaleString()} bytes hashed.`, 'success');
                showFirmware(data);
                setTimeout(() => { uploadProgress.style.display = 'none'; uploadBar.style.width = '0%'; }, 1500);
            } else {
                setOtaMsg('Upload failed: ' + (data.error || 'unknown'), 'error');
                uploadProgress.style.display = 'none';
            }
        })
        .catch(err => {
            clearInterval(tick);
            uploadProgress.style.display = 'none';
            setOtaMsg('Upload error: ' + err.message, 'error');
        });
}

// ─── OTA trigger button ────────────────────────────────────────
otaTriggerBtn.addEventListener('click', () => {
    if (!confirm('Push OTA update to device now?')) return;
    otaTriggerBtn.disabled = true;
    fetch('/api/ota/trigger', { method: 'POST', headers: authHeaders() })
        .then(r => r.json())
        .then(d => {
            if (d.success) setOtaMsg('OTA triggered — device will download and apply update.', 'success');
            else setOtaMsg('Trigger failed: ' + (d.error || 'unknown'), 'error');
        })
        .catch(e => setOtaMsg('Error: ' + e.message, 'error'))
        .finally(() => { setTimeout(() => { otaTriggerBtn.disabled = false; }, 3000); });
});

rebootBtn.addEventListener('click', () => {
    if (!confirm('Reboot device into bootloader?')) return;
    fetch('/api/device/reboot', { method: 'POST', headers: authHeaders() })
        .then(r => r.json())
        .then(d => {
            if (d.success) setOtaMsg('Reboot command sent.', 'success');
            else setOtaMsg('Reboot failed: ' + (d.error || 'unknown'), 'error');
        })
        .catch(e => setOtaMsg('Error: ' + e.message, 'error'));
});

// ─── WebSocket ─────────────────────────────────────────────────
function connect() {
    const ws = new WebSocket(`ws://${window.location.host}?token=${encodeURIComponent(getToken() || '')}`);

    ws.onopen = () => {
        wsDot.className   = 'dot dot-online';
        wsLabel.textContent = 'Server Connected';
    };

    ws.onclose = (e) => {
        wsDot.className    = 'dot dot-offline';
        wsLabel.textContent = 'Reconnecting…';
        if (e.code === 4001) { clearToken(); showLogin('Session expired. Please sign in again.'); return; }
        setTimeout(connect, 2000);
    };

    ws.onerror = () => ws.close();

    ws.onmessage = ({ data }) => {
        let msg;
        try { msg = JSON.parse(data); } catch { return; }

        switch (msg.type) {

            case 'init':
                serverStart = Date.now() - (msg.server_uptime || 0);
                tickUptime();
                setDeviceStatus(msg.device_connected);
                replayHistory(msg.history);
                populateFaultLog(msg.fault_log);
                populateDevlog(msg.device_log);
                showFirmware(msg.firmware);
                // Restore email config UI
                if (msg.email_recipient) emailRecipient.value = msg.email_recipient;
                if (msg.email_sender)    emailSender.value    = msg.email_sender;
                if (msg.email_cooldown)  emailCooldown.value  = msg.email_cooldown;
                setEmailChip(msg.email_configured);
                break;

            case 'telemetry':
                onTelemetry(msg);
                break;

            case 'fault_event':
                sessionFaults++;
                kpiFaults.textContent = sessionFaults;
                appendFaultRow(msg);
                break;

            case 'device_status':
                setDeviceStatus(msg.connected, msg.remote);
                break;

            case 'ota_update':
                showFirmware(msg);
                setOtaMsg('New firmware deployed to server.', 'success');
                break;

            case 'ota_triggered':
                setOtaMsg('OTA push sent — device downloading firmware…', 'info');
                break;

            case 'ota_flash_result':
                showFlashToast(msg.success, msg.message);
                setOtaMsg(msg.message, msg.success ? 'success' : 'error');
                break;

            case 'device_reboot':
                setOtaMsg('Device reboot initiated.', 'info');
                break;

            case 'device_log':
                appendDevlogRow(msg);
                break;

            case 'rate_tick':
                msgRate.textContent = msg.rate;
                if (msg.server_uptime) uptimeEl.textContent = formatUptime(msg.server_uptime);
                break;

            case 'email_config':
                setEmailChip(msg.configured);
                break;
        }
    };
}

// ─── Email chip helper ──────────────────────────────────────────
function setEmailChip(configured) {
    if (!emailStatusChip) return;
    emailStatusChip.textContent = configured ? 'Configured' : 'Not configured';
    emailStatusChip.style.background = configured ? 'rgba(52,211,153,0.15)' : '';
    emailStatusChip.style.color      = configured ? '#34d399' : '';
}

function setEmailMsg(msg, type = 'info') {
    emailStatusMsg.textContent = msg;
    emailStatusMsg.className   = `ota-status-msg ota-msg-${type}`;
    if (msg) setTimeout(() => { emailStatusMsg.textContent = ''; emailStatusMsg.className = 'ota-status-msg'; }, 6000);
}

// ─── Email save ─────────────────────────────────────────────────
emailSaveBtn.addEventListener('click', async () => {
    emailSaveBtn.disabled = true;
    try {
        const r = await fetch('/api/email/config', {
            method: 'POST', headers: authHeaders(),
            body: JSON.stringify({
                recipient: emailRecipient.value.trim(),
                sender:    emailSender.value.trim(),
                password:  emailPassword.value || undefined,
                cooldown:  Number(emailCooldown.value) || 5
            })
        });
        const d = await r.json();
        if (d.success) { setEmailMsg('Email configuration saved.', 'success'); emailPassword.value = ''; }
        else setEmailMsg(d.error || 'Save failed', 'error');
    } catch (e) { setEmailMsg('Error: ' + e.message, 'error'); }
    finally { emailSaveBtn.disabled = false; }
});

// ─── Email test ─────────────────────────────────────────────────
emailTestBtn.addEventListener('click', async () => {
    emailTestBtn.disabled = true;
    setEmailMsg('Sending test email…', 'info');
    try {
        const r = await fetch('/api/email/test', { method: 'POST', headers: authHeaders() });
        const d = await r.json();
        if (d.success) setEmailMsg('Test email sent successfully!', 'success');
        else setEmailMsg('Failed: ' + (d.error || 'unknown'), 'error');
    } catch (e) { setEmailMsg('Error: ' + e.message, 'error'); }
    finally { emailTestBtn.disabled = false; }
});

// ─── Theme Toggle & High-Tech Features ───────────────────────────
const themeToggle = document.getElementById('theme-toggle');

function updateChartTheme(isLight) {
    const tColor = isLight ? '#475569' : '#64748b';
    const gColor = isLight ? 'rgba(0,0,0,0.06)' : 'rgba(255,255,255,0.06)';
    const lColor = isLight ? '#0f172a' : '#cbd5e1';
    const tBg    = isLight ? 'rgba(255,255,255,0.95)' : '#1e293b';
    const tBo    = isLight ? '#e2e8f0' : '#334155';
    const tTxt   = isLight ? '#0f172a' : '#fff';

    [accelChart, peakChart, condChart].forEach(c => {
        if (!c) return;
        ['x', 'y', 'yCF', 'yKurt'].forEach(axis => {
            if (c.options.scales[axis]) {
                if(c.options.scales[axis].ticks) c.options.scales[axis].ticks.color = tColor;
                if(c.options.scales[axis].grid)  c.options.scales[axis].grid.color = gColor;
            }
        });
        if (c.options.plugins.legend?.labels) c.options.plugins.legend.labels.color = lColor;
        if (c.options.plugins.tooltip) {
            c.options.plugins.tooltip.backgroundColor = tBg;
            c.options.plugins.tooltip.borderColor = tBo;
            c.options.plugins.tooltip.titleColor = tTxt;
            c.options.plugins.tooltip.bodyColor = tTxt;
        }
        c.update('none');
    });
}

function setTheme(light) {
    if (light) {
        document.documentElement.setAttribute('data-theme', 'light');
        if(themeToggle) themeToggle.textContent = '☀️';
        localStorage.setItem('pe_theme', 'light');
        updateChartTheme(true);
    } else {
        document.documentElement.removeAttribute('data-theme');
        if(themeToggle) themeToggle.textContent = '🌙';
        localStorage.setItem('pe_theme', 'dark');
        updateChartTheme(false);
    }
}

if(themeToggle) {
    themeToggle.addEventListener('click', () => {
        setTheme(document.documentElement.getAttribute('data-theme') !== 'light');
    });
}
if(localStorage.getItem('pe_theme') === 'light') setTheme(true);

// ─── CSV Export ──────────────────────────────────────────────────
function downloadCSV(csv, filename) {
    if (!csv) return;
    const link = document.createElement("a");
    link.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8;' }));
    link.setAttribute("download", filename);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

function extractCSV(tbody) {
    return Array.from(tbody.querySelectorAll('tr:not(.empty-row)')).map(r => 
        Array.from(r.querySelectorAll('td')).map(c => `"${c.innerText.replace(/"/g, '""')}"`).join(',')
    ).join('\n');
}

document.getElementById('export-csv-btn')?.addEventListener('click', () => {
    const csv = extractCSV(faultTbody);
    if(csv) downloadCSV("Time,Peak_G,Freq_Hz\n" + csv, "fault_log.csv");
});

document.getElementById('export-devlog-csv-btn')?.addEventListener('click', () => {
    const csv = extractCSV(devlogTbody);
    if(csv) downloadCSV("Time,Level,Message\n" + csv, "device_events.csv");
});

// ─── Fullscreen Toggle ───────────────────────────────────────────
window.toggleFullscreen = function(btn, chartId) {
    const panel = btn.closest('.panel');
    panel.classList.toggle('fullscreen');
    btn.textContent = panel.classList.contains('fullscreen') ? '×' : '⛶';
    setTimeout(() => {
        if(chartId === 'accelChart') accelChart.resize();
        if(chartId === 'peakChart') peakChart.resize();
        if(chartId === 'condChart') condChart.resize();
    }, 350);
}

// ─── Settings Modal & 3D Cube ────────────────────────────────────
const settingsBtn = document.getElementById('settings-btn');
const settingsOverlay = document.getElementById('settings-overlay');
const closeSettingsBtn = document.getElementById('close-settings-btn');
const saveSettingsBtn = document.getElementById('save-settings-btn');
const settingGlass = document.getElementById('setting-glass');
const settingAnimations = document.getElementById('setting-animations');
const settingPoints = document.getElementById('setting-points');
const imuCube = document.getElementById('imu-cube');

function updateCube(ax, ay, az) {
    if (!imuCube) return;
    const pitch = Math.atan2(ay, az) * (180 / Math.PI);
    const roll = Math.atan2(-ax, Math.sqrt(ay * ay + az * az)) * (180 / Math.PI);
    imuCube.style.transform = `translateZ(-55px) rotateX(${pitch}deg) rotateY(${roll}deg)`;
}

settingsBtn?.addEventListener('click', () => { settingsOverlay.style.display = 'flex'; });
closeSettingsBtn?.addEventListener('click', () => { settingsOverlay.style.display = 'none'; });

saveSettingsBtn?.addEventListener('click', () => {
    localStorage.setItem('pe_glass', settingGlass.checked ? '1' : '0');
    localStorage.setItem('pe_anim', settingAnimations.checked ? '1' : '0');
    if (settingPoints.value) {
        MAX_POINTS = parseInt(settingPoints.value) || 120;
        localStorage.setItem('pe_points', MAX_POINTS);
    }
    applySettings();
    settingsOverlay.style.display = 'none';
});

function applySettings() {
    const useGlass = localStorage.getItem('pe_glass') !== '0';
    if(useGlass) document.body.classList.add('glass-fx'); else document.body.classList.remove('glass-fx');
    
    const useAnim = localStorage.getItem('pe_anim') !== '0';
    if(useAnim) document.body.classList.add('micro-animations'); else document.body.classList.remove('micro-animations');
    
    const pts = localStorage.getItem('pe_points');
    if(pts) MAX_POINTS = parseInt(pts);
    
    if(settingGlass) settingGlass.checked = useGlass;
    if(settingAnimations) settingAnimations.checked = useAnim;
    if(settingPoints) settingPoints.value = MAX_POINTS;
}
applySettings();

// connect() is called by auth section after token validation

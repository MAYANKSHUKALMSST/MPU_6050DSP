/*
 * dashboard_html.h
 * Local ESP32 web dashboard served at http://stm32.local (or device IP)
 * Shows live telemetry, OTA status, and WiFi config.
 */

#pragma once

static const char index_html[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>STM32 Edge Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#0b0f1a;color:#e2e8f0;min-height:100vh;padding:1rem}
h1{font-size:1.2rem;font-weight:700;color:#fff;margin-bottom:1rem}
.sub{font-size:0.75rem;color:#64748b;margin-top:0.15rem}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:0.75rem;margin-bottom:1rem}
.card{background:#111827;border:1px solid rgba(255,255,255,0.07);border-radius:10px;padding:0.85rem 1rem}
.label{font-size:0.7rem;text-transform:uppercase;letter-spacing:.06em;color:#64748b;margin-bottom:0.25rem}
.value{font-size:1.5rem;font-weight:700;font-family:'Courier New',monospace;color:#e2e8f0}
.value.blue{color:#3b82f6}.value.green{color:#22c55e}.value.amber{color:#f59e0b}.value.red{color:#ef4444}
.panel{background:#111827;border:1px solid rgba(255,255,255,0.07);border-radius:10px;padding:1rem;margin-bottom:0.75rem}
.panel h2{font-size:0.8rem;text-transform:uppercase;letter-spacing:.06em;color:#64748b;margin-bottom:0.75rem}
.status-bar{display:flex;align-items:center;gap:0.5rem;font-size:0.85rem;margin-bottom:0.5rem}
.dot{width:8px;height:8px;border-radius:50%;background:#ef4444}
.dot.on{background:#22c55e}
progress{width:100%;height:8px;border-radius:4px;appearance:none;margin:0.4rem 0}
progress::-webkit-progress-bar{background:#1e293b;border-radius:4px}
progress::-webkit-progress-value{background:#3b82f6;border-radius:4px;transition:width .3s}
.msg{font-size:0.8rem;color:#94a3b8;margin-top:0.25rem}
input[type=text],input[type=password]{width:100%;padding:0.5rem 0.75rem;background:#1e293b;border:1px solid rgba(255,255,255,0.1);border-radius:6px;color:#e2e8f0;font-size:0.85rem;margin-bottom:0.5rem;outline:none}
input:focus{border-color:#3b82f6}
button{padding:0.5rem 1.2rem;border:none;border-radius:6px;background:#3b82f6;color:#fff;font-size:0.85rem;font-weight:600;cursor:pointer;transition:background .15s}
button:hover{background:#2563eb}
button.danger{background:#ef4444}button.danger:hover{background:#dc2626}
#wifi-msg{font-size:0.8rem;margin-top:0.4rem;color:#94a3b8}
</style>
</head>
<body>
<h1>&#9889; STM32 Edge Monitor</h1>
<div class="sub" id="ip-line">Loading...</div>
<br/>

<!-- KPI cards -->
<div class="grid">
  <div class="card"><div class="label">Peak Vibration</div><div class="value blue" id="v-peak">—</div><div class="sub">g</div></div>
  <div class="card"><div class="label">Frequency</div><div class="value blue" id="v-freq">—</div><div class="sub">Hz</div></div>
  <div class="card"><div class="label">Temperature</div><div class="value amber" id="v-temp">—</div><div class="sub">°C</div></div>
  <div class="card"><div class="label">Accel X</div><div class="value" id="v-ax">—</div><div class="sub">g</div></div>
  <div class="card"><div class="label">Accel Y</div><div class="value" id="v-ay">—</div><div class="sub">g</div></div>
  <div class="card"><div class="label">Accel Z</div><div class="value" id="v-az">—</div><div class="sub">g</div></div>
  <div class="card"><div class="label">Fault</div><div class="value" id="v-fault">—</div><div class="sub">status</div></div>
</div>

<!-- OTA status -->
<div class="panel">
  <h2>&#8679; OTA Flash Status</h2>
  <div class="status-bar"><span class="dot" id="ota-dot"></span><span id="ota-state">Idle</span></div>
  <progress id="ota-prog" value="0" max="100"></progress>
  <div class="msg" id="ota-msg">Ready</div>
</div>

<!-- WiFi config -->
<div class="panel">
  <h2>&#8984; WiFi Configuration</h2>
  <input type="text" id="wifi-ssid" placeholder="SSID"/>
  <input type="password" id="wifi-pass" placeholder="Password"/>
  <button onclick="saveWifi()">Save &amp; Reconnect</button>
  <div id="wifi-msg"></div>
</div>

<script>
// Fetch telemetry every 500 ms
function poll(){
  fetch('/api/tele').then(r=>r.json()).then(d=>{
    document.getElementById('v-peak').textContent = d.peak!=null?d.peak.toFixed(3):'—';
    document.getElementById('v-freq').textContent = d.freq!=null?d.freq.toFixed(1):'—';
    document.getElementById('v-temp').textContent = d.temp!=null?d.temp.toFixed(1):'—';
    document.getElementById('v-ax').textContent   = d.ax!=null?d.ax.toFixed(3):'—';
    document.getElementById('v-ay').textContent   = d.ay!=null?d.ay.toFixed(3):'—';
    document.getElementById('v-az').textContent   = d.az!=null?d.az.toFixed(3):'—';
    var fEl=document.getElementById('v-fault');
    fEl.textContent=d.fault===0?'OK':'FAULT';
    fEl.className='value '+(d.fault===0?'green':'red');
  }).catch(()=>{});
}

// Poll OTA status every second
function pollOta(){
  fetch('/api/ota_status').then(r=>r.json()).then(d=>{
    document.getElementById('ota-state').textContent=d.state||'idle';
    document.getElementById('ota-msg').textContent=d.message||'';
    document.getElementById('ota-prog').value=d.progress||0;
    var dot=document.getElementById('ota-dot');
    dot.className='dot'+(d.state==='flashing'||d.state==='done'?' on':'');
  }).catch(()=>{});
}

function saveWifi(){
  var s=document.getElementById('wifi-ssid').value;
  var p=document.getElementById('wifi-pass').value;
  if(!s){document.getElementById('wifi-msg').textContent='Enter SSID';return;}
  fetch('/api/wifi?s='+encodeURIComponent(s)+'&p='+encodeURIComponent(p),{method:'POST'})
    .then(r=>r.text()).then(t=>{
      document.getElementById('wifi-msg').textContent=t;
    }).catch(()=>{document.getElementById('wifi-msg').textContent='Error saving';});
}

document.getElementById('ip-line').textContent='Local dashboard · '+window.location.host;
setInterval(poll,500);
setInterval(pollOta,1000);
poll();pollOta();
</script>
</body>
</html>
)rawhtml";

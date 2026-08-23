// Auto-generated from tools/scope.html by tools/embed_pages.py — do not hand-edit.
// Regenerate after editing that file:  python tools/embed_pages.py
#pragma once
#include <Arduino.h>   // for PROGMEM — must precede its use regardless of include order elsewhere

static const char SCOPE_HTML[] PROGMEM = R"AIRTTSCOPE(
<!DOCTYPE html>
<!--
  Air TT — sensor scope.

  Open this file in desktop Chrome or Edge. It reads the paddle two ways:

    SERIAL     (Phase 0) — Web Serial straight to the ESP32 over USB, 115200.
    WEBSOCKET  (Phase 1+) — the real binary protocol from the ESP32 over WiFi.

  This is the tool the swing thresholds get tuned with. Swing, watch the trace,
  read the peak numbers off the top bar. No Python, nothing to install.

  Note: Web Serial needs a real origin or a local file opened directly — it
  works from file:// in Chrome/Edge on Windows. Firefox does not support it.
-->
<html lang="en">
<head>
<meta charset="utf-8">
<title>Air TT — Sensor Scope</title>
<style>
  :root{
    --bg:#0e1116; --panel:#161b22; --line:#2a323d; --fg:#e6edf3; --dim:#8b949e;
    --ax:#ff6b6b; --ay:#51cf66; --az:#4dabf7; --mag:#ffd43b; --warn:#ff4757; --ok:#2ed573;
  }
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
       font:13px/1.45 ui-monospace,"Cascadia Code",Consolas,monospace}
  header{display:flex;gap:10px;align-items:center;flex-wrap:wrap;
         padding:10px 14px;background:var(--panel);border-bottom:1px solid var(--line)}
  h1{font-size:14px;margin:0 12px 0 0;letter-spacing:.5px}
  button,select,input{background:#21262d;color:var(--fg);border:1px solid var(--line);
                      border-radius:6px;padding:6px 12px;font:inherit;cursor:pointer}
  button:hover:not(:disabled){background:#30363d}
  button:disabled{opacity:.4;cursor:default}
  button.primary{background:#1f6feb;border-color:#1f6feb}
  button.primary:hover:not(:disabled){background:#388bfd}
  input{width:190px;cursor:text}
  .stats{display:flex;gap:18px;flex-wrap:wrap;padding:8px 14px;
         background:var(--panel);border-bottom:1px solid var(--line)}
  .stat{display:flex;gap:6px;align-items:baseline}
  .stat b{font-size:16px;font-weight:600}
  .stat span{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.6px}
  .good{color:var(--ok)} .bad{color:var(--warn)}
  #banner{display:none;padding:9px 14px;background:var(--warn);color:#fff;font-weight:600}
  #banner.show{display:block}
  .plots{padding:12px;display:flex;flex-direction:column;gap:12px}
  .plot{background:var(--panel);border:1px solid var(--line);border-radius:8px;overflow:hidden}
  .plot h2{margin:0;padding:7px 12px;font-size:11px;font-weight:600;color:var(--dim);
           text-transform:uppercase;letter-spacing:.8px;border-bottom:1px solid var(--line);
           display:flex;justify-content:space-between}
  .legend{display:flex;gap:12px;font-weight:400;text-transform:none;letter-spacing:0}
  canvas{display:block;width:100%;height:210px}
  #log{margin:0 12px 12px;padding:9px 12px;background:var(--panel);border:1px solid var(--line);
       border-radius:8px;height:120px;overflow-y:auto;white-space:pre-wrap;
       font-size:11px;color:var(--dim)}
</style>
</head>
<body>

<header>
  <h1>AIR TT · SCOPE</h1>
  <select id="src">
    <option value="serial">Serial (Phase 0, USB)</option>
    <option value="ws">WebSocket (Phase 1+, WiFi)</option>
  </select>
  <input id="wsurl" value="ws://192.168.4.1/ws" style="display:none">
  <button id="connect" class="primary">Connect</button>
  <button id="rezero" style="display:none" disabled>Re-zero</button>
  <button id="reset" disabled>Reset peaks</button>
  <button id="save" disabled>Save CSV</button>
  <label style="color:var(--dim)">window
    <select id="win">
      <option value="3">3 s</option>
      <option value="6" selected>6 s</option>
      <option value="15">15 s</option>
    </select>
  </label>
</header>

<div class="stats">
  <div class="stat"><span>state</span><b id="sState" class="bad">offline</b></div>
  <div class="stat"><span>rate</span><b id="sRate">–</b></div>
  <div class="stat" id="rttStat" style="display:none"><span>rtt</span><b id="sRtt">–</b></div>
  <div class="stat"><span>peak accel</span><b id="sPeakA">–</b></div>
  <div class="stat"><span>peak gyro</span><b id="sPeakG">–</b></div>
  <div class="stat"><span>clip a/g</span><b id="sClip" class="good">0 / 0</b></div>
  <div class="stat"><span>samples</span><b id="sN">0</b></div>
</div>

<div id="banner"></div>

<div class="plots">
  <div class="plot">
    <h2>Accelerometer &middot; g
      <span class="legend">
        <span style="color:var(--ax)">■ x</span><span style="color:var(--ay)">■ y</span>
        <span style="color:var(--az)">■ z</span><span style="color:var(--mag)">■ |a|</span>
      </span>
    </h2>
    <canvas id="cAcc"></canvas>
  </div>
  <div class="plot">
    <h2>Gyroscope &middot; deg/s
      <span class="legend">
        <span style="color:var(--ax)">■ x</span><span style="color:var(--ay)">■ y</span>
        <span style="color:var(--az)">■ z</span><span style="color:var(--mag)">■ |ω|</span>
      </span>
    </h2>
    <canvas id="cGyr"></canvas>
  </div>
</div>

<div id="log">Waiting.

Phase 0 procedure:
  1. Flash firmware/phase0_bringup, plug the ESP32 in over USB.
  2. Source = Serial, hit Connect, pick the ESP32's COM port.
  3. Hold the paddle dead still for 3 seconds — traces should be flat,
     |a| should sit at 1.00 g, |ω| near 0.
  4. Now swing it as hard as you ever will during a game.

Phase 0 passes when the peaks look like a real swing AND clip stays 0 / 0.
A nonzero clip count means the sensor pinned at its rail and the swing data
is garbage exactly where the game needs it.
</div>

<script>
const MAXPTS = 4000;                       // ~20 s at 200 Hz
const buf = { t: [], ax: [], ay: [], az: [], gx: [], gy: [], gz: [], am: [], gm: [] };
let peakA = 0, peakG = 0, clipA = 0, clipG = 0, nTotal = 0;
let rateCount = 0, rateT = performance.now(), rateHz = 0;
let connected = false, stop = null, csv = [];

const $ = id => document.getElementById(id);
const log = m => { const l = $('log'); l.textContent += '\n' + m; l.scrollTop = l.scrollHeight; };

$('src').onchange = e => { $('wsurl').style.display = e.target.value === 'ws' ? '' : 'none'; };

/* ---------- ingest ---------- */
// One sample, already in physical units.
function push(t, ax, ay, az, gx, gy, gz) {
  const am = Math.hypot(ax, ay, az), gm = Math.hypot(gx, gy, gz);
  const b = buf;
  b.t.push(t); b.ax.push(ax); b.ay.push(ay); b.az.push(az);
  b.gx.push(gx); b.gy.push(gy); b.gz.push(gz); b.am.push(am); b.gm.push(gm);
  if (b.t.length > MAXPTS) for (const k in b) b[k].shift();

  if (am > peakA) peakA = am;
  if (gm > peakG) peakG = gm;
  // Mirrors CLIP_LIMIT in the sketch: 32000/4096 g and 32000/16.4 dps.
  if (Math.max(Math.abs(ax), Math.abs(ay), Math.abs(az)) > 32000 / 4096) clipA++;
  if (Math.max(Math.abs(gx), Math.abs(gy), Math.abs(gz)) > 32000 / 16.4) clipG++;
  nTotal++; rateCount++;
  csv.push([t, ax, ay, az, gx, gy, gz].map(v => typeof v === 'number' ? v.toFixed(4) : v).join(','));
  if (csv.length > 200000) csv.shift();
}

// Phase 0 serial line: D,t_ms,ax,ay,az,gx,gy,gz,aMag,gMag,tempC
function onLine(line) {
  line = line.trim();
  if (!line) return;
  if (line[0] === 'D') {
    const p = line.split(',');
    if (p.length >= 8) push(+p[1], +p[2], +p[3], +p[4], +p[5], +p[6], +p[7]);
  } else {
    log(line);                              // summaries, warnings, boot banner
  }
}

/* ---------- serial (Phase 0) ---------- */
async function connectSerial() {
  if (!('serial' in navigator)) { log('!! Web Serial unavailable. Use Chrome or Edge on desktop.'); return; }
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: 115200 });
  log('serial open at 115200');
  setState(true);

  const dec = new TextDecoderStream();
  port.readable.pipeTo(dec.writable).catch(() => {});
  const reader = dec.readable.getReader();
  let tail = '';

  stop = async () => { try { await reader.cancel(); } catch {} try { await port.close(); } catch {} };

  (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        tail += value;
        const parts = tail.split('\n');
        tail = parts.pop();
        parts.forEach(onLine);
      }
    } catch (e) { log('serial ended: ' + e.message); }
    setState(false);
  })();
}

/* ---------- websocket (Phase 1+) ---------- */
// 20-byte telemetry frame + a 9-byte ping reply, both little-endian.
// See docs/PROTOCOL.md.
let wsRef = null, pingTimer = null, pingSentAt = 0, rttMs = null;

function sendPing() {
  if (!wsRef || wsRef.readyState !== WebSocket.OPEN) return;
  const b = new ArrayBuffer(5);
  const d = new DataView(b);
  d.setUint8(0, 0x01);                       // OP_PING
  pingSentAt = performance.now();
  d.setUint32(1, pingSentAt >>> 0, true);     // payload is echoed back verbatim
  wsRef.send(b);
}

async function connectWS() {
  const ws = new WebSocket($('wsurl').value);
  wsRef = ws;
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    log('websocket open — plots below show fused roll/pitch (scaled ±90°) ' +
        'in the accel lanes and gyro rate in the gyro lanes');
    setState(true);
    $('rezero').style.display = '';
    $('rttStat').style.display = '';
    pingTimer = setInterval(sendPing, 1000);
    sendPing();
  };
  ws.onclose = () => { log('websocket closed'); setState(false); };
  ws.onerror = () => log('!! websocket error — check the URL and that you joined the AirTT AP');
  ws.onmessage = ev => {
    if (typeof ev.data === 'string') { log(ev.data); return; }
    const d = new DataView(ev.data);
    if (d.byteLength === 9 && d.getUint8(0) === 0x01) {
      // Ping reply: our timestamp echoed back. RTT is our clock, both ends —
      // no clock sync with the ESP32 needed.
      const echoed = d.getUint32(1, true);
      rttMs = performance.now() - echoed;
      $('sRtt').textContent = rttMs.toFixed(0) + ' ms';
      return;
    }
    if (d.byteLength < 20 || d.getUint8(0) !== 0xA7) return;
    const roll = d.getInt16(8, true) / 100, pitch = d.getInt16(10, true) / 100;
    const wx = d.getInt16(12, true) / 10, wy = d.getInt16(14, true) / 10, wz = d.getInt16(16, true) / 10;
    push(d.getUint32(4, true), roll / 90, pitch / 90, 0, wx, wy, wz);
  };
  stop = () => {
    clearInterval(pingTimer);
    $('rezero').style.display = 'none';
    $('rttStat').style.display = 'none';
    ws.close();
  };
}

$('rezero').onclick = () => {
  if (!wsRef || wsRef.readyState !== WebSocket.OPEN) return;
  wsRef.send(new Uint8Array([0x02]));   // OP_REZERO
  log('-- sent re-zero --');
};

/* ---------- plotting ---------- */
function draw(cv, keys, colors, unit) {
  const dpr = devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  if (cv.width !== w * dpr) { cv.width = w * dpr; cv.height = h * dpr; }
  const g = cv.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, w, h);

  const n = buf.t.length;
  if (n < 2) return;

  // Only the trailing `window` seconds.
  const span = +$('win').value * 1000;
  const tEnd = buf.t[n - 1];
  let i0 = n - 1;
  while (i0 > 0 && tEnd - buf.t[i0] < span) i0--;
  const count = n - i0;
  if (count < 2) return;

  // Symmetric autoscale so zero sits on the centre line.
  let peak = 1e-9;
  for (const k of keys) for (let i = i0; i < n; i++) { const v = Math.abs(buf[k][i]); if (v > peak) peak = v; }
  peak *= 1.15;

  const y = v => h / 2 - (v / peak) * (h / 2 - 8);

  // grid
  g.strokeStyle = '#2a323d'; g.lineWidth = 1; g.font = '10px monospace'; g.fillStyle = '#8b949e';
  for (const f of [-1, -0.5, 0, 0.5, 1]) {
    const yy = Math.round(y(peak * f)) + 0.5;
    g.beginPath(); g.moveTo(0, yy); g.lineTo(w, yy); g.stroke();
    if (f !== 0) g.fillText((peak * f).toFixed(peak < 10 ? 2 : 0) + unit, 4, yy - 3);
  }

  keys.forEach((k, ki) => {
    g.strokeStyle = colors[ki];
    g.lineWidth = ki === keys.length - 1 ? 1.8 : 1.1;
    g.beginPath();
    for (let i = i0; i < n; i++) {
      const x = ((i - i0) / (count - 1)) * w;
      const yy = y(buf[k][i]);
      i === i0 ? g.moveTo(x, yy) : g.lineTo(x, yy);
    }
    g.stroke();
  });
}

const C = ['#ff6b6b', '#51cf66', '#4dabf7', '#ffd43b'];

function tick() {
  draw($('cAcc'), ['ax', 'ay', 'az', 'am'], C, 'g');
  draw($('cGyr'), ['gx', 'gy', 'gz', 'gm'], C, '');

  const now = performance.now();
  if (now - rateT > 500) { rateHz = rateCount / ((now - rateT) / 1000); rateCount = 0; rateT = now; }

  $('sRate').textContent  = connected ? rateHz.toFixed(0) + ' Hz' : '–';
  $('sPeakA').textContent = peakA.toFixed(2) + ' g';
  $('sPeakG').textContent = peakG.toFixed(0) + ' °/s';
  $('sN').textContent     = nTotal;

  const clipEl = $('sClip');
  clipEl.textContent = clipA + ' / ' + clipG;
  clipEl.className = (clipA || clipG) ? 'bad' : 'good';

  const banner = $('banner');
  if (clipA || clipG) {
    banner.className = 'show';
    banner.textContent = '⚠ CLIPPING — the sensor is pinned at its rail. Swing data is unusable. '
                       + 'Raise AFS_SEL / FS_SEL in the sketch and reflash.';
  } else banner.className = '';

  requestAnimationFrame(tick);
}

function setState(on) {
  connected = on;
  const s = $('sState');
  s.textContent = on ? 'live' : 'offline';
  s.className = on ? 'good' : 'bad';
  $('connect').textContent = on ? 'Disconnect' : 'Connect';
  $('reset').disabled = $('save').disabled = $('rezero').disabled = !on;
}

$('connect').onclick = async () => {
  if (connected) { if (stop) await stop(); setState(false); return; }
  try {
    $('src').value === 'ws' ? await connectWS() : await connectSerial();
  } catch (e) { log('!! connect failed: ' + e.message); }
};

$('reset').onclick = () => {
  peakA = peakG = clipA = clipG = nTotal = 0;
  csv = [];
  for (const k in buf) buf[k].length = 0;
  log('-- peaks, clip counters and capture cleared --');
};

$('save').onclick = () => {
  const blob = new Blob(['t_ms,ax,ay,az,gx,gy,gz\n' + csv.join('\n')], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'airtt-capture-' + Date.now() + '.csv';
  a.click();
  log('saved ' + csv.length + ' samples — replayable through tools/fake_paddle');
};

// When this page is served BY the ESP32 itself (Phase 1+ hosts it at "/"),
// same-origin is always correct and the phone never has to know an IP.
// Opened as a local file:// during development, fall back to the manual
// default so nothing breaks.
if (location.protocol.startsWith('http')) {
  $('wsurl').value = `ws://${location.host}/ws`;
  $('src').value = 'ws';
  $('src').dispatchEvent(new Event('change'));
}

tick();
</script>
</body>
</html>

)AIRTTSCOPE";

// NixRoute — Dashboard SPA (modern dark mode, zero external dependency)
// Stored in flash via PROGMEM. Served by esp32_router.ino at "/".
#pragma once
#include <pgmspace.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 500 500'%3E%3Crect x='70' y='70' width='92' height='375' rx='46' fill='%230c1a30'/%3E%3Crect x='338' y='70' width='92' height='375' rx='46' fill='%230c1a30'/%3E%3Cline x1='125' y1='130' x2='375' y2='380' stroke='%230c1a30' stroke-width='96' stroke-linecap='round'/%3E%3Cline x1='125' y1='130' x2='375' y2='380' stroke='%23ffffff' stroke-width='18' stroke-linecap='round'/%3E%3Ccircle cx='125' cy='130' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='125' cy='130' r='16' fill='%2300a8b5'/%3E%3Ccircle cx='250' cy='255' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='250' cy='255' r='16' fill='%2300a8b5'/%3E%3Ccircle cx='375' cy='380' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='375' cy='380' r='16' fill='%2300a8b5'/%3E%3C/svg%3E">
<title>NixRoute</title>
<style>
:root{
  --bg:#0d1117; --surface:#161b22; --surface-2:#1c2128; --surface-3:#0d1117;
  --border:#30363d; --border-2:#3d444d;
  --text:#e6edf3; --muted:#8b949e; --subtle:#6e7681;
  --accent:#00a8b5; --accent-h:#0fb9c6; --accent-soft:rgba(0,168,181,.16);
  --ok:#3fb950; --warn:#d29922; --danger:#f85149;
  --radius:6px;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{font-family:ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;
  -webkit-font-smoothing:antialiased}
a{color:var(--accent);text-decoration:none}
code,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}

header{position:sticky;top:0;z-index:20;background:rgba(13,17,23,.85);backdrop-filter:blur(10px);
  border-bottom:1px solid var(--border);padding:0 20px}
.hbar{max-width:1080px;margin:0 auto;display:flex;align-items:center;gap:14px;height:60px}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:15px;white-space:nowrap}
.brand .logo{width:36px;height:36px;flex-shrink:0}
.brand .sub{font-weight:400;font-size:11px;color:var(--subtle)}
.hbar .grow{flex:1}
.pill{font-size:11px;font-weight:600;padding:4px 10px;border-radius:999px;border:1px solid var(--border-2);
  white-space:nowrap;display:inline-flex;align-items:center;gap:6px}
.pill .dot{width:7px;height:7px;border-radius:50%}
.pill.ok{color:var(--ok);background:rgba(63,185,80,.12);border-color:rgba(63,185,80,.35)}
.pill.ok .dot{background:var(--ok)}
.pill.warn{color:var(--warn);background:rgba(210,153,34,.12);border-color:rgba(210,153,34,.35)}
.pill.warn .dot{background:var(--warn)}
.pill.off{color:var(--muted);background:var(--surface-2)}
.pill.off .dot{background:var(--subtle)}
.ipbox{font-size:12px;color:var(--muted);white-space:nowrap}
.ipbox b{color:var(--text)}
.heapbar{width:120px;flex-shrink:0}
.heapbar .lbl{font-size:10px;color:var(--subtle);display:flex;justify-content:space-between;margin-bottom:3px}
.heapbar .track{height:6px;border-radius:99px;background:var(--surface-2);overflow:hidden}
.heapbar .fill{height:100%;background:linear-gradient(90deg,#00a8b5,#3fb950);border-radius:99px;transition:width .4s}
.tabs{max-width:1080px;margin:0 auto;display:flex;gap:4px;padding:12px 20px 0;overflow-x:auto}
.tabs button{background:transparent;border:0;color:var(--muted);font-size:13px;font-weight:600;
  padding:9px 16px;border-radius:6px;cursor:pointer;white-space:nowrap}
.tabs button:hover{color:var(--text);background:var(--surface)}
.tabs button.on{color:var(--text);background:var(--surface);box-shadow:inset 0 0 0 1px var(--border)}

main{max-width:1080px;margin:0 auto;padding:20px}
.view{display:none}
.view.on{display:block}

.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);
  padding:20px;margin-bottom:18px}
.card.form{max-width:640px}
.card h2{font-size:14px;font-weight:700;margin:0 0 16px;display:flex;align-items:center;gap:8px}
.card h2 .count{font-size:11px;font-weight:600;color:var(--muted);background:var(--surface-2);
  padding:2px 9px;border-radius:999px}
.card h2 .spacer{flex:1}
.card .desc{font-size:12px;color:var(--subtle);margin:-8px 0 14px}

.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:12px}
.stat{background:var(--surface-3);border:1px solid var(--border);border-radius:var(--radius);padding:15px}
.stat .lbl{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.03em}
.stat .val{font-size:21px;font-weight:700;margin-top:5px}
.stat .val.mono{font-size:14px;word-break:break-all}
.stat .val.ok{color:var(--ok)} .stat .val.warn{color:var(--warn)} .stat .val.danger{color:var(--danger)}

label{font-size:12px;font-weight:600;display:block;margin:12px 0 6px;color:var(--muted)}
input,select,textarea{width:100%;padding:10px 12px;border:1px solid var(--border-2);border-radius:6px;
  font-size:14px;background:var(--surface-3);color:var(--text);outline:none;font-family:inherit}
input:focus,select:focus,textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
input::placeholder,textarea::placeholder{color:var(--subtle)}
.frow{display:flex;gap:12px;flex-wrap:wrap}
.frow>div{flex:1;min-width:180px}
.pwrap{position:relative}
.pwrap .eye{position:absolute;right:8px;top:50%;transform:translateY(-50%);background:none;border:0;color:var(--subtle);cursor:pointer;font-size:11px;font-weight:600;padding:4px 6px}
.pwrap .eye:hover{color:var(--accent)}
.btn{padding:9px 16px;border-radius:6px;border:1px solid transparent;background:var(--accent);color:#fff;
  font-size:13px;font-weight:600;cursor:pointer}
.btn:hover{background:var(--accent-h)}
.btn.ghost{background:transparent;color:var(--text);border-color:var(--border-2)}
.btn.ghost:hover{background:var(--surface-2)}
.btn.danger{background:transparent;color:var(--danger);border-color:rgba(248,81,73,.4)}
.btn.danger:hover{background:rgba(248,81,73,.1)}
.btn.sm{padding:6px 11px;font-size:12px;border-radius:6px}
.btn:disabled{opacity:.45;cursor:default}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.spacer{flex:1}
.spin{display:inline-block;width:12px;height:12px;border:2px solid var(--border-2);border-top-color:var(--accent);border-radius:50%;animation:spin .7s linear infinite;vertical-align:-2px}
@keyframes spin{to{transform:rotate(360deg)}}

table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.03em;color:var(--subtle);
  font-weight:600;padding:8px 10px;border-bottom:1px solid var(--border)}
td{padding:9px 10px;border-bottom:1px solid var(--border);color:var(--text)}
tr:last-child td{border-bottom:0}
td.mono{font-family:ui-monospace,monospace;font-size:12px}
td.ok{color:var(--ok)} td.danger{color:var(--danger)}
.tnum{text-align:right;font-variant-numeric:tabular-nums}

.provider{border:1px solid var(--border);border-radius:var(--radius);padding:16px;margin-bottom:12px;background:var(--surface-3)}
.provider .head{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.provider .name{font-weight:700}
.provider .id{font-family:ui-monospace,monospace;font-size:12px;color:var(--accent)}
.provider .url{font-family:ui-monospace,monospace;font-size:12px;color:var(--muted);word-break:break-all;margin:6px 0}
.provider .meta{font-size:12px;color:var(--subtle)}
.provider .ping{font-size:11px;color:var(--subtle)}
.provider .ping.ok{color:var(--ok)} .provider .ping.bad{color:var(--danger)}
.switch{position:relative;width:40px;height:22px;flex-shrink:0}
.switch input{opacity:0;width:0;height:0}
.switch .sl{position:absolute;inset:0;background:var(--border-2);border-radius:99px;cursor:pointer;transition:.2s}
.switch .sl:before{content:"";position:absolute;width:16px;height:16px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.2s}
.switch input:checked+.sl{background:var(--ok)}
.switch input:checked+.sl:before{transform:translateX(18px)}
.metrics{display:flex;gap:16px;flex-wrap:wrap;margin-top:10px;padding-top:10px;border-top:1px dashed var(--border)}
.metrics .m{font-size:12px;color:var(--muted)}
.metrics .m b{display:block;font-size:15px;color:var(--text)}
.metrics .m b.ok{color:var(--ok)} .metrics .m b.danger{color:var(--danger)} .metrics .m b.warn{color:var(--warn)}

.models{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{font-family:ui-monospace,monospace;font-size:11px;padding:3px 9px;border-radius:4px;
  background:var(--surface-2);border:1px solid var(--border);color:var(--muted)}
.chip.key{color:var(--ok)}

.snippet{display:flex;align-items:center;gap:8px;background:var(--surface-3);border:1px solid var(--border);
  border-radius:6px;padding:10px 12px;margin-bottom:8px}
.snippet .tag{font-size:11px;font-weight:700;color:var(--accent);min-width:90px}
.snippet code{flex:1;font-size:12px;word-break:break-all;color:var(--muted)}

.playground{display:flex;flex-direction:column;gap:12px}
.playground .chat{display:flex;gap:8px}
.playground .chat input{flex:1}
.pg-output{background:var(--surface-3);border:1px solid var(--border);border-radius:6px;padding:16px;
  min-height:200px;max-height:50vh;overflow-y:auto;white-space:pre-wrap;font-size:13px;color:var(--text)}

.empty{color:var(--subtle);font-size:13px;text-align:center;padding:26px}

.kv{display:flex;flex-direction:column;gap:10px}
.kv .item{display:flex;justify-content:space-between;gap:14px;padding:11px 0;border-bottom:1px solid var(--border);font-size:13px}
.kv .item:last-child{border-bottom:0}
.kv .lbl{color:var(--muted)}
.kv .val{font-family:ui-monospace,monospace;word-break:break-all;text-align:right}

#toast{position:fixed;bottom:20px;right:20px;display:flex;flex-direction:column;gap:8px;z-index:100}
.toast{background:var(--surface);border:1px solid var(--border-2);border-radius:6px;padding:12px 16px;
  font-size:13px;max-width:340px;animation:slide .2s ease}
.toast.error{border-color:var(--danger)} .toast.ok{border-color:var(--ok)}
@keyframes slide{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}

@media(max-width:640px){
  .hbar{height:auto;flex-wrap:wrap;padding:10px 0}
  .heapbar{width:100px}
  .ipbox{display:none}
  main{padding:14px}
  .frow>div{min-width:100%}
}
</style>
<style>
/* NixRoute glass theme — translucent surfaces over a quiet deep-blue canvas.
   No external assets; backdrop-filter degrades gracefully to a flat dark. */
:root{
  --bg:#0a0f1e; --surface:rgba(255,255,255,.045); --surface-2:rgba(255,255,255,.07);
  --surface-3:rgba(255,255,255,.028); --border:rgba(255,255,255,.09); --border-2:rgba(255,255,255,.15);
  --text:#eaf0fb; --muted:#9aa8c2; --subtle:#7c89a6;
  --accent:#2fd3de; --accent-h:#59e1ea; --accent-soft:rgba(47,211,222,.16);
  --ok:#58e08c; --warn:#ffcd5c; --danger:#ff7d8a; --radius:14px;
}
body{
  background:
    radial-gradient(1100px 620px at 88% -12%, rgba(70,90,150,.26), transparent 62%),
    radial-gradient(900px 520px at -8% 22%, rgba(0,168,181,.15), transparent 55%),
    radial-gradient(700px 500px at 50% 115%, rgba(88,60,140,.14), transparent 60%),
    linear-gradient(180deg,#0b1022 0%,#080d1b 55%,#05070f 100%);
  background-attachment:fixed;
}
header{background:rgba(11,16,34,.55);backdrop-filter:blur(18px) saturate(1.4);-webkit-backdrop-filter:blur(18px) saturate(1.4);
  border-bottom:1px solid rgba(255,255,255,.08)}
.card{background:linear-gradient(180deg,rgba(255,255,255,.065),rgba(255,255,255,.03));
  border:1px solid rgba(255,255,255,.11);
  backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
  box-shadow:0 12px 34px rgba(2,6,18,.4),inset 0 1px 0 rgba(255,255,255,.07)}
.stat{background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.1);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px)}
.provider{background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.1);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px)}
.snippet{background:rgba(255,255,255,.03);border-color:rgba(255,255,255,.09)}
input,select,textarea{background:rgba(3,7,16,.55);border-color:rgba(255,255,255,.13)}
input:focus,select:focus,textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
.btn{background:linear-gradient(180deg,#16c7d4,#00a4b0);box-shadow:0 5px 16px rgba(0,168,181,.26)}
.btn:hover{filter:brightness(1.08)}
.btn.ghost{background:rgba(255,255,255,.06);border-color:rgba(255,255,255,.16);box-shadow:none}
.btn.ghost:hover{background:rgba(255,255,255,.11)}
.btn.danger{border-color:rgba(255,125,138,.4);box-shadow:none}
.tabs button.on{background:rgba(255,255,255,.09);box-shadow:inset 0 0 0 1px rgba(255,255,255,.16)}
.tabs button:hover{background:rgba(255,255,255,.05)}
.pill.ok{color:var(--ok);background:rgba(88,224,140,.12);border-color:rgba(88,224,140,.35)}
.pill.warn{color:var(--warn);background:rgba(255,205,92,.12);border-color:rgba(255,205,92,.35)}
.pill.off{color:var(--muted);background:rgba(255,255,255,.05);border-color:rgba(255,255,255,.12)}
.chip{background:rgba(255,255,255,.06)}
.pg-output{background:rgba(3,7,16,.5);border-color:rgba(255,255,255,.1);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px)}
#toast .toast{background:rgba(18,24,44,.92);border-color:rgba(255,255,255,.14);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px)}
.heapbar .track{background:rgba(255,255,255,.08)}
.kv .val.open{color:var(--ok)}
table td,table th{border-color:rgba(255,255,255,.07)}
</style>
</head>
<body>

<header>
  <div class="hbar">
    <div class="brand"><svg class="logo" viewBox="0 0 500 500" xmlns="http://www.w3.org/2000/svg"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><path d="M416 85l26-26m0 0h-24m24 0v24" fill="none" stroke="#0c1a30" stroke-width="15" stroke-linecap="round" stroke-linejoin="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#ffffff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg><div>NixRoute<span class="sub"><br>ESP32 API Gateway</span></div></div>
    <div class="grow"></div>
    <span class="pill off" id="wifi-pill"><span class="dot"></span>—</span>
    <div class="ipbox">IP <b id="hdr-ip">—</b></div>
    <div class="heapbar">
      <div class="lbl"><span>Free Heap</span><span id="hdr-heap">—</span></div>
      <div class="track"><div class="fill" id="hdr-heap-fill" style="width:0%"></div></div>
    </div>
    <button class="btn ghost sm" onclick="location.href='/admin/logout'" title="Logout">Logout</button>
  </div>
</header>

<div class="tabs">
  <button class="on" data-v="overview" onclick="show('overview')">Overview</button>
  <button data-v="usage" onclick="show('usage')">Usage</button>
  <button data-v="playground" onclick="show('playground')">Playground</button>
  <button data-v="providers" onclick="show('providers')">Providers</button>
  <button data-v="settings" onclick="show('settings')">Settings</button>
</div>

<main>

  <section class="view on" id="v-overview">
    <div class="card"><h2>Device Status</h2><div class="grid" id="ov-grid"></div></div>
    <div class="card"><h2>Endpoint</h2>
      <div class="row">
        <code id="endpoint-url" style="padding:10px 12px;background:var(--surface-3);border:1px solid var(--border);border-radius:9px"></code>
        <button class="btn ghost" onclick="copyText(epUrl())">Copy</button>
      </div>
      <div class="kv" style="margin-top:14px">
        <div class="item"><span class="lbl">Token in use</span><span class="val" id="ep-token">—</span>
          <button class="btn ghost sm" onclick="copyText($('#ep-token').textContent)">Copy</button>
        </div>
      </div>
    </div>
    <div class="card"><h2>Provider Health</h2><div id="ov-providers"></div></div>
    <div class="card"><h2>Quick Client Setup</h2>
      <div class="desc">One-click snippets to point your tools at NixRoute.</div>
      <div id="snippets"></div>
    </div>
  </section>

  <section class="view" id="v-usage">
    <div class="card"><h2>Token Usage</h2><div class="grid" id="usage-grid"></div></div>
    <div class="card"><h2>Per Model</h2><div id="usage-models"></div></div>
    <div class="card"><h2>Live Requests <span class="count" id="usage-recent-count">0</span></h2>
      <div id="usage-recent"></div>
    </div>
  </section>

  <section class="view" id="v-playground">
    <div class="card">
      <h2>Playground</h2>
      <div class="desc">Test prompts with live streaming against your active providers.</div>
      <div class="playground">
        <div>
          <label>Model</label>
          <select id="pg-model"></select>
        </div>
        <div class="chat">
          <input id="pg-input" placeholder="Type a prompt…" onkeydown="if(event.key==='Enter')playgroundSend()">
          <button class="btn" id="pg-send" onclick="playgroundSend()">Send</button>
        </div>
        <div class="pg-output" id="pg-output">Response will appear here…</div>
      </div>
    </div>
  </section>

  <section class="view" id="v-providers">
    <div class="card form">
      <h2 id="prov-form-title">Add Provider</h2>
      <div class="frow">
        <div><label>Name</label><input id="p-name" placeholder="e.g. Groq"></div>
        <div><label>Base URL</label><input id="p-url" placeholder="https://api.groq.com/openai"></div>
      </div>
      <div class="frow">
        <div><label>API Key</label><div class="pwrap"><input id="p-key" type="password" placeholder="sk-..."><button class="eye" onclick="toggleEye('p-key',this)">Show</button></div></div>
        <div style="min-width:120px;flex:0 0 120px"><label>Active</label>
          <label class="switch" style="margin-top:6px"><input type="checkbox" id="p-active" checked><span class="sl"></span></label>
        </div>
      </div>
      <div class="row" style="margin-top:16px">
        <button class="btn" id="p-save" onclick="saveProvider()">Save &amp; Fetch Models</button>
        <button class="btn ghost" id="p-cancel" style="display:none" onclick="resetProviderForm()">Cancel</button>
        <span class="spacer"></span>
        <span style="font-size:12px;color:var(--subtle)" id="p-id-preview"></span>
      </div>
    </div>
    <div class="card"><h2>Providers <span class="count" id="prov-count">0</span></h2>
      <div id="provider-list"></div>
    </div>
  </section>

  <section class="view" id="v-settings">
    <div class="card form">
      <h2>Local API Token</h2>
      <div class="desc">Up to 5 bearer tokens for client access. Empty list = open (no auth).</div>
      <div id="token-list"></div>
      <div class="row" style="margin-top:14px">
        <button class="btn" id="token-create" onclick="generateToken()">Create Token</button>
        <button class="btn ghost" onclick="clearToken()">Clear All</button>
      </div>
    </div>
    <div class="card form">
      <h2>Wi-Fi</h2>
      <div class="frow">
        <div><label>SSID</label><input id="w-ssid" placeholder="WiFi SSID"></div>
        <div><label>Password</label><div class="pwrap"><input id="w-pass" type="password" placeholder="WiFi Password"><button class="eye" onclick="toggleEye('w-pass',this)">Show</button></div></div>
      </div>
      <div class="row" style="margin-top:14px"><button class="btn" onclick="saveWifi()">Save &amp; Reboot</button></div>
    </div>
    <div class="card form">
      <h2>Admin Password</h2>
      <label>New Password</label><div class="pwrap"><input id="a-pass" type="password" placeholder="min 3 characters"><button class="eye" onclick="toggleEye('a-pass',this)">Show</button></div>
      <div class="row" style="margin-top:14px"><button class="btn" onclick="savePassword()">Update</button></div>
    </div>
    <div class="card">
      <h2>System</h2>
      <div class="row"><button class="btn danger" onclick="rebootDevice()">Reboot ESP32</button><span class="spacer"></span><span style="font-size:12px;color:var(--subtle)">v<span id="foot-ver"></span></span></div>
    </div>
  </section>

</main>

<div id="toast"></div>

<script>
var S=null;
function $(s){return document.querySelector(s)}
function esc(s){return String(s==null?'':s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;')}
function toast(msg,type){var t=document.createElement('div');t.className='toast '+(type||'');t.textContent=msg;
  document.getElementById('toast').appendChild(t);setTimeout(function(){t.remove()},4000)}
async function api(path,opts){opts=opts||{};var r=await fetch(path,opts);
  if(r.status===401){location.href='/login';throw new Error('unauthorized')}
  var d=await r.json().catch(function(){return{}});
  if(!r.ok)throw new Error((d.error&&d.error.message)||('HTTP '+r.status));return d}
function post(path,body){return api(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})})}
function epUrl(){return S&&S.wifi.ip?('http://'+S.wifi.ip+'/v1'):''}
function authHeaders(){var h={'Content-Type':'application/json'};if(S&&S.token.list&&S.token.list.length)h['Authorization']='Bearer '+S.token.list[0];return h}

function show(v){
  document.querySelectorAll('.view').forEach(function(e){e.classList.remove('on')});
  var el=document.getElementById('v-'+v);if(el)el.classList.add('on');
  document.querySelectorAll('.tabs button').forEach(function(b){b.classList.toggle('on',b.getAttribute('data-v')===v)});
  try{history.replaceState(null,'','#/'+v)}catch(e){}
}

function copyText(t){if(!t)return;navigator.clipboard.writeText(t).then(function(){toast('Copied','ok')},function(){prompt('Copy:',t)})}
function toggleEye(id,btn){var el=document.getElementById(id);if(!el)return;
  if(el.type==='password'){el.type='text';btn.textContent='Hide'}else{el.type='password';btn.textContent='Show'}}

function fmtBytes(n){if(n>1048576)return (n/1048576).toFixed(1)+' MB';if(n>1024)return (n/1024).toFixed(1)+' KB';return n+' B'}
function fmtNum(n){return Number(n||0).toLocaleString('en-US')}
function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  if(d)return d+'d '+h+'h';if(h)return h+'h '+m+'m';if(m)return m+'m '+(s%60)+'s';return s+'s'}

/* ---------- Overview ---------- */
function renderOverview(){
  var w=S.wifi,s=S.stats;
  var g=document.getElementById('ov-grid');g.innerHTML='';
  function stat(l,v,cls,mono,id){var d=document.createElement('div');d.className='stat';
    d.innerHTML='<div class="lbl">'+esc(l)+'</div><div class="val '+(cls||'')+(mono?' mono':'')+'"'+(id?' id="'+id+'"':'')+'>'+esc(v)+'</div>';g.appendChild(d)}
  stat('IP Address',w.ip||'—','',true);
  stat('Wi-Fi',w.ap_mode?'AP Mode':(w.connected?(w.ssid||'Connected'):'Disconnected'),w.connected||w.ap_mode?'ok':'warn');
  stat('RSSI',w.connected?(w.rssi+' dBm'):'—');
  stat('Uptime',fmtUptime(s.uptime_s),undefined,undefined,'uptime-val');
  stat('Requests',fmtNum(s.requests_total));
  stat('Avg Latency',s.avg_latency_ms?(s.avg_latency_ms+' ms'):'—');
  stat('Active Providers',S.providers.filter(function(p){return p.active}).length);
  stat('Free Heap',fmtBytes(s.heap),s.heap<20000?'warn':'ok',true);
  var pill=document.getElementById('wifi-pill');
  if(w.ap_mode){pill.className='pill warn';pill.innerHTML='<span class="dot"></span>AP Mode'}
  else if(w.connected){pill.className='pill ok';pill.innerHTML='<span class="dot"></span>Online'}
  else{pill.className='pill off';pill.innerHTML='<span class="dot"></span>Offline'}
  document.getElementById('hdr-ip').textContent=w.ip||'—';
  document.getElementById('hdr-heap').textContent=fmtBytes(s.heap);
  var pct=s.heap_total?Math.max(0,Math.min(100,Math.round(s.heap/s.heap_total*100))):50;
  document.getElementById('hdr-heap-fill').style.width=pct+'%';
  document.getElementById('endpoint-url').textContent=epUrl()||'http://<ip>/v1';
  var epTok=document.getElementById('ep-token');
  if(epTok){var tl=S.token&&S.token.list;
    epTok.textContent=(tl&&tl.length)?tl[0]:'open (no auth)';
    epTok.className='val'+(tl&&tl.length?'':' open');}
  document.getElementById('foot-ver').textContent=S.version;
  renderSnippets();
  var op=document.getElementById('ov-providers');op.innerHTML='';
  if(!S.providers.length){op.innerHTML='<div class="empty">No providers yet. Add one in the Providers tab.</div>';return}
  S.providers.forEach(function(p){
    var d=document.createElement('div');d.className='provider';
    var ok=p.metrics.total?Math.round(p.metrics.success/p.metrics.total*100):0;
    d.innerHTML='<div class="head"><span class="name">'+esc(p.name)+'</span><span class="id">'+esc(p.id)+'</span>'+
      (p.cooling?'<span class="pill warn" style="margin-left:auto"><span class="dot"></span>cooling</span>':(p.active?'<span class="pill ok" style="margin-left:auto"><span class="dot"></span>active</span>':'<span class="pill off" style="margin-left:auto"><span class="dot"></span>disabled</span>'))+'</div>'+
      '<div class="metrics">'+
      '<div class="m">Requests<b>'+p.metrics.total+'</b></div>'+
      '<div class="m">Success<b class="ok">'+ok+'%</b></div>'+
      '<div class="m">429<b class="warn">'+p.metrics.rate_limited+'</b></div>'+
      '<div class="m">Latency<b>'+(p.metrics.last_latency_ms?p.metrics.last_latency_ms+' ms':'—')+'</b></div>'+
      '</div>';
    op.appendChild(d);
  });
}

function renderSnippets(){
  var base=epUrl();var ip=S.wifi.ip||'<ip>';var tk=(S.token.list&&S.token.list.length)?S.token.list[0]:'<token>';
  var snippets=[
    {tag:'Cursor', code:'"openai": {"baseURL": "'+base+'", "apiKey": "'+tk+'"}'},
    {tag:'Claude Code', code:'export OPENAI_BASE_URL='+base+' OPENAI_API_KEY='+tk},
    {tag:'Python SDK', code:'OpenAI(base_url="'+base+'", api_key="'+tk+'")'},
    {tag:'cURL', code:'curl '+base+'/chat/completions -H "Authorization: Bearer '+tk+'" -d \'{"model":"<provider>/<model>","messages":[{"role":"user","content":"hi"}]}\''}
  ];
  var el=document.getElementById('snippets');el.innerHTML='';
  snippets.forEach(function(s){
    var d=document.createElement('div');d.className='snippet';
    d.innerHTML='<span class="tag">'+s.tag+'</span><code>'+esc(s.code)+'</code><button class="btn ghost sm" onclick="copyText(this.parentNode.querySelector(\'code\').textContent)">Copy</button>';
    el.appendChild(d);
  });
}

/* ---------- Usage ---------- */
function renderUsage(){
  var u=S.usage||{};
  var g=document.getElementById('usage-grid');g.innerHTML='';
  function stat(l,v,cls){var d=document.createElement('div');d.className='stat';
    d.innerHTML='<div class="lbl">'+esc(l)+'</div><div class="val '+(cls||'')+'">'+esc(v)+'</div>';g.appendChild(d)}
  stat('Total Tokens',fmtNum(u.total_tokens),'ok');
  stat('Prompt Tokens',fmtNum(u.prompt_tokens));
  stat('Completion Tokens',fmtNum(u.completion_tokens));
  stat('Total Requests',fmtNum(S.stats.requests_total));

  var mm=document.getElementById('usage-models');
  var models=u.models||[];
  if(!models.length){mm.innerHTML='<div class="empty">No usage yet. Send a request to /v1/chat/completions.</div>'}
  else{
    models.sort(function(a,b){return b.tokens-a.tokens});
    var h='<table><tr><th>Model</th><th class="tnum">Requests</th><th class="tnum">Tokens</th></tr>';
    models.forEach(function(m){h+='<tr><td class="mono">'+esc(m.model)+'</td><td class="tnum">'+fmtNum(m.requests)+'</td><td class="tnum">'+fmtNum(m.tokens)+'</td></tr>'});
    h+='</table>';mm.innerHTML=h;
  }
  renderRecent(u.recent||[]);
}

function renderRecent(recent){
  var rc=recent.slice(0,20);
  document.getElementById('usage-recent-count').textContent=rc.length;
  var rr=document.getElementById('usage-recent');
  if(!rc.length){rr.innerHTML='<div class="empty">No requests yet.</div>';return}
  var h='<table><tr><th>Model</th><th class="tnum">Prompt</th><th class="tnum">Completion</th><th class="tnum">Total</th><th class="tnum">Latency</th><th></th></tr>';
  rc.forEach(function(r){
    var st=r.ok?'<td class="ok">ok</td>':'<td class="danger">fail</td>';
    h+='<tr><td class="mono">'+esc(r.model)+'</td><td class="tnum">'+fmtNum(r.prompt_tokens)+'</td><td class="tnum">'+fmtNum(r.completion_tokens)+'</td><td class="tnum">'+fmtNum(r.total_tokens)+'</td><td class="tnum">'+(r.latency_ms?r.latency_ms+' ms':'—')+'</td>'+st+'</tr>';
  });
  h+='</table>';rr.innerHTML=h;
}

// Live telemetry: prepend a WebSocket event to the recent log.
function appendLive(d){
  var rr=document.getElementById('usage-recent');
  var tbody=rr.querySelector('table');
  if(!tbody){renderRecent([{model:d.model,prompt_tokens:0,completion_tokens:0,total_tokens:d.tokens,latency_ms:d.latency_ms,ok:d.status==='ok'}]);return}
  var st=d.status==='ok'?'<td class="ok">ok</td>':'<td class="danger">fail</td>';
  var row='<tr><td class="mono">'+esc(d.model)+'</td><td class="tnum">0</td><td class="tnum">0</td><td class="tnum">'+fmtNum(d.tokens)+'</td><td class="tnum">'+(d.latency_ms?d.latency_ms+' ms':'—')+'</td>'+st+'</tr>';
  var first=tbody.querySelector('tr');
  var tr=document.createElement('tr');tr.innerHTML=row;
  tbody.insertBefore(tr, first);
  // trim to 20 rows
  var rows=tbody.querySelectorAll('tr');
  for(var i=20;i<rows.length;i++)rows[i].remove();
  document.getElementById('usage-recent-count').textContent=tbody.querySelectorAll('tr').length;
}

/* ---------- Playground ---------- */
function renderPlayground(){
  var sel=document.getElementById('pg-model');sel.innerHTML='';
  S.providers.forEach(function(p){
    if(!p.active)return;
    (p.models||[]).forEach(function(m){var o=document.createElement('option');o.value=m;o.textContent=m;sel.appendChild(o)});
  });
}

async function playgroundSend(){
  var model=document.getElementById('pg-model').value;
  var msg=document.getElementById('pg-input').value;
  if(!model){toast('Pick a model first','error');return}
  if(!msg){toast('Type a prompt','error');return}
  var out=document.getElementById('pg-output');out.textContent='';
  var btn=document.getElementById('pg-send');btn.disabled=true;btn.textContent='…';
  try{
    var resp=await fetch('/v1/chat/completions',{method:'POST',headers:authHeaders(),
      body:JSON.stringify({model:model,messages:[{role:'user',content:msg}],stream:true})});
    if(!resp.ok){var e=await resp.json().catch(function(){return{}});throw new Error((e.error&&e.error.message)||('HTTP '+resp.status))}
    var reader=resp.body.getReader();var dec=new TextDecoder();var buf='';
    while(true){
      var r=await reader.read();if(r.done)break;
      buf+=dec.decode(r.value,{stream:true});
      var lines=buf.split('\n');buf=lines.pop();
      for(var i=0;i<lines.length;i++){
        var l=lines[i];if(l.indexOf('data:')!==0)continue;
        var d=l.slice(5).trim();if(d==='[DONE]')continue;
        try{var j=JSON.parse(d);var delta=j.choices&&j.choices[0]&&j.choices[0].delta&&j.choices[0].delta.content;if(delta)out.textContent+=delta}catch(_){}
      }
    }
  }catch(err){out.textContent='Error: '+err.message}
  btn.disabled=false;btn.textContent='Send';
}

/* ---------- Providers ---------- */
function renderProviders(){
  document.getElementById('prov-count').textContent=S.providers.length;
  var list=document.getElementById('provider-list');list.innerHTML='';
  if(!S.providers.length){list.innerHTML='<div class="empty">No providers. Add one above.</div>';return}
  S.providers.forEach(function(p){
    var d=document.createElement('div');d.className='provider';
    var models='';
    if(p.models&&p.models.length)models='<div class="models">'+p.models.map(function(m){return '<span class="chip">'+esc(m)+'</span>'}).join('')+'</div>';
    else models='<div class="empty" style="padding:10px;text-align:left">No models cached — click Sync Models.</div>';
    d.innerHTML='<div class="head">'+
      '<label class="switch"><input type="checkbox" '+(p.active?'checked':'')+' onchange="toggleProvider(\''+esc(p.id)+'\',this.checked)"><span class="sl"></span></label>'+
      '<span class="name">'+esc(p.name)+'</span><span class="id">'+esc(p.id)+'</span>'+
      (p.cooling?'<span class="pill warn"><span class="dot"></span>cooling</span>':'')+
      '<span class="spacer"></span>'+
      '<button class="btn ghost sm" onclick="pingProvider(\''+esc(p.id)+'\',this)" title="Test latency &amp; key">Ping</button>'+
      '<button class="btn ghost sm" onclick="syncModels(\''+esc(p.id)+'\',this)">Sync</button>'+
      '<button class="btn ghost sm" onclick="editProvider(\''+esc(p.id)+'\')">Edit</button>'+
      '<button class="btn danger sm" onclick="removeProvider(\''+esc(p.id)+'\')">Delete</button></div>'+
      '<div class="url">'+esc(p.url)+'</div>'+
      '<div class="meta">API Key: '+(p.has_key?('<span class="chip key">'+esc(p.key_masked)+'</span>'):'<span style="color:var(--danger)">not set</span>')+' · '+p.models.length+' models · <span class="ping" id="ping-'+esc(p.id)+'"></span></div>'+
      '<div class="metrics">'+
      '<div class="m">Requests<b>'+p.metrics.total+'</b></div>'+
      '<div class="m">Success<b class="ok">'+p.metrics.success+'</b></div>'+
      '<div class="m">Failed<b class="danger">'+p.metrics.failed+'</b></div>'+
      '<div class="m">429<b class="warn">'+p.metrics.rate_limited+'</b></div>'+
      '<div class="m">Latency<b>'+(p.metrics.last_latency_ms?p.metrics.last_latency_ms+' ms':'—')+'</b></div>'+
      '</div>'+models;
    list.appendChild(d);
  });
}

var provEditId=null;
function resetProviderForm(){
  provEditId=null;
  document.getElementById('prov-form-title').textContent='Add Provider';
  document.getElementById('p-name').value='';document.getElementById('p-url').value='';
  document.getElementById('p-key').value='';document.getElementById('p-active').checked=true;
  document.getElementById('p-id-preview').textContent='';document.getElementById('p-cancel').style.display='none';
}

async function saveProvider(){
  var name=document.getElementById('p-name').value.trim();
  var url=document.getElementById('p-url').value.trim();
  var key=document.getElementById('p-key').value.trim();
  var active=document.getElementById('p-active').checked;
  if(!name||!url){toast('Name and Base URL are required','error');return}
  var btn=document.getElementById('p-save');btn.disabled=true;btn.textContent='Saving...';
  try{
    var body={name:name,url:url,key:key,active:active};
    if(provEditId)body.id=provEditId;   // explicit id marks an edit, not an add
    var r=await post('/api/providers',body);
    if(r.fetched_models>=0)toast('Provider "'+r.name+'" saved — '+r.fetched_models+' models','ok');
    else if(r.fetch_error)toast('Saved, but model fetch failed: '+r.fetch_error,'error');
    else toast('Provider "'+r.name+'" saved','ok');
    resetProviderForm();await load();
  }catch(e){toast(e.message,'error')}
  btn.disabled=false;btn.textContent='Save & Fetch Models';
}

function editProvider(id){
  var p=S.providers.find(function(x){return x.id===id});if(!p)return;
  provEditId=id;
  document.getElementById('prov-form-title').textContent='Edit Provider';
  document.getElementById('p-name').value=p.name;
  document.getElementById('p-url').value=p.url;
  document.getElementById('p-key').value='';
  document.getElementById('p-active').checked=p.active;
  document.getElementById('p-id-preview').textContent='id: '+p.id+' (leave key blank to keep existing)';
  document.getElementById('p-cancel').style.display='inline-block';
  show('providers');document.getElementById('p-name').focus();
}

async function syncModels(id,btn){
  if(btn){btn.disabled=true;btn.innerHTML='<span class="spin"></span>'}
  try{var r=await post('/api/providers/fetch',{id:id});
    if(r.ok)toast(id+': '+r.count+' models','ok');else toast(id+': '+r.error,'error');await load()}
  catch(e){toast(e.message,'error');if(btn){btn.disabled=false;btn.textContent='Sync'}}
}

async function pingProvider(id,btn){
  var el=document.getElementById('ping-'+id);if(el)el.textContent='…';
  try{
    var r=await post('/api/providers/ping',{id:id});
    if(el){el.textContent=r.ok?(r.latency_ms+' ms'):('HTTP '+r.status);el.className='ping '+(r.ok?'ok':'bad')}
  }catch(e){if(el){el.textContent=e.message;el.className='ping bad'}}
}

async function toggleProvider(id,on){
  try{await post('/api/providers/toggle',{id:id,active:on});toast('Provider '+(on?'enabled':'disabled'),'ok');await load()}
  catch(e){toast(e.message,'error');await load()}}

async function removeProvider(id){
  if(!confirm('Delete provider "'+id+'"?'))return;
  try{await post('/api/providers/remove',{id:id});toast('Provider deleted','ok');await load()}
  catch(e){toast(e.message,'error')}}

/* ---------- Settings ---------- */
function renderSettings(){
  var tk=S.token||{};
  var arr=tk.list||[];
  var list=document.getElementById('token-list');
  if(!arr.length){list.innerHTML='<div class="empty">No tokens. Client access is open (no auth).</div>'}
  else{
    var h='';
    arr.forEach(function(t){
      h+='<div class="snippet"><code>'+esc(t)+'</code><button class="btn ghost sm" onclick="copyText(this.parentNode.querySelector(\'code\').textContent)">Copy</button><button class="btn danger sm" onclick="deleteToken(\''+esc(t)+'\')">Delete</button></div>';
    });
    list.innerHTML=h;
  }
  var btn=document.getElementById('token-create');
  btn.disabled=arr.length>=5;
  btn.textContent=arr.length>=5?('Limit Reached ('+arr.length+'/5)'):'Create Token';
  document.getElementById('w-ssid').value=S.wifi.ssid||'';
  document.getElementById('w-pass').value='';
}

async function generateToken(){try{var r=await post('/api/token/generate');toast('Token created','ok');await load()}catch(e){toast(e.message,'error')}}
async function deleteToken(tok){if(!confirm('Delete token "'+tok+'"?'))return;
  try{await post('/api/token/delete',{token:tok});toast('Token deleted','ok');await load()}catch(e){toast(e.message,'error')}}
async function clearToken(){try{await post('/api/token/clear');toast('All tokens cleared','ok');await load()}catch(e){toast(e.message,'error')}}
async function saveWifi(){
  var s=document.getElementById('w-ssid').value.trim(),p=document.getElementById('w-pass').value;
  if(!s){toast('SSID required','error');return}
  try{await post('/api/wifi',{ssid:s,pass:p});toast('Saving & rebooting...','ok')}catch(e){toast(e.message,'error')}}
async function savePassword(){
  var p=document.getElementById('a-pass').value;
  if(p.length<3){toast('Password min 3 characters','error');return}
  try{await post('/api/password',{password:p});toast('Password updated','ok');document.getElementById('a-pass').value=''}catch(e){toast(e.message,'error')}}
async function rebootDevice(){
  if(!confirm('Reboot the ESP32?'))return;
  try{await post('/api/reboot');toast('Rebooting…','ok')}catch(e){toast(e.message,'error')}}

/* ---------- render + init ---------- */
var uptimeBase=0, uptimeAt=0;
function render(){renderOverview();renderUsage();renderPlayground();renderProviders();renderSettings()}
async function load(){try{S=await api('/api/state');uptimeBase=S.stats.uptime_s;uptimeAt=Date.now();render();connectWs()}catch(e){toast('Failed to load: '+e.message,'error')}}

var ws;
function connectWs(){
  if(ws){try{ws.close()}catch(_){}}
  if(!S||!S.wifi.ip)return;
  try{
    ws=new WebSocket('ws://'+S.wifi.ip+':81');
    ws.onmessage=function(ev){try{var d=JSON.parse(ev.data);if(d.type==='request')appendLive(d)}catch(_){}};
    ws.onclose=function(){setTimeout(function(){if(S&&S.wifi.ip)connectWs()},3000)};
  }catch(_){}
}

(function init(){
  var v=(location.hash||'').replace('#/','');
  if(['overview','usage','playground','providers','settings'].indexOf(v)<0)v='overview';
  show(v);load();
  // Local uptime ticker — increments every second between server polls.
  setInterval(function(){var el=document.getElementById('uptime-val');if(el&&uptimeAt)el.textContent=fmtUptime(uptimeBase+Math.floor((Date.now()-uptimeAt)/1000))},1000);
  setInterval(function(){if(!S)return;api('/api/state').then(function(d){S=d;uptimeBase=S.stats.uptime_s;uptimeAt=Date.now();render()},function(){})},30000);
})();
</script>
</body>
</html>
)rawliteral";

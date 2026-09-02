// ESP32 Router — Dashboard SPA (modern dark mode, zero external dependency)
// Stored in flash via PROGMEM. Served by esp32_router.ino at "/".
#pragma once
#include <pgmspace.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Router</title>
<style>
:root{
  --bg:#0f172a; --surface:#1e293b; --surface-2:#273449; --surface-3:#1a2438;
  --border:#334155; --border-2:#3b4a63;
  --text:#e2e8f0; --muted:#94a3b8; --subtle:#64748b;
  --accent:#6366f1; --accent-h:#818cf8; --accent-soft:rgba(99,102,241,.14);
  --ok:#10b981; --warn:#f59e0b; --danger:#ef4444;
  --radius:14px; --shadow:0 4px 24px rgba(0,0,0,.35);
}
*{box-sizing:border-box}
html,body{margin:0;padding:0}
body{font-family:ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
  background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;
  -webkit-font-smoothing:antialiased}
a{color:var(--accent);text-decoration:none}
code,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}

/* ---------- Header ---------- */
header{position:sticky;top:0;z-index:20;background:rgba(15,23,42,.85);backdrop-filter:blur(10px);
  border-bottom:1px solid var(--border);padding:0 20px}
.hbar{max-width:1080px;margin:0 auto;display:flex;align-items:center;gap:14px;height:60px}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:15px;white-space:nowrap}
.brand .mark{width:34px;height:34px;border-radius:9px;background:linear-gradient(135deg,#6366f1,#8b5cf6);
  display:flex;align-items:center;justify-content:center;color:#fff;font-weight:800;font-size:16px}
.brand .sub{font-weight:400;font-size:11px;color:var(--subtle)}
.hbar .grow{flex:1}
.pill{font-size:11px;font-weight:600;padding:4px 10px;border-radius:999px;border:1px solid var(--border-2);
  white-space:nowrap;display:inline-flex;align-items:center;gap:6px}
.pill .dot{width:7px;height:7px;border-radius:50%}
.pill.ok{color:var(--ok);background:rgba(16,185,129,.1);border-color:rgba(16,185,129,.3)}
.pill.ok .dot{background:var(--ok)}
.pill.warn{color:var(--warn);background:rgba(245,158,11,.1);border-color:rgba(245,158,11,.3)}
.pill.warn .dot{background:var(--warn)}
.pill.off{color:var(--muted);background:var(--surface-2)}
.pill.off .dot{background:var(--subtle)}
.ipbox{font-size:12px;color:var(--muted);white-space:nowrap}
.ipbox b{color:var(--text)}
.heapbar{width:120px;flex-shrink:0}
.heapbar .lbl{font-size:10px;color:var(--subtle);display:flex;justify-content:space-between;margin-bottom:3px}
.heapbar .track{height:6px;border-radius:99px;background:var(--surface-2);overflow:hidden}
.heapbar .fill{height:100%;background:linear-gradient(90deg,#6366f1,#10b981);border-radius:99px;transition:width .4s}
.iconbtn{width:36px;height:36px;border-radius:10px;border:1px solid var(--border-2);background:var(--surface);
  color:var(--text);cursor:pointer;display:inline-flex;align-items:center;justify-content:center;font-size:15px}
.iconbtn:hover{background:var(--surface-2)}

/* ---------- Tabs ---------- */
.tabs{max-width:1080px;margin:0 auto;display:flex;gap:4px;padding:12px 20px 0;overflow-x:auto}
.tabs button{background:transparent;border:0;color:var(--muted);font-size:13px;font-weight:600;
  padding:9px 16px;border-radius:10px;cursor:pointer;white-space:nowrap}
.tabs button:hover{color:var(--text);background:var(--surface)}
.tabs button.on{color:var(--text);background:var(--surface);box-shadow:inset 0 0 0 1px var(--border)}
.tabs button .ico{margin-right:7px}

/* ---------- Layout ---------- */
main{max-width:1080px;margin:0 auto;padding:20px}
.view{display:none}
.view.on{display:block}

.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);
  padding:20px;margin-bottom:18px;box-shadow:var(--shadow)}
.card h2{font-size:14px;font-weight:700;margin:0 0 16px;display:flex;align-items:center;gap:8px}
.card h2 .count{font-size:11px;font-weight:600;color:var(--muted);background:var(--surface-2);
  padding:2px 9px;border-radius:999px}
.card h2 .spacer{flex:1}
.card .desc{font-size:12px;color:var(--subtle);margin:-8px 0 14px}

/* metric grid */
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(190px,1fr));gap:12px}
.stat{background:var(--surface-3);border:1px solid var(--border);border-radius:12px;padding:15px}
.stat .lbl{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.03em}
.stat .val{font-size:21px;font-weight:700;margin-top:5px}
.stat .val.mono{font-size:14px;word-break:break-all}
.stat .val.ok{color:var(--ok)} .stat .val.warn{color:var(--warn)} .stat .val.danger{color:var(--danger)}

/* forms */
label{font-size:12px;font-weight:600;display:block;margin:12px 0 6px;color:var(--muted)}
input,select{width:100%;padding:10px 12px;border:1px solid var(--border-2);border-radius:10px;
  font-size:14px;background:var(--surface-3);color:var(--text);outline:none}
input:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 3px var(--accent-soft)}
input::placeholder{color:var(--subtle)}
.frow{display:flex;gap:12px;flex-wrap:wrap}
.frow>div{flex:1;min-width:180px}
.btn{padding:9px 16px;border-radius:10px;border:1px solid transparent;background:var(--accent);color:#fff;
  font-size:13px;font-weight:600;cursor:pointer}
.btn:hover{background:var(--accent-h)}
.btn.ghost{background:transparent;color:var(--text);border-color:var(--border-2)}
.btn.ghost:hover{background:var(--surface-2)}
.btn.danger{background:transparent;color:var(--danger);border-color:rgba(239,68,68,.4)}
.btn.danger:hover{background:rgba(239,68,68,.1)}
.btn.sm{padding:6px 11px;font-size:12px;border-radius:8px}
.btn:disabled{opacity:.45;cursor:default}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.spacer{flex:1}

/* tables */
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;font-size:11px;text-transform:uppercase;letter-spacing:.03em;color:var(--subtle);
  font-weight:600;padding:8px 10px;border-bottom:1px solid var(--border)}
td{padding:9px 10px;border-bottom:1px solid var(--border);color:var(--text)}
tr:last-child td{border-bottom:0}
td.mono{font-family:ui-monospace,monospace;font-size:12px}
td.ok{color:var(--ok)} td.danger{color:var(--danger)}
.tnum{text-align:right;font-variant-numeric:tabular-nums}

/* provider cards */
.provider{border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:12px;background:var(--surface-3)}
.provider .head{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.provider .name{font-weight:700}
.provider .id{font-family:ui-monospace,monospace;font-size:12px;color:var(--accent)}
.provider .url{font-family:ui-monospace,monospace;font-size:12px;color:var(--muted);word-break:break-all;margin:6px 0}
.provider .meta{font-size:12px;color:var(--subtle)}
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

/* chips / models */
.models{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{font-family:ui-monospace,monospace;font-size:11px;padding:3px 9px;border-radius:7px;
  background:var(--surface-2);border:1px solid var(--border);color:var(--muted)}
.chip.key{color:var(--ok)}

.empty{color:var(--subtle);font-size:13px;text-align:center;padding:26px}

/* kv rows for settings */
.kv{display:flex;flex-direction:column;gap:10px}
.kv .item{display:flex;justify-content:space-between;gap:14px;padding:11px 0;border-bottom:1px solid var(--border);font-size:13px}
.kv .item:last-child{border-bottom:0}
.kv .lbl{color:var(--muted)}
.kv .val{font-family:ui-monospace,monospace;word-break:break-all;text-align:right}

/* toast */
#toast{position:fixed;bottom:20px;right:20px;display:flex;flex-direction:column;gap:8px;z-index:100}
.toast{background:var(--surface);border:1px solid var(--border-2);border-radius:10px;padding:12px 16px;
  font-size:13px;box-shadow:var(--shadow);max-width:340px;animation:slide .2s ease}
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
</head>
<body>

<header>
  <div class="hbar">
    <div class="brand"><div class="mark">R</div><div>ESP32 Router<span class="sub"><br>AI API Gateway</span></div></div>
    <div class="grow"></div>
    <span class="pill off" id="wifi-pill"><span class="dot"></span>—</span>
    <div class="ipbox">IP <b id="hdr-ip">—</b></div>
    <div class="heapbar">
      <div class="lbl"><span>Free Heap</span><span id="hdr-heap">—</span></div>
      <div class="track"><div class="fill" id="hdr-heap-fill" style="width:0%"></div></div>
    </div>
    <button class="iconbtn" onclick="location.href='/admin/logout'" title="Logout">&#9166;</button>
  </div>
</header>

<div class="tabs">
  <button class="on" data-v="overview" onclick="show('overview')"><span class="ico">&#9678;</span>Overview</button>
  <button data-v="usage" onclick="show('usage')"><span class="ico">&#128200;</span>Usage</button>
  <button data-v="providers" onclick="show('providers')"><span class="ico">&#9889;</span>Providers</button>
  <button data-v="settings" onclick="show('settings')"><span class="ico">&#9881;</span>Settings</button>
</div>

<main>

  <!-- ============ OVERVIEW ============ -->
  <section class="view on" id="v-overview">
    <div class="card"><h2>Device Status</h2><div class="grid" id="ov-grid"></div></div>
    <div class="card"><h2>Endpoint</h2>
      <div class="row">
        <code id="endpoint-url" style="padding:10px 12px;background:var(--surface-3);border:1px solid var(--border);border-radius:9px"></code>
        <button class="btn ghost" onclick="copyText(epUrl())">Copy</button>
      </div>
    </div>
    <div class="card"><h2>Provider Health</h2><div id="ov-providers"></div></div>
  </section>

  <!-- ============ USAGE ============ -->
  <section class="view" id="v-usage">
    <div class="card"><h2>Token Usage</h2><div class="grid" id="usage-grid"></div></div>
    <div class="card"><h2>Per Model</h2><div id="usage-models"></div></div>
    <div class="card"><h2>Recent Requests <span class="count" id="usage-recent-count">0</span></h2>
      <div id="usage-recent"></div>
    </div>
  </section>

  <!-- ============ PROVIDERS ============ -->
  <section class="view" id="v-providers">
    <div class="card">
      <h2 id="prov-form-title">Add Provider</h2>
      <div class="frow">
        <div><label>Name</label><input id="p-name" placeholder="e.g. Groq"></div>
        <div><label>Base URL</label><input id="p-url" placeholder="https://api.groq.com/openai"></div>
      </div>
      <div class="frow">
        <div><label>API Key</label><input id="p-key" type="password" placeholder="sk-..."></div>
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

  <!-- ============ SETTINGS ============ -->
  <section class="view" id="v-settings">
    <div class="card">
      <h2>Local API Token</h2>
      <div class="kv" id="token-kv"></div>
      <div class="row" style="margin-top:14px">
        <button class="btn" onclick="generateToken()">Generate Token</button>
        <button class="btn ghost" onclick="clearToken()">Clear</button>
      </div>
    </div>
    <div class="card">
      <h2>Wi-Fi</h2>
      <div class="frow">
        <div><label>SSID</label><input id="w-ssid" placeholder="WiFi SSID"></div>
        <div><label>Password</label><input id="w-pass" type="password" placeholder="WiFi Password"></div>
      </div>
      <div class="row" style="margin-top:14px"><button class="btn" onclick="saveWifi()">Save &amp; Reboot</button></div>
    </div>
    <div class="card">
      <h2>Admin Password</h2>
      <label>New Password</label><input id="a-pass" type="password" placeholder="min 3 characters">
      <div class="row" style="margin-top:14px"><button class="btn" onclick="savePassword()">Update</button></div>
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

function show(v){
  document.querySelectorAll('.view').forEach(function(e){e.classList.remove('on')});
  var el=document.getElementById('v-'+v);if(el)el.classList.add('on');
  document.querySelectorAll('.tabs button').forEach(function(b){b.classList.toggle('on',b.getAttribute('data-v')===v)});
  try{history.replaceState(null,'','#/'+v)}catch(e){}
}

function copyText(t){if(!t)return;navigator.clipboard.writeText(t).then(function(){toast('Copied','ok')},function(){prompt('Copy:',t)})}

function fmtBytes(n){if(n>1048576)return (n/1048576).toFixed(1)+' MB';if(n>1024)return (n/1024).toFixed(1)+' KB';return n+' B'}
function fmtNum(n){return Number(n||0).toLocaleString('en-US')}
function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  if(d)return d+'d '+h+'h';if(h)return h+'h '+m+'m';if(m)return m+'m '+(s%60)+'s';return s+'s'}

/* ---------- Overview ---------- */
function renderOverview(){
  var w=S.wifi,s=S.stats;
  var g=document.getElementById('ov-grid');g.innerHTML='';
  function stat(l,v,cls,mono){var d=document.createElement('div');d.className='stat';
    d.innerHTML='<div class="lbl">'+esc(l)+'</div><div class="val '+(cls||'')+(mono?' mono':'')+'">'+esc(v)+'</div>';g.appendChild(d)}
  stat('IP Address',w.ip||'—','',true);
  stat('Wi-Fi',w.ap_mode?'AP Mode':(w.connected?(w.ssid||'Connected'):'Disconnected'),w.connected||w.ap_mode?'ok':'warn');
  stat('RSSI',w.connected?(w.rssi+' dBm'):'—');
  stat('Uptime',fmtUptime(s.uptime_s));
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
  var op=document.getElementById('ov-providers');op.innerHTML='';
  if(!S.providers.length){op.innerHTML='<div class="empty">No providers yet. Add one in the Providers tab.</div>';return}
  S.providers.forEach(function(p){
    var d=document.createElement('div');d.className='provider';
    var ok=p.metrics.total?Math.round(p.metrics.success/p.metrics.total*100):0;
    d.innerHTML='<div class="head"><span class="name">'+esc(p.name)+'</span><span class="id">'+esc(p.id)+'</span>'+
      (p.active?'<span class="pill ok" style="margin-left:auto"><span class="dot"></span>active</span>':'<span class="pill off" style="margin-left:auto"><span class="dot"></span>disabled</span>')+'</div>'+
      '<div class="metrics">'+
      '<div class="m">Requests<b>'+p.metrics.total+'</b></div>'+
      '<div class="m">Success<b class="ok">'+ok+'%</b></div>'+
      '<div class="m">429<b class="warn">'+p.metrics.rate_limited+'</b></div>'+
      '<div class="m">Latency<b>'+(p.metrics.last_latency_ms?p.metrics.last_latency_ms+' ms':'—')+'</b></div>'+
      '</div>';
    op.appendChild(d);
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

  var rc=(u.recent||[]).slice(0,20);
  document.getElementById('usage-recent-count').textContent=rc.length;
  var rr=document.getElementById('usage-recent');
  if(!rc.length){rr.innerHTML='<div class="empty">No requests yet.</div>'}
  else{
    var h='<table><tr><th>Model</th><th class="tnum">Prompt</th><th class="tnum">Completion</th><th class="tnum">Total</th><th class="tnum">Latency</th><th></th></tr>';
    rc.forEach(function(r){
      var st=r.ok?'<td class="ok">ok</td>':'<td class="danger">fail</td>';
      h+='<tr><td class="mono">'+esc(r.model)+'</td><td class="tnum">'+fmtNum(r.prompt_tokens)+'</td><td class="tnum">'+fmtNum(r.completion_tokens)+'</td><td class="tnum">'+fmtNum(r.total_tokens)+'</td><td class="tnum">'+(r.latency_ms?r.latency_ms+' ms':'—')+'</td>'+st+'</tr>';
    });
    h+='</table>';rr.innerHTML=h;
  }
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
      '<span class="name">'+esc(p.name)+'</span><span class="id">'+esc(p.id)+'</span><span class="spacer"></span>'+
      '<button class="btn ghost sm" onclick="syncModels(\''+esc(p.id)+'\')">Sync Models</button>'+
      '<button class="btn ghost sm" onclick="editProvider(\''+esc(p.id)+'\')">Edit</button>'+
      '<button class="btn danger sm" onclick="removeProvider(\''+esc(p.id)+'\')">Delete</button></div>'+
      '<div class="url">'+esc(p.url)+'</div>'+
      '<div class="meta">API Key: '+(p.has_key?('<span class="chip key">'+esc(p.key_masked)+'</span>'):'<span style="color:var(--danger)">not set</span>')+' · '+p.models.length+' models</div>'+
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

function resetProviderForm(){
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
    var r=await post('/api/providers',{name:name,url:url,key:key,active:active});
    if(r.fetched_models>=0)toast('Provider "'+r.name+'" saved — '+r.fetched_models+' models','ok');
    else if(r.fetch_error)toast('Saved, but model fetch failed: '+r.fetch_error,'error');
    else toast('Provider "'+r.name+'" saved','ok');
    resetProviderForm();await load();
  }catch(e){toast(e.message,'error')}
  btn.disabled=false;btn.textContent='Save & Fetch Models';
}

function editProvider(id){
  var p=S.providers.find(function(x){return x.id===id});if(!p)return;
  document.getElementById('prov-form-title').textContent='Edit Provider';
  document.getElementById('p-name').value=p.name;
  document.getElementById('p-url').value=p.url;
  document.getElementById('p-key').value='';
  document.getElementById('p-active').checked=p.active;
  document.getElementById('p-id-preview').textContent='id: '+p.id+' (leave key blank to keep existing)';
  document.getElementById('p-cancel').style.display='inline-block';
  show('providers');document.getElementById('p-name').focus();
}

async function syncModels(id){toast('Syncing models for '+id+'...');
  try{var r=await post('/api/providers/fetch',{id:id});
    if(r.ok)toast(id+': '+r.count+' models','ok');else toast(id+': '+r.error,'error');await load()}
  catch(e){toast(e.message,'error')}}

async function toggleProvider(id,on){
  try{await post('/api/providers/toggle',{id:id,active:on});toast('Provider '+(on?'enabled':'disabled'),'ok');await load()}
  catch(e){toast(e.message,'error');await load()}}

async function removeProvider(id){
  if(!confirm('Delete provider "'+id+'"?'))return;
  try{await post('/api/providers/remove',{id:id});toast('Provider deleted','ok');await load()}
  catch(e){toast(e.message,'error')}}

/* ---------- Settings ---------- */
function renderSettings(){
  var tk=S.token;
  document.getElementById('token-kv').innerHTML=
    '<div class="item"><span class="lbl">Status</span><span class="val">'+(tk.set?'Enabled (Bearer required)':'Disabled (open)')+'</span></div>'+
    '<div class="item"><span class="lbl">Token</span><span class="val">'+(tk.set?esc(tk.full):'—')+'</span></div>';
  document.getElementById('w-ssid').value=S.wifi.ssid||'';
  document.getElementById('w-pass').value='';
}

async function generateToken(){try{var r=await post('/api/token/generate');toast('Token generated','ok');await load()}catch(e){toast(e.message,'error')}}
async function clearToken(){try{await post('/api/token/clear');toast('Token cleared','ok');await load()}catch(e){toast(e.message,'error')}}
async function saveWifi(){
  var s=document.getElementById('w-ssid').value.trim(),p=document.getElementById('w-pass').value;
  if(!s){toast('SSID required','error');return}
  try{await post('/api/wifi',{ssid:s,pass:p});toast('Saving & rebooting...','ok')}catch(e){toast(e.message,'error')}}
async function savePassword(){
  var p=document.getElementById('a-pass').value;
  if(p.length<3){toast('Password min 3 characters','error');return}
  try{await post('/api/password',{password:p});toast('Password updated','ok');document.getElementById('a-pass').value=''}catch(e){toast(e.message,'error')}}

/* ---------- render + init ---------- */
function render(){renderOverview();renderUsage();renderProviders();renderSettings()}
async function load(){try{S=await api('/api/state');render()}catch(e){toast('Failed to load: '+e.message,'error')}}

(function init(){
  var v=(location.hash||'').replace('#/','');
  if(['overview','usage','providers','settings'].indexOf(v)<0)v='overview';
  show(v);load();setInterval(load,15000);
})();
</script>
</body>
</html>
)rawliteral";

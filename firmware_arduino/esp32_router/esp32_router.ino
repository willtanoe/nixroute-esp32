// ESP32-WROOM-32 AI API Router — Arduino (DOIT V1) — 9router-style Dashboard
// Board: DOIT ESP32 DEVKIT V1, Flash 4MB, Upload 115200
// Dashboard: http://<IP>/  (login default 123456, bisa ganti)
// Mirip 9router: Endpoint + API Keys + Usage + Settings

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>

const char* WIFI_SSID = "SuprimX";
const char* WIFI_PASS = "wooting60he+";
const char* DEEPSEEK_URL = "https://api.deepseek.com/v1/chat/completions";
const char* OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions";

Preferences prefs;
String g_localToken, g_deepseekKey, g_openrouterKey, g_customUrl, g_customKey, g_adminPass;
WebServer server(80);
uint32_t reqTotal=0, reqOk=0, reqFail=0;
unsigned long bootMs=0;

String maskKey(const String& k){
  if(k.length()<=8) return k.length()?"***":"(kosong)";
  return k.substring(0,4)+"***"+k.substring(k.length()-4);
}
String genToken(int len=32){
  const char* cs="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String s; s.reserve(len);
  for(int i=0;i<len;i++) s+=cs[esp_random()%62];
  return "sk-local-"+s;
}
void loadConfig(){
  prefs.begin("gateway", true);
  g_localToken = prefs.getString("local_token","");
  g_deepseekKey = prefs.getString("ds_key","");
  g_openrouterKey = prefs.getString("or_key","");
  g_customUrl = prefs.getString("custom_url","");
  g_customKey = prefs.getString("custom_key","");
  g_adminPass = prefs.getString("admin_pass","123456");
  prefs.end();
}
void saveKey(const char* k, const String& v){
  prefs.begin("gateway", false);
  prefs.putString(k, v);
  prefs.end();
}
bool isAuthenticated(){
  if(!server.hasHeader("Cookie")) return false;
  String c=server.header("Cookie");
  return c.indexOf("esp_auth=ok")>=0;
}
bool isDashboardAuth(){
  // dashboard + admin butuh login kalau sudah set password (default 123456)
  // kalau belum login, redirect ke /login
  return isAuthenticated();
}
bool authCheck(){
  if(g_localToken.length()==0) return true;
  if(!server.hasHeader("Authorization")) return false;
  String h=server.header("Authorization");
  String need="Bearer "+g_localToken;
  if(h.length()!=need.length()) return false;
  volatile int d=0; for(unsigned i=0;i<h.length();i++) d|=h[i]^need[i];
  return d==0;
}
void sendJson(int code, const String& j){
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Cache-Control","no-store");
  server.send(code,"application/json",j);
}
void handleHealth(){
  String ip=WiFi.localIP().toString();
  bool conn=WiFi.status()==WL_CONNECTED;
  String j=String("{\"status\":\"")+(conn?"ok":"wifi_disconnected")+"\",\"uptime_s\":"+(millis()/1000)
    +",\"wifi_connected\":"+(conn?"true":"false")+",\"ip\":\""+ip+"\",\"rssi\":"+WiFi.RSSI()
    +",\"free_heap\":"+ESP.getFreeHeap()+",\"requests_total\":"+reqTotal+",\"requests_ok\":"+reqOk+",\"requests_fail\":"+reqFail
    +",\"local_token_set\":"+(g_localToken.length()?"true":"false")+"}";
  sendJson(200,j);
}
void handleModels(){
  if(!authCheck()){ sendJson(401,"{\"error\":{\"message\":\"unauthorized\"}}"); return; }
  String m="{\"object\":\"list\",\"data\":[{\"id\":\"deepseek-chat\",\"object\":\"model\",\"owned_by\":\"deepseek\"},{\"id\":\"deepseek-reasoner\",\"object\":\"model\",\"owned_by\":\"deepseek\"},{\"id\":\"openrouter-auto\",\"object\":\"model\",\"owned_by\":\"openrouter\"}";
  if(g_customUrl.length()) m+=",{\"id\":\"custom-model\",\"object\":\"model\",\"owned_by\":\"custom\"}";
  m+="]}";
  sendJson(200,m);
}
void handleOptions(){
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Access-Control-Allow-Headers","Authorization, Content-Type");
  server.sendHeader("Access-Control-Allow-Methods","GET, POST, OPTIONS");
  server.send(204,"","");
}
void handleChat(){
  reqTotal++;
  if(!authCheck()){ reqFail++; sendJson(401,"{\"error\":{\"message\":\"unauthorized\"}}"); return; }
  String body=server.arg("plain");
  if(body.length()==0){ reqFail++; sendJson(400,"{\"error\":{\"message\":\"empty body\"}}"); return; }
  if(body.length()>8192){ reqFail++; sendJson(413,"{\"error\":{\"message\":\"payload too large\"}}"); return; }
  String model="deepseek-chat";
  int mi=body.indexOf("\"model\"");
  if(mi>=0){ int q1=body.indexOf("\"",mi+7); int q2=body.indexOf("\"",q1+1); if(q1>0&&q2>q1) model=body.substring(q1+1,q2); }
  bool useCustom=(model.startsWith("custom-")||model.startsWith("bandel-")) && g_customUrl.length() && g_customKey.length();
  bool useOR=!useCustom && (model.startsWith("openrouter-")||model.startsWith("claude-")||model.startsWith("gemini-"));
  String apiKey, url;
  if(useCustom){ apiKey=g_customKey; url=g_customUrl; if(!url.endsWith("/chat/completions")){ if(url.endsWith("/v1")) url+="/chat/completions"; else if(url.endsWith("/")) url+="v1/chat/completions"; else url+="/v1/chat/completions"; } }
  else if(useOR){ apiKey=g_openrouterKey; url=OPENROUTER_URL; }
  else { apiKey=g_deepseekKey; url=DEEPSEEK_URL; }
  if(apiKey.length()==0){
    if(!useCustom && g_deepseekKey.length()){ apiKey=g_deepseekKey; url=DEEPSEEK_URL; }
    else if(!useCustom && g_openrouterKey.length()){ apiKey=g_openrouterKey; url=OPENROUTER_URL; }
    else if(g_customUrl.length() && g_customKey.length()){ apiKey=g_customKey; url=g_customUrl; if(!url.endsWith("/chat/completions")) url+="/v1/chat/completions"; }
    else { reqFail++; sendJson(500,"{\"error\":{\"message\":\"provider API key belum diisi — buka http://"+WiFi.localIP().toString()+"/\"}}"); return; }
  }
  bool isStream=body.indexOf("\"stream\":true")>=0 || body.indexOf("\"stream\": true")>=0;
  WiFiClientSecure *client=new WiFiClientSecure; client->setInsecure();
  HTTPClient https; https.begin(*client, url);
  https.addHeader("Content-Type","application/json");
  https.addHeader("Authorization","Bearer "+apiKey);
  https.addHeader("Accept", isStream?"text/event-stream":"application/json");
  if(useOR){ https.addHeader("HTTP-Referer","http://"+WiFi.localIP().toString()); https.addHeader("X-Title","ESP32 Router"); }
  https.setTimeout(20000);
  int code=https.POST(body);
  String resp=https.getString();
  https.end(); delete client;
  if(code>=200 && code<300){
    reqOk++;
    if(isStream){ server.sendHeader("Access-Control-Allow-Origin","*"); server.sendHeader("Cache-Control","no-cache"); server.send(200,"text/event-stream",resp); }
    else sendJson(200, resp.length()?resp:"{}");
  } else {
    reqFail++;
    if(resp.length() && resp[0]=='{') sendJson(code>0?code:502, resp);
    else sendJson(code>0?code:502, String("{\"error\":{\"message\":\"upstream_error code ")+code+"\"}}");
  }
  Serial.printf("chat model=%s code=%d heap=%d\n", model.c_str(), code, ESP.getFreeHeap());
}
void handleNotFound(){ sendJson(404,"{\"error\":{\"message\":\"not found\"}}"); }
void handleFavicon(){ server.sendHeader("Cache-Control","max-age=86400"); server.send(200,"image/svg+xml",R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 500 500"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#fff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg>)SVG"); }
void handleNixrouteSvg(){ handleFavicon(); }

// ---- Login ----
void handleLogin(){
  String html=R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>Login — NixRoute ESP32</title><style>
:root{--c-bg:#f5f8fc;--c-surface:#ffffff;--c-border:#c7d3e0;--c-border-sub:#dde6ef;--c-text:#0c1a30;--c-muted:#52657d;--c-primary:#00a8b5;--c-primary-h:#008b97;--r:8px;--r-lg:12px;--sh:0 1px 2px rgba(12,26,48,.06);--sh-el:0 8px 24px rgba(12,26,48,.12)}
*{box-sizing:border-box}body{margin:0;font-family:"IBM Plex Sans",system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--c-bg);color:var(--c-text);display:flex;align-items:center;justify-content:center;min-height:100vh}
.card{width:380px;background:var(--c-surface);border:1px solid var(--c-border-sub);border-radius:16px;padding:28px;box-shadow:var(--sh-el)}
.logo{display:flex;align-items:center;gap:12px;margin-bottom:20px}.logo svg{width:36px;height:36px}
h1{font-size:18px;font-weight:700;margin:0} .sub{font-size:13px;color:var(--c-muted);margin:6px 0 18px}
label{font-size:12px;font-weight:600;color:var(--c-text);display:block;margin:12px 0 6px}
.input{width:100%;padding:11px 12px;border:1px solid var(--c-border);border-radius:10px;font-size:14px;background:#fff;outline:none}
.input:focus{border-color:var(--c-primary);box-shadow:0 0 0 3px rgba(0,168,181,.18)}
.btn{width:100%;padding:11px;border-radius:10px;border:0;background:var(--c-primary);color:#fff;font-weight:600;font-size:14px;cursor:pointer;margin-top:14px}
.btn:hover{background:var(--c-primary-h)} .hint{font-size:11px;color:var(--c-muted);margin-top:12px;text-align:center} code{background:#f0f4f8;padding:1px 5px;border-radius:6px;font-size:11px}
</style></head><body><div class=card><div class=logo><svg viewBox="0 0 500 500" width="36" height="36"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#fff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg><div><div style="font-weight:800;letter-spacing:.2px">NixRoute</div><div style="font-size:11px;color:var(--c-muted)">ESP32 · SuprimX</div></div></div><h1>Masuk Dashboard</h1><p class=sub>Password default <code>123456</code></p><form method=POST action=/admin/login><label>Password</label><input class=input name=password type=password placeholder="••••••" required autofocus><button class=btn>Masuk</button></form><p class=hint>Ganti password di Settings setelah login untuk keamanan</p></div></body></html>)HTML";
  server.send(200,"text/html",html);
}
void handleLoginPost(){
  String p=server.arg("password");
  if(p==g_adminPass){
    server.sendHeader("Set-Cookie","esp_auth=ok; Path=/; Max-Age=86400");
    server.sendHeader("Location","/");
    server.send(303,"","");
  } else {
    server.send(200,"text/html","<p>Password salah <a href=/login>kembali</a></p>");
  }
}
void handleLogout(){
  server.sendHeader("Set-Cookie","esp_auth=; Path=/; Max-Age=0");
  server.sendHeader("Location","/login");
  server.send(303,"","");
}

// ---- Dashboard 9router-style ----
String getActiveClass(const String& uri, const String& key){
  if(key=="routes" && (uri=="/"||uri=="/dashboard"||uri=="/dashboard/endpoint")) return "on";
  if(uri.indexOf(key)>=0) return "on";
  return "";
}
void handleRoot(){
  if(!isAuthenticated()){
    server.sendHeader("Location","/login");
    server.send(303,"","");
    return;
  }
  String ip=WiFi.localIP().toString();
  bool conn=WiFi.status()==WL_CONNECTED;
  String tokenMask=g_localToken.length()?maskKey(g_localToken):"(open)";
  String dsMask=g_deepseekKey.length()?maskKey(g_deepseekKey):"kosong";
  String orMask=g_openrouterKey.length()?maskKey(g_openrouterKey):"kosong";
  String cuMask=g_customUrl.length()?(g_customUrl+" <code>"+maskKey(g_customKey)+"</code>"):"kosong";
  String html = String(R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>NixRoute ESP32 v1.0.0</title><link rel="icon" href="/favicon.svg"><style>
*{scroll-behavior:smooth}
*{box-sizing:border-box}body{margin:0;font-family:"IBM Plex Sans",system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--c-bg);color:var(--c-text)}
:root{--c-bg:#f5f8fc;--c-surface:#ffffff;--c-surface-2:#f0f4f8;--c-border:#c7d3e0;--c-border-sub:#dde6ef;--c-text:#0c1a30;--c-muted:#52657d;--c-subtle:#7d8da2;--c-primary:#00a8b5;--c-primary-h:#008b97;--c-data-soft:#e8fbfc;--r:8px;--r-lg:12px;--sh:0 1px 2px rgba(12,26,48,.06);--sh-el:0 8px 24px rgba(12,26,48,.12)}
.dark{--c-bg:#07111f;--c-surface:#0f2038;--c-surface-2:#152942;--c-border:#29435f;--c-border-sub:#1f3752;--c-text:#f4f8fc;--c-muted:#a9bacd;--c-subtle:#7489a2;--c-primary:#18bec9;--c-data-soft:#103943;}
.shell{display:flex;min-height:100vh}
.rail{width:72px;background:var(--c-surface);border-right:1px solid var(--c-border-sub);display:flex;flex-direction:column;align-items:center;padding:14px 0;gap:6px;position:sticky;top:0;height:100vh}
.rail a{width:56px;height:56px;border-radius:12px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px;font-size:10px;font-weight:600;letter-spacing:.2px;color:var(--c-muted);text-decoration:none}
.rail a.on{background:var(--c-data-soft);color:var(--c-primary);border:1px solid #c9f3f6}
.rail a:hover{background:var(--c-surface-2);color:var(--c-text)}
.main{flex:1;min-width:0;background:var(--c-bg)}
.header{position:sticky;top:0;background:var(--c-surface);border-bottom:1px solid var(--c-border-sub);padding:14px 16px;display:flex;justify-content:space-between;align-items:center;backdrop-filter:saturate(1.2)}
.logo{display:flex;align-items:center;gap:10px;font-weight:800;letter-spacing:.2px}
.wrap{max-width:880px;margin:18px auto;padding:0 16px;display:flex;flex-direction:column;gap:14px}
.card{background:var(--c-surface);border:1px solid var(--c-border-sub);border-radius:16px;padding:16px;box-shadow:var(--sh)}
.card h2{font-size:13px;font-weight:700;letter-spacing:.2px;margin:0 0 12px;display:flex;align-items:center;gap:8px;color:var(--c-text)}
.row{display:flex;align-items:center;gap:8px;margin:8px 0;flex-wrap:wrap}
.badge{font-size:11px;font-family:monospace;padding:3px 8px;border-radius:999px;background:var(--c-surface-2);border:1px solid var(--c-border-sub);min-width:78px;text-align:center;color:var(--c-muted)}
.badge.on{background:var(--c-data-soft);color:var(--c-primary);border-color:#c9f3f6}.mono{font-family:"IBM Plex Mono",monospace;font-size:12px;background:var(--c-surface-2);border:1px solid var(--c-border-sub);border-radius:10px;padding:8px 10px;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.btn{padding:8px 12px;border-radius:10px;border:1px solid var(--c-primary);background:var(--c-primary);color:#fff;font-size:12px;font-weight:600;cursor:pointer}
.btn:hover{background:var(--c-primary-h)} .btn.ghost{background:var(--c-surface);color:var(--c-text);border:1px solid var(--c-border)}
.input{width:100%;padding:9px 10px;border:1px solid var(--c-border);border-radius:10px;font-size:13px;background:var(--c-surface)}
.input:focus{outline:none;border-color:var(--c-primary);box-shadow:0 0 0 3px rgba(0,168,181,.15)}
.kv{display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:12px;color:var(--c-muted)}
.kv b{color:var(--c-text)}
.small{font-size:12px;color:var(--c-muted);line-height:1.5}
.ok{color:#147a5b}.warn{color:#9a6817}
@media(max-width:640px){.rail{display:none}}
</style></head><body><div class=shell><aside class=rail>
<a href="/dashboard/endpoint" class=on><span style="width:28px;height:28px;display:flex;align-items:center;justify-content:center"><svg viewBox="0 0 500 500" width="28" height="28"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#fff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg></span>Routes</a>
<a href="/dashboard/providers"><span>🗄️</span>Providers</a>
<a href="/dashboard/policies"><span>🧩</span>Policies</a>
<a href="/dashboard/usage"><span>📊</span>Observe</a>
<a href="/dashboard/tools"><span>🔧</span>Tools</a>
<div style="flex:1"></div>
<a href="/admin/logout" style="color:#888;font-size:10px">Logout</a>
</aside><div class=main><div class=header><div class=logo>NixRoute ESP32 <span style="font-weight:400;color:#666">— SuprimX · nixroute 1.0.0</span></div><div style="font-size:12px;color:#666">)HTML") + ip + R"HTML(</div></div><div class=wrap>)HTML"
  + "<div class=card id=\"routes\"><h2>◉ API Endpoint — Routes</h2>"
  + "<div class=row><span class='badge on'>Local</span><div class=mono>http://"+ip+"/v1</div><button class='btn ghost' onclick=\"navigator.clipboard.writeText('http://"+ip+"/v1')\">Copy</button></div>"
  + "<div class=row><span class=badge>WiFi</span><div class=mono>"+(conn?ip+" · RSSI "+WiFi.RSSI()+"dBm":"disconnected")+"</div><span class='small "+(conn?"ok":"warn")+"'>"+(conn?"connected":"disconnected")+"</span></div>"
  + "<div class=kv><div><b>Uptime</b> "+(millis()/1000)+"s</div><div><b>Heap</b> "+ESP.getFreeHeap()+"</div><div><b>Requests</b> "+reqTotal+" ("+reqOk+" ok / "+reqFail+" fail)</div><div><b>IP</b> "+ip+"</div></div></div>"

  + "<div class=card><h2>🔑 API Keys — Require API key: "+String(g_localToken.length()?"ON":"OFF")+"</h2>"
  + "<div class=row><span class=badge>Local Token</span><div class=mono>"+tokenMask+"</div><form method=POST action=/admin/token/generate style='display:inline'><button class=btn>Generate</button></form><form method=POST action=/admin/token/clear style='display:inline'><button class='btn ghost'>Clear</button></form></div>"
  + (g_localToken.length()?"<div class=mono style='font-size:11px;word-break:break-all'>"+g_localToken+"</div>":"<div class=small>Open — client tidak perlu Authorization</div>")
  + "<div class=small style='margin:8px 0'>Client: <code>Authorization: Bearer TOKEN</code> atau <code>OPENAI_API_KEY=TOKEN</code> + <code>OPENAI_BASE_URL=http://"+ip+"/v1</code> — <code>curl -H \"Authorization: Bearer "+(g_localToken.length()?g_localToken:"TOKEN")+"\" http://"+ip+"/v1/models</code></div>"
  + "</div>"

  + "<div class=card id=\"providers\"><h2>🔌 Providers</h2>"
  + "<div class=row><span class=badge>DeepSeek</span><div class=mono>"+dsMask+"</div></div>"
  + "<div class=row><span class=badge>OpenRouter</span><div class=mono>"+orMask+"</div></div>"
  + "<div class=row><span class=badge>Custom</span><div class=mono style='font-size:12px'>"+cuMask+"</div></div>"
  + "<form method=POST action=/admin/keys>"
  + "<input class=input name=ds_key placeholder='DeepSeek sk-...'>"
  + "<input class=input name=or_key placeholder='OpenRouter sk-or-...'>"
  + "<input class=input name=custom_url placeholder='Custom https://bandelbanget.xyz/v1'>"
  + "<input class=input name=custom_key placeholder='Custom sk-...'>"
  + "<button class=btn>Simpan Keys</button> <span class=small>pakai model <code>custom-*</code> / <code>bandel-*</code> untuk custom</span>"
  + "</form></div>"

  + "<div class=card id=\"policies\"><h2>🧩 Policies — Routing</h2><div class=small><code>deepseek-*</code> → DeepSeek &nbsp; <code>openrouter-*/claude-*/gemini-*</code> → OpenRouter &nbsp; <code>custom-*/bandel-*</code> → Custom ("+ (g_customUrl.length()?g_customUrl:"(custom kosong)") +")</div><div class=small>Fallback: DeepSeek → OpenRouter → Custom (sebelum byte pertama)</div></div>"

  + "<div class=card id=\"observe\"><h2>📊 Observe — Usage</h2><div class=kv><div><b>Total</b> "+reqTotal+"</div><div><b>OK</b> "+reqOk+"</div><div><b>Fail</b> "+reqFail+"</div><div><b>Heap</b> "+ESP.getFreeHeap()+"</div></div><div class=small>GET <code>/health</code> · <code>/admin/status</code> untuk JSON</div></div>"

  + "<div class=card id=\"tools\"><h2>🔧 Tools</h2><div class=small><code>curl http://"+ip+"/health</code> · <code>curl -H \"Authorization: Bearer TOKEN\" http://"+ip+"/v1/models</code> · <code>POST /v1/chat/completions</code></div><div class=small>OpenAI SDK: <code>OPENAI_BASE_URL=http://"+ip+"/v1</code></div></div>"

  + "<div class=card><h2>🛡 Settings</h2>"
  + "<form method=POST action=/admin/password><div class=row><input class=input name=new_pass placeholder='Ganti password dashboard (default 123456)'><button class=btn>Update Password</button></div></form>"
  + "<div class=small>GET <code>/health</code> · <code>GET /v1/models</code> · <code>POST /v1/chat/completions</code> · <code>GET /admin/status</code></div>"
  + "</div>"

  + "<script>try{if(localStorage.getItem(\"nixroute-theme\")==\"dark\"||(!localStorage.getItem(\"nixroute-theme\")&&matchMedia(\"(prefers-color-scheme:dark)\").matches))document.documentElement.classList.add(\"dark\")}catch(e){}</script>"
  + "</div></div></body></html>";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"text/html",html);
}
void handleKeysPost(){
  String ds=server.arg("ds_key"), orK=server.arg("or_key"), cu=server.arg("custom_url"), ck=server.arg("custom_key");
  if(ds.length()){ saveKey("ds_key", ds); g_deepseekKey=ds; }
  if(orK.length()){ saveKey("or_key", orK); g_openrouterKey=orK; }
  if(cu.length()){ if(cu.endsWith("/")) cu=cu.substring(0,cu.length()-1); saveKey("custom_url", cu); g_customUrl=cu; }
  if(ck.length()){ saveKey("custom_key", ck); g_customKey=ck; }
  server.sendHeader("Location","/"); server.send(303,"","");
}
void handleTokenGen(){ String t=genToken(32); saveKey("local_token",t); g_localToken=t; server.sendHeader("Location","/"); server.send(303,"",""); }
void handleTokenClear(){ saveKey("local_token",""); g_localToken=""; server.sendHeader("Location","/"); server.send(303,"",""); }
void handlePasswordPost(){
  if(!isAuthenticated()){ server.send(401,"text/plain","unauthorized"); return; }
  String np=server.arg("new_pass");
  if(np.length()>=3){ saveKey("admin_pass", np); g_adminPass=np; }
  server.sendHeader("Location","/"); server.send(303,"","");
}

void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n=== NixRoute ESP32 v1.0.0 ===");
  loadConfig();
  Serial.printf("admin %s local %s ds %s or %s custom %s\n", g_adminPass.c_str(), g_localToken.length()?"set":"open", g_deepseekKey.length()?"set":"-", g_openrouterKey.length()?"set":"-", g_customUrl.c_str());
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  int t=0; while(WiFi.status()!=WL_CONNECTED && t<30){ delay(500); Serial.print("."); t++; }
  if(WiFi.status()==WL_CONNECTED) Serial.printf("\nWiFi OK IP %s RSSI %d heap %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
  else Serial.printf("\nWiFi FAIL %d\n", WiFi.status());

  server.on("/favicon.svg", HTTP_GET, handleFavicon);
  server.on("/nixroute.svg", HTTP_GET, handleNixrouteSvg);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleRoot);
  server.on("/dashboard/endpoint", HTTP_GET, handleRoot);
  server.on("/dashboard/providers", HTTP_GET, handleRoot);
  server.on("/dashboard/policies", HTTP_GET, handleRoot);
  server.on("/dashboard/usage", HTTP_GET, handleRoot);
  server.on("/dashboard/tools", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/admin/login", HTTP_POST, handleLoginPost);
  server.on("/admin/logout", HTTP_GET, handleLogout);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/v1/models", HTTP_GET, handleModels);
  server.on("/admin/status", HTTP_GET, handleHealth);
  server.on("/v1/chat/completions", HTTP_POST, handleChat);
  server.on("/admin/keys", HTTP_POST, handleKeysPost);
  server.on("/admin/token/generate", HTTP_POST, handleTokenGen);
  server.on("/admin/token/clear", HTTP_POST, handleTokenClear);
  server.on("/admin/password", HTTP_POST, handlePasswordPost);
  server.on("/v1/chat/completions", HTTP_OPTIONS, handleOptions);
  server.on("/health", HTTP_OPTIONS, handleOptions);
  server.on("/v1/models", HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);
  const char* hk[]={"Authorization","Cookie"}; server.collectHeaders(hk,2);
  server.begin();
  Serial.printf("HTTP :80 dashboard http://%s/ (login %s) heap %d\n", WiFi.localIP().toString().c_str(), g_adminPass.c_str(), ESP.getFreeHeap());
}
void loop(){
  server.handleClient();
  static unsigned long last=0;
  if(millis()-last>10000){ last=millis(); Serial.printf("heartbeat uptime=%lus heap=%d wifi=%d ip=%s\n", millis()/1000, ESP.getFreeHeap(), WiFi.status()==WL_CONNECTED, WiFi.localIP().toString().c_str()); }
}
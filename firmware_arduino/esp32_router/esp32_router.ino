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

// ---- Login ----
void handleLogin(){
  String html=R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>Login — ESP32 Router</title><style>body{font-family:system-ui,sans-serif;background:#f6f6f3;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0} .card{background:#fff;border:1px solid #e5e5e5;border-radius:16px;padding:24px;width:360px;box-shadow:0 4px 16px rgba(0,0,0,.06)} h1{font-size:20px;margin:0 0 8px} p{color:#666;font-size:13px} input{width:100%;padding:10px 12px;border:1px solid #ddd;border-radius:10px;margin:12px 0;box-sizing:border-box} button{width:100%;padding:10px;border:0;border-radius:10px;background:#111;color:#fff;cursor:pointer} .hint{font-size:12px;color:#888;margin-top:8px}</style></head><body><div class=card><h1>ESP32 Router</h1><p>Masukkan password dashboard</p><form method=POST action=/admin/login><input name=password type=password placeholder="Password" required><button>Masuk</button></form><p class=hint>Default: <code>123456</code> — ganti di Settings setelah login</p></div></body></html>)HTML";
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
  String html = String(R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>ESP32 Router</title><style>
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#f6f6f3;color:#111}
.header{position:sticky;top:0;background:#fff;border-bottom:1px solid #e5e5e5;padding:12px 16px;display:flex;justify-content:space-between;align-items:center}
.logo{display:flex;align-items:center;gap:8px;font-weight:700}.logo i{width:28px;height:28px;border-radius:8px;background:linear-gradient(135deg,#f97815,#c2590a);display:flex;align-items:center;justify-content:center;color:#fff;font-weight:800}
.wrap{max-width:920px;margin:20px auto;padding:0 16px;display:flex;flex-direction:column;gap:16px}
.card{background:#fff;border:1px solid #e5e5e5;border-radius:16px;padding:16px 16px;box-shadow:0 1px 2px rgba(0,0,0,.04)}
.card h2{font-size:14px;font-weight:600;margin:0 0 12px;display:flex;align-items:center;gap:8px}
.row{display:flex;align-items:center;gap:8px;margin:8px 0}
.badge{font-size:11px;font-family:monospace;padding:2px 8px;border-radius:999px;background:#f0f0f0;min-width:78px;text-align:center}
.badge.on{background:#e6f4ea;color:#137333}.mono{font-family:monospace;font-size:13px;background:#f6f6f3;border:1px solid #e5e5e5;border-radius:10px;padding:8px 10px;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.btn{padding:7px 12px;border-radius:10px;border:1px solid #111;background:#111;color:#fff;font-size:13px;cursor:pointer}
.btn.ghost{background:#fff;color:#111;border:1px solid #ddd}
.btn.red{background:#fff;color:#d00;border:1px solid #f0c0c0}
.input{width:100%;padding:8px 10px;border:1px solid #ddd;border-radius:10px;font-size:13px}
.kv{display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:12px;color:#666}
.kv b{color:#111}
.small{font-size:12px;color:#666}
.ok{color:#137333}.warn{color:#b06000}
</style></head><body><div class=header><div class=logo><i>9</i> ESP32 Router <span style="font-weight:400;color:#666">— SuprimX</span></div><div><a href=/admin/logout style="font-size:13px;color:#666;text-decoration:none">Logout</a></div></div><div class=wrap>
)HTML")
  + "<div class=card><h2>◉ API Endpoint</h2>"
  + "<div class=row><span class='badge on'>Local</span><div class=mono>http://"+ip+"/v1</div><button class='btn ghost' onclick=\"navigator.clipboard.writeText('http://"+ip+"/v1')\">Copy</button></div>"
  + "<div class=row><span class=badge>WiFi</span><div class=mono>"+(conn?ip+" · RSSI "+WiFi.RSSI()+"dBm":"disconnected")+"</div><span class='small "+(conn?"ok":"warn")+"'>"+(conn?"connected":"disconnected")+"</span></div>"
  + "<div class=kv><div><b>Uptime</b> "+(millis()/1000)+"s</div><div><b>Heap</b> "+ESP.getFreeHeap()+"</div><div><b>Requests</b> "+reqTotal+" ("+reqOk+" ok / "+reqFail+" fail)</div><div><b>IP</b> "+ip+"</div></div></div>"

  + "<div class=card><h2>🔑 API Keys — Require API key: "+String(g_localToken.length()?"ON":"OFF")+"</h2>"
  + "<div class=row><span class=badge>Local Token</span><div class=mono>"+tokenMask+"</div><form method=POST action=/admin/token/generate style='display:inline'><button class=btn>Generate</button></form><form method=POST action=/admin/token/clear style='display:inline'><button class='btn ghost'>Clear</button></form></div>"
  + (g_localToken.length()?"<div class=mono style='font-size:11px;word-break:break-all'>"+g_localToken+"</div>":"<div class=small>Open — client tidak perlu Authorization</div>")
  + "<div class=small style='margin:8px 0'>Client: <code>Authorization: Bearer TOKEN</code> atau <code>OPENAI_API_KEY=TOKEN</code> + <code>OPENAI_BASE_URL=http://"+ip+"/v1</code> — <code>curl -H \"Authorization: Bearer "+(g_localToken.length()?g_localToken:"TOKEN")+"\" http://"+ip+"/v1/models</code></div>"
  + "</div>"

  + "<div class=card><h2>🔌 Providers</h2>"
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

  + "<div class=card><h2>🛡 Settings</h2>"
  + "<form method=POST action=/admin/password><div class=row><input class=input name=new_pass placeholder='Ganti password dashboard (default 123456)'><button class=btn>Update Password</button></div></form>"
  + "<div class=small>GET <code>/health</code> · <code>GET /v1/models</code> · <code>POST /v1/chat/completions</code> · <code>GET /admin/status</code></div>"
  + "</div>"

  + "</div></body></html>";
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
  Serial.println("\n=== ESP32 Router — 9router-style ===");
  loadConfig();
  Serial.printf("admin %s local %s ds %s or %s custom %s\n", g_adminPass.c_str(), g_localToken.length()?"set":"open", g_deepseekKey.length()?"set":"-", g_openrouterKey.length()?"set":"-", g_customUrl.c_str());
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  int t=0; while(WiFi.status()!=WL_CONNECTED && t<30){ delay(500); Serial.print("."); t++; }
  if(WiFi.status()==WL_CONNECTED) Serial.printf("\nWiFi OK IP %s RSSI %d heap %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
  else Serial.printf("\nWiFi FAIL %d\n", WiFi.status());

  server.on("/", HTTP_GET, handleRoot);
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

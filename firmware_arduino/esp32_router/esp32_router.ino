// ESP32-WROOM-32 AI API Router — Arduino IDE (DOIT V1) + Dashboard
// Board: DOIT ESP32 DEVKIT V1, Flash 4MB, Upload 115200
// Libs: ArduinoJson (Library Manager)
// Dashboard: http://<IP>/  -> kelola DeepSeek/OpenRouter keys + generate LOCAL_TOKEN (NVS)
// API: POST /v1/chat/completions, GET /health, GET /v1/models (OpenAI-compatible)

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
String g_localToken, g_deepseekKey, g_openrouterKey, g_customUrl, g_customKey;
WebServer server(80);
uint32_t reqTotal=0, reqOk=0, reqFail=0;

String maskKey(const String& k){
  if(k.length()<=8) return k.length()?"***":"(kosong)";
  return k.substring(0,4)+"***"+k.substring(k.length()-4);
}
String genToken(int len=32){
  const char* cs="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String s; s.reserve(len);
  for(int i=0;i<len;i++) s += cs[esp_random()%62];
  return "sk-local-"+s;
}
void loadConfig(){
  prefs.begin("gateway", true);
  g_localToken = prefs.getString("local_token","");
  g_deepseekKey = prefs.getString("ds_key","");
  g_openrouterKey = prefs.getString("or_key","");
  g_customUrl = prefs.getString("custom_url",""); // ex: https://bandelbanget.xyz/v1
  g_customKey = prefs.getString("custom_key","");
  prefs.end();
}
void saveKey(const char* nsKey, const String& val){
  prefs.begin("gateway", false);
  prefs.putString(nsKey, val);
  prefs.end();
}

bool authCheck(){
  if(g_localToken.length()==0) return true;
  if(!server.hasHeader("Authorization")) return false;
  String h=server.header("Authorization");
  String need="Bearer "+g_localToken;
  if(h.length()!=need.length()) return false;
  volatile int d=0;
  for(unsigned i=0;i<h.length();i++) d|=h[i]^need[i];
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
    +",\"local_token_set\":"+(g_localToken.length()?"true":"false")+",\"ds_set\":"+(g_deepseekKey.length()?"true":"false")+",\"or_set\":"+(g_openrouterKey.length()?"true":"false")+",\"custom_set\":"+(g_customUrl.length()&&g_customKey.length()?"true":"false")+"}";
  sendJson(200,j);
}
void handleModels(){
  if(!authCheck()){ sendJson(401,"{\"error\":{\"message\":\"unauthorized\"}}"); return; }
  String m="{\"object\":\"list\",\"data\":[{\"id\":\"deepseek-chat\",\"object\":\"model\",\"owned_by\":\"deepseek\"},{\"id\":\"deepseek-reasoner\",\"object\":\"model\",\"owned_by\":\"deepseek\"},{\"id\":\"openrouter-auto\",\"object\":\"model\",\"owned_by\":\"openrouter\"}";
  if(g_customUrl.length()) m+=",{\"id\":\"custom-model\",\"object\":\"model\",\"owned_by\":\"custom\"},{\"id\":\"bandel-model\",\"object\":\"model\",\"owned_by\":\"custom\"}";
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
  if(!authCheck()){ reqFail++; sendJson(401,"{\"error\":{\"message\":\"unauthorized\",\"type\":\"auth_error\"}}"); return; }
  String body=server.arg("plain");
  if(body.length()==0){ reqFail++; sendJson(400,"{\"error\":{\"message\":\"empty body\"}}"); return; }
  if(body.length()>8192){ reqFail++; sendJson(413,"{\"error\":{\"message\":\"payload too large\"}}"); return; }
  String model="deepseek-chat";
  int mi=body.indexOf("\"model\"");
  if(mi>=0){ int q1=body.indexOf("\"",mi+7); int q2=body.indexOf("\"",q1+1); if(q1>0&&q2>q1) model=body.substring(q1+1,q2); }
  bool useCustom = (model.startsWith("custom-")||model.startsWith("bandel-")) && g_customUrl.length() && g_customKey.length();
  bool useOR = !useCustom && (model.startsWith("openrouter-") || model.startsWith("claude-") || model.startsWith("gemini-"));
  String apiKey, url;
  if(useCustom){ apiKey=g_customKey; url=g_customUrl; if(!url.endsWith("/chat/completions")){ if(url.endsWith("/v1")) url+="/chat/completions"; else if(url.endsWith("/")) url+="v1/chat/completions"; else url+="/v1/chat/completions"; } }
  else if(useOR){ apiKey=g_openrouterKey; url=OPENROUTER_URL; }
  else { apiKey=g_deepseekKey; url=DEEPSEEK_URL; }
  if(apiKey.length()==0){
    if(useCustom){ /* no fallback for custom */ }
    else if(g_deepseekKey.length()){ apiKey=g_deepseekKey; url=DEEPSEEK_URL; }
    else if(g_openrouterKey.length()){ apiKey=g_openrouterKey; url=OPENROUTER_URL; }
    else if(g_customUrl.length() && g_customKey.length()){ apiKey=g_customKey; url=g_customUrl; if(!url.endsWith("/chat/completions")) url+="/v1/chat/completions"; }
    else { reqFail++; sendJson(500,"{\"error\":{\"message\":\"provider API key belum diisi — buka http://"+WiFi.localIP().toString()+"/ isi di dashboard\",\"type\":\"config\"}}"); return; }
  }
  bool isStream = body.indexOf("\"stream\":true")>=0 || body.indexOf("\"stream\": true")>=0;
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

// --- Dashboard ---
void handleRoot(){
  String ip=WiFi.localIP().toString();
  String html = String(R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>ESP32 Router</title><style>body{font-family:system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 16px}h1{font-size:24px}h2{margin-top:32px}code{background:#f3f3f3;padding:2px 6px;border-radius:4px}.card{border:1px solid #ddd;border-radius:12px;padding:16px;margin:12px 0}input{width:100%;padding:8px;margin:6px 0;box-sizing:border-box}button{padding:8px 14px;border-radius:8px;border:0;background:#111;color:#fff;cursor:pointer}small{color:#666}.ok{color:#0a7}.warn{color:#d60}</style></head><body>)HTML")
  + "<h1>ESP32 Router — SuprimX</h1><div class=card><b>IP:</b> <code>"+ip+"</code> · <b>WiFi:</b> "+(WiFi.status()==WL_CONNECTED?"<span class=ok>connected</span>":"<span class=warn>disconnected</span>")+" · <b>RSSI</b> "+WiFi.RSSI()+" · <b>Heap</b> "+ESP.getFreeHeap()+" · <b>Uptime</b> "+(millis()/1000)+"s</div>"
  + "<div class=card><h2>Gateway Token (untuk client)</h2><p>Client pakai <code>Authorization: Bearer TOKEN</code> atau <code>OPENAI_API_KEY=TOKEN</code> + <code>OPENAI_BASE_URL=http://"+ip+"/v1</code></p>"
  + "<p><b>Token saat ini:</b> <code>"+(g_localToken.length()?maskKey(g_localToken):"(open — tidak butuh token)")+"</code> "+(g_localToken.length()?"<small>"+g_localToken+"</small>":"")+"</p>"
  + "<form method=POST action=/admin/token/generate><button>Generate Token Baru</button> <small>generate <code>sk-local-...</code> 32 char, simpan ke NVS</small></form>"
  + "<form method=POST action=/admin/token/clear style='margin-top:8px'><button style='background:#666'>Hapus Token (jadi open)</button></form>"
  + "<p><b>Curl test:</b> <code>curl -H \"Authorization: Bearer "+(g_localToken.length()?g_localToken:"TOKEN")+"\" http://"+ip+"/v1/models</code></p></div>"
  + "<div class=card><h2>Provider Keys</h2>"
  + "<p><b>DeepSeek:</b> "+(g_deepseekKey.length()?"<span class=ok>tersimpan</span> <code>"+maskKey(g_deepseekKey)+"</code>":"<span class=warn>kosong</span>")+"</p>"
  + "<p><b>OpenRouter:</b> "+(g_openrouterKey.length()?"<span class=ok>tersimpan</span> <code>"+maskKey(g_openrouterKey)+"</code>":"<span class=warn>kosong</span>")+"</p>"
  + "<p><b>Custom (bandelbanget.xyz):</b> "+(g_customUrl.length()?"<span class=ok>"+g_customUrl+"</span> <code>"+maskKey(g_customKey)+"</code>":"<span class=warn>kosong</span>")+"</p>"
  + "<form method=POST action=/admin/keys><label>DeepSeek API Key<br><input name=ds_key placeholder='sk-...' value=''></label><br><label>OpenRouter API Key<br><input name=or_key placeholder='sk-or-...' value=''></label><br><label>Custom Base URL (ex: https://bandelbanget.xyz/v1)<br><input name=custom_url placeholder='https://bandelbanget.xyz/v1' value=''></label><br><label>Custom API Key<br><input name=custom_key placeholder='sk-custom-...' value=''></label><br><button>Simpan Keys</button> <small>custom pakai model <code>custom-*</code> atau <code>bandel-*</code></small></form></div>"
  + "<div class=card><h2>API</h2><p><code>GET /health</code> · <code>GET /v1/models</code> · <code>POST /v1/chat/completions</code> · <code>GET /admin/status</code></p><p><small>Contoh OpenAI SDK: <code>OPENAI_BASE_URL=http://"+ip+"/v1</code></small></p></div>"
  + "</body></html>";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"text/html",html);
}
void handleKeysPost(){
  String ds=server.arg("ds_key"); String orK=server.arg("or_key");
  String cu=server.arg("custom_url"); String ck=server.arg("custom_key");
  if(ds.length()){ saveKey("ds_key", ds); g_deepseekKey=ds; }
  if(orK.length()){ saveKey("or_key", orK); g_openrouterKey=orK; }
  if(cu.length()){ if(cu.endsWith("/")) cu=cu.substring(0,cu.length()-1); saveKey("custom_url", cu); g_customUrl=cu; }
  if(ck.length()){ saveKey("custom_key", ck); g_customKey=ck; }
  server.sendHeader("Location","/"); server.send(303,"","");
}
void handleTokenGen(){
  String t=genToken(32);
  saveKey("local_token", t); g_localToken=t;
  server.sendHeader("Location","/"); server.send(303,"","");
}
void handleTokenClear(){
  saveKey("local_token",""); g_localToken="";
  server.sendHeader("Location","/"); server.send(303,"","");
}

void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n=== ESP32 Router Arduino + Dashboard ===");
  loadConfig();
  Serial.printf("local_token %s ds %s or %s\n", g_localToken.length()?"set":"open", g_deepseekKey.length()?"set":"-", g_openrouterKey.length()?"set":"-");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  int t=0; while(WiFi.status()!=WL_CONNECTED && t<30){ delay(500); Serial.print("."); t++; }
  if(WiFi.status()==WL_CONNECTED) Serial.printf("\nWiFi OK IP %s RSSI %d heap %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
  else Serial.printf("\nWiFi FAIL %d\n", WiFi.status());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/v1/models", HTTP_GET, handleModels);
  server.on("/admin/status", HTTP_GET, handleHealth);
  server.on("/v1/chat/completions", HTTP_POST, handleChat);
  server.on("/admin/keys", HTTP_POST, handleKeysPost);
  server.on("/admin/token/generate", HTTP_POST, handleTokenGen);
  server.on("/admin/token/clear", HTTP_POST, handleTokenClear);
  server.on("/v1/chat/completions", HTTP_OPTIONS, handleOptions);
  server.on("/health", HTTP_OPTIONS, handleOptions);
  server.on("/v1/models", HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);
  const char* hk[]={"Authorization"}; server.collectHeaders(hk,1);
  server.begin();
  Serial.printf("HTTP :80 dashboard http://%s/ heap %d\n", WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
}
void loop(){
  server.handleClient();
  static unsigned long last=0;
  if(millis()-last>10000){ last=millis(); Serial.printf("heartbeat uptime=%lus heap=%d wifi=%d ip=%s token=%s\n", millis()/1000, ESP.getFreeHeap(), WiFi.status()==WL_CONNECTED, WiFi.localIP().toString().c_str(), g_localToken.length()?"set":"open"); }
}

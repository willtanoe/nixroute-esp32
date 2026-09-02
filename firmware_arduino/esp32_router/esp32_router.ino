// ESP32-WROOM-32 AI API Router — Arduino (DOIT V1) — 9router-style Dashboard
// Board: DOIT ESP32 DEVKIT V1, Flash 4MB, Upload 115200
// Dashboard: http://<IP>/  (login default 123456, bisa ganti)
// Dynamic providers: add / remove / fetch-models / set (upsert), route by model prefix.

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>

#define MAX_PROVIDERS 8
#define MAX_MODELS_CACHE 120

struct Provider { String id, name, url, key; };

Preferences prefs;
String g_wifiSsid, g_wifiPass, g_localToken, g_adminPass;
Provider g_providers[MAX_PROVIDERS];
int g_providerCount = 0;
String g_providerModels[MAX_PROVIDERS]; // comma-separated cached model ids
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

// ---- Provider helpers ----
String apiRoot(const String& url){
  String u=url;
  if(u.endsWith("/chat/completions")) u=u.substring(0,u.length()-17);
  while(u.length() && u.endsWith("/")) u=u.substring(0,u.length()-1);
  if(!u.endsWith("/v1")) u+="/v1";
  return u;
}
int findProvider(const String& id){
  for(int i=0;i<g_providerCount;i++) if(g_providers[i].id==id) return i;
  return -1;
}
void saveProviders(){
  JsonDocument doc;
  JsonArray arr=doc.to<JsonArray>();
  for(int i=0;i<g_providerCount;i++){
    JsonObject o=arr.add<JsonObject>();
    o["id"]=g_providers[i].id; o["name"]=g_providers[i].name;
    o["url"]=g_providers[i].url; o["key"]=g_providers[i].key;
  }
  String raw; serializeJson(doc,raw);
  prefs.begin("gateway",false); prefs.putString("providers",raw); prefs.end();
}
void loadProviders(){
  prefs.begin("gateway",true);
  String raw=prefs.getString("providers","");
  prefs.end();
  g_providerCount=0;
  if(raw.length()){
    JsonDocument doc;
    if(!deserializeJson(doc,raw)){
      JsonArray arr=doc.as<JsonArray>();
      for(JsonVariant v:arr){
        if(g_providerCount>=MAX_PROVIDERS) break;
        JsonObject o=v.as<JsonObject>();
        Provider& p=g_providers[g_providerCount++];
        p.id=o["id"]|""; p.name=o["name"]|p.id.c_str(); p.url=o["url"]|""; p.key=o["key"]|"";
      }
    }
  }
  for(int i=0;i<g_providerCount;i++){
    String k="models_"+g_providers[i].id;
    prefs.begin("gateway",true);
    g_providerModels[i]=prefs.getString(k.c_str(),"");
    prefs.end();
  }
}
// migrate legacy single-provider keys (ds_key/or_key/custom_*) on first boot
void migrateLegacy(){
  if(g_providerCount>0) return;
  prefs.begin("gateway",true);
  String ds=prefs.getString("ds_key",""), orK=prefs.getString("or_key","");
  String cu=prefs.getString("custom_url",""), ck=prefs.getString("custom_key","");
  prefs.end();
  bool changed=false;
  if(ds.length() && g_providerCount<MAX_PROVIDERS){
    Provider& p=g_providers[g_providerCount++];
    p.id="deepseek"; p.name="DeepSeek"; p.url="https://api.deepseek.com"; p.key=ds; changed=true;
  }
  if(orK.length() && g_providerCount<MAX_PROVIDERS){
    Provider& p=g_providers[g_providerCount++];
    p.id="openrouter"; p.name="OpenRouter"; p.url="https://openrouter.ai/api/v1"; p.key=orK; changed=true;
  }
  if(cu.length() && ck.length() && g_providerCount<MAX_PROVIDERS){
    Provider& p=g_providers[g_providerCount++];
    p.id="custom"; p.name="Custom"; p.url=cu; p.key=ck; changed=true;
  }
  if(changed) saveProviders();
}
int countModels(int idx){
  String l=g_providerModels[idx]; if(!l.length()) return 0;
  int n=1; for(unsigned i=0;i<l.length();i++) if(l[i]==',') n++;
  return n;
}
String previewModels(int idx,int maxLen){
  String l=g_providerModels[idx];
  if(l.length()>(unsigned)maxLen) return l.substring(0,maxLen)+" …";
  return l;
}
bool modelInProvider(int idx,const String& model){
  String l=g_providerModels[idx];
  int start=0;
  while(start<(int)l.length()){
    int comma=l.indexOf(',',start);
    String m=(comma<0)?l.substring(start):l.substring(start,comma);
    if(m==model) return true;
    if(comma<0) break;
    start=comma+1;
  }
  return false;
}
int routeProvider(const String& model){
  // 1. prefix: "<id>-" or "<id>/"
  for(int i=0;i<g_providerCount;i++){
    String p=g_providers[i].id+"-", s=g_providers[i].id+"/";
    if(model.startsWith(p)||model.startsWith(s)) return i;
  }
  // 2. exact match against cached models
  for(int i=0;i<g_providerCount;i++) if(modelInProvider(i,model)) return i;
  // 3. default: first provider with a key
  for(int i=0;i<g_providerCount;i++) if(g_providers[i].key.length()) return i;
  return g_providerCount?0:-1;
}
int fetchModels(int idx){
  String root=apiRoot(g_providers[idx].url);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.begin(client, root+"/models");
  http.addHeader("Authorization","Bearer "+g_providers[idx].key);
  http.setTimeout(15000);
  int code=http.GET();
  String body=http.getString();
  http.end();
  if(code<200||code>=300) return -1;
  JsonDocument doc;
  JsonDocument filter; filter["data"][0]["id"]=true;
  DeserializationError err=deserializeJson(doc,body,DeserializationOption::Filter(filter));
  if(err) return -2;
  String ids=""; int count=0;
  JsonArray data=doc["data"].as<JsonArray>();
  for(JsonVariant v:data){
    const char* id=v["id"];
    if(id){
      if(ids.length()) ids+=",";
      ids+=id;
      if(++count>=MAX_MODELS_CACHE) break;
    }
  }
  g_providerModels[idx]=ids;
  String k="models_"+g_providers[idx].id;
  prefs.begin("gateway",false); prefs.putString(k.c_str(),ids); prefs.end();
  return count;
}

void loadConfig(){
  prefs.begin("gateway",true);
  g_wifiSsid=prefs.getString("wifi_ssid","");
  g_wifiPass=prefs.getString("wifi_pass","");
  g_localToken=prefs.getString("local_token","");
  g_adminPass=prefs.getString("admin_pass","123456");
  prefs.end();
  loadProviders();
  migrateLegacy();
}
void saveKey(const char* k,const String& v){
  prefs.begin("gateway",false); prefs.putString(k,v); prefs.end();
}
bool isAuthenticated(){
  if(!server.hasHeader("Cookie")) return false;
  return server.header("Cookie").indexOf("esp_auth=ok")>=0;
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
void sendJson(int code,const String& j){
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
    +",\"local_token_set\":"+(g_localToken.length()?"true":"false")+",\"providers\":"+g_providerCount+"}";
  sendJson(200,j);
}
void handleModels(){
  if(!authCheck()){ sendJson(401,"{\"error\":{\"message\":\"unauthorized\"}}"); return; }
  JsonDocument doc;
  JsonObject root=doc.to<JsonObject>();
  root["object"]="list";
  JsonArray data=root["data"].to<JsonArray>();
  for(int i=0;i<g_providerCount;i++){
    String l=g_providerModels[i];
    if(l.length()){
      int start=0;
      while(start<(int)l.length()){
        int comma=l.indexOf(',',start);
        String m=(comma<0)?l.substring(start):l.substring(start,comma);
        JsonObject mo=data.add<JsonObject>();
        mo["id"]=m; mo["object"]="model"; mo["owned_by"]=g_providers[i].id;
        if(comma<0) break;
        start=comma+1;
      }
    } else {
      JsonObject mo=data.add<JsonObject>();
      mo["id"]=g_providers[i].id+"-auto"; mo["object"]="model"; mo["owned_by"]=g_providers[i].id;
    }
  }
  String out; serializeJson(doc,out);
  sendJson(200,out);
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
  String model="";
  int mi=body.indexOf("\"model\"");
  if(mi>=0){ int q1=body.indexOf("\"",mi+7); int q2=body.indexOf("\"",q1+1); if(q1>0&&q2>q1) model=body.substring(q1+1,q2); }
  int idx=routeProvider(model);
  if(idx<0){ reqFail++; sendJson(500,"{\"error\":{\"message\":\"no provider configured — buka dashboard /dashboard/providers\"}}"); return; }
  Provider& p=g_providers[idx];
  if(p.key.length()==0){ reqFail++; sendJson(500,"{\"error\":{\"message\":\"provider \\\""+p.id+"\\\" has no API key — set it in dashboard\"}}"); return; }
  String url=apiRoot(p.url)+"/chat/completions";
  bool isStream=body.indexOf("\"stream\":true")>=0 || body.indexOf("\"stream\": true")>=0;
  WiFiClientSecure *client=new WiFiClientSecure; client->setInsecure();
  HTTPClient https; https.begin(*client,url);
  https.addHeader("Content-Type","application/json");
  https.addHeader("Authorization","Bearer "+p.key);
  https.addHeader("Accept",isStream?"text/event-stream":"application/json");
  if(p.id=="openrouter"){ https.addHeader("HTTP-Referer","http://"+WiFi.localIP().toString()); https.addHeader("X-Title","ESP32 Router"); }
  https.setTimeout(20000);
  int code=https.POST(body);
  String resp=https.getString();
  https.end(); delete client;
  if(code>=200 && code<300){
    reqOk++;
    if(isStream){ server.sendHeader("Access-Control-Allow-Origin","*"); server.sendHeader("Cache-Control","no-cache"); server.send(200,"text/event-stream",resp); }
    else sendJson(200,resp.length()?resp:"{}");
  } else {
    reqFail++;
    if(resp.length() && resp[0]=='{') sendJson(code>0?code:502,resp);
    else sendJson(code>0?code:502,String("{\"error\":{\"message\":\"upstream_error code ")+code+"\"}}");
  }
  Serial.printf("chat model=%s -> %s code=%d heap=%d\n",model.c_str(),p.id.c_str(),code,ESP.getFreeHeap());
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
</style></head><body><div class=card><div class=logo><svg viewBox="0 0 500 500" width="36" height="36"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#fff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg><div><div style="font-weight:800;letter-spacing:.2px">NixRoute</div><div style="font-size:11px;color:var(--c-muted)">ESP32 Router</div></div></div><h1>Masuk Dashboard</h1><p class=sub>Password default <code>123456</code></p><form method=POST action=/admin/login><label>Password</label><input class=input name=password type=password placeholder="••••••" required autofocus><button class=btn>Masuk</button></form><p class=hint>Ganti password di Settings setelah login untuk keamanan</p></div></body></html>)HTML";
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

// ---- Dashboard SPA (section-based) ----
void handleRoot(){
  if(!isAuthenticated()){
    server.sendHeader("Location","/login");
    server.send(303,"","");
    return;
  }
  String ip=WiFi.localIP().toString();
  bool conn=WiFi.status()==WL_CONNECTED;
  bool apMode=(WiFi.getMode()==WIFI_AP);
  String tokenMask=g_localToken.length()?maskKey(g_localToken):"(open)";

  String providersHtml="";
  for(int i=0;i<g_providerCount;i++){
    Provider& p=g_providers[i];
    int mc=countModels(i);
    providersHtml+="<div class=card style='margin:0'>";
    providersHtml+="<div class=row><span class=badge>"+p.name+"</span><div class=mono>"+p.url+"</div><span class=small>key "+(p.key.length()?maskKey(p.key):"kosong")+"</span></div>";
    providersHtml+="<div class=row><span class=small>id <code>"+p.id+"</code> · "+String(mc)+" models</span>";
    providersHtml+="<form method=POST action=/admin/providers/fetch style='display:inline'><input type=hidden name=id value='"+p.id+"'><button class='btn ghost'>Fetch Models</button></form>";
    providersHtml+="<form method=POST action=/admin/providers/remove style='display:inline' onsubmit=\"return confirm('Hapus provider "+p.id+"?')\"><input type=hidden name=id value='"+p.id+"'><button class='btn ghost'>Remove</button></form></div>";
    if(mc) providersHtml+="<div class=small style='word-break:break-all'>"+previewModels(i,200)+"</div>";
    else providersHtml+="<div class=small>belum fetch models — routing pakai prefix <code>"+p.id+"-*</code></div>";
    providersHtml+="</div>";
  }

  String html = String(R"HTML(<!doctype html><html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>NixRoute ESP32 v1.1.0</title><link rel="icon" href="/favicon.svg"><style>
*{scroll-behavior:smooth}
*{box-sizing:border-box}body{margin:0;font-family:"IBM Plex Sans",system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--c-bg);color:var(--c-text)}
:root{--c-bg:#f5f8fc;--c-surface:#ffffff;--c-surface-2:#f0f4f8;--c-border:#c7d3e0;--c-border-sub:#dde6ef;--c-text:#0c1a30;--c-muted:#52657d;--c-subtle:#7d8da2;--c-primary:#00a8b5;--c-primary-h:#008b97;--c-data-soft:#e8fbfc;--r:8px;--r-lg:12px;--sh:0 1px 2px rgba(12,26,48,.06);--sh-el:0 8px 24px rgba(12,26,48,.12)}
.dark{--c-bg:#07111f;--c-surface:#0f2038;--c-surface-2:#152942;--c-border:#29435f;--c-border-sub:#1f3752;--c-text:#f4f8fc;--c-muted:#a9bacd;--c-subtle:#7489a2;--c-primary:#18bec9;--c-data-soft:#103943;}
.shell{display:flex;min-height:100vh}
.rail{width:72px;background:var(--c-surface);border-right:1px solid var(--c-border-sub);display:flex;flex-direction:column;align-items:center;padding:14px 0;gap:6px;position:sticky;top:0;height:100vh}
.rail a{width:56px;height:56px;border-radius:12px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px;font-size:10px;font-weight:600;letter-spacing:.2px;color:var(--c-muted);text-decoration:none}
.rail a.on{background:var(--c-data-soft);color:var(--c-primary);border:1px solid #c9f3f6}
.dark .rail a.on{border-color:#0f4a52}
.rail a:hover{background:var(--c-surface-2);color:var(--c-text)}
.main{flex:1;min-width:0;background:var(--c-bg)}
.header{position:sticky;top:0;background:var(--c-surface);border-bottom:1px solid var(--c-border-sub);padding:14px 16px;display:flex;justify-content:space-between;align-items:center;backdrop-filter:saturate(1.2)}
.logo{display:flex;align-items:center;gap:10px;font-weight:800;letter-spacing:.2px}
.tbtn{width:36px;height:36px;border-radius:10px;border:1px solid var(--c-border);background:var(--c-surface);font-size:16px;cursor:pointer;display:flex;align-items:center;justify-content:center;color:var(--c-text)}
.tbtn:hover{background:var(--c-surface-2)}
.sep{width:40px;height:1px;background:var(--c-border-sub);margin:6px 0}
.wrap{max-width:880px;margin:18px auto;padding:0 16px}
.page{display:none}.page.on{display:flex;flex-direction:column;gap:14px}
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
<a href="/dashboard/endpoint" data-page="endpoint" onclick="nav('endpoint');return false"><span style="width:28px;height:28px;display:flex;align-items:center;justify-content:center"><svg viewBox="0 0 500 500" width="28" height="28"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#fff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#fff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg></span>Routes</a>
<a href="/dashboard/providers" data-page="providers" onclick="nav('providers');return false"><span>🗄️</span>Providers</a>
<a href="/dashboard/policies" data-page="policies" onclick="nav('policies');return false"><span>🧩</span>Policies</a>
<a href="/dashboard/usage" data-page="usage" onclick="nav('usage');return false"><span>📊</span>Observe</a>
<a href="/dashboard/tools" data-page="tools" onclick="nav('tools');return false"><span>🔧</span>Tools</a>
<div class=sep></div>
<a href="/dashboard/settings" data-page="settings" onclick="nav('settings');return false"><span>⚙️</span>Settings</a>
<div style="flex:1"></div>
<a href="/admin/logout" style="color:#888;font-size:10px">Logout</a>
</aside><div class=main><div class=header><div class=logo>NixRoute ESP32 <span style="font-weight:400;color:#666">— nixroute 1.1.0</span></div><div style="display:flex;align-items:center;gap:10px"><div style="font-size:12px;color:#666">)HTML") + ip + R"HTML(</div><button class=tbtn onclick="toggleTheme()" title="Dark mode">🌓</button></div></div><div class=wrap>)HTML"
  + "<section class=\"page\" id=\"page-endpoint\"><div class=card><h2>◉ API Endpoint — Routes</h2>"
  + "<div class=row><span class='badge on'>Local</span><div class=mono>http://"+ip+"/v1</div><button class='btn ghost' onclick=\"copyEndpoint('http://"+ip+"/v1')\">Copy</button></div>"
  + "<div class=row><span class=badge>WiFi</span><div class=mono>"+(apMode?("AP mode · "+ip):(conn?ip+" · RSSI "+WiFi.RSSI()+"dBm":"disconnected"))+"</div><span class='small "+(conn||apMode?"ok":"warn")+"'>"+(apMode?"setup AP":(conn?"connected":"disconnected"))+"</span></div>"
  + "<div class=kv><div><b>Uptime</b> "+(millis()/1000)+"s</div><div><b>Heap</b> "+ESP.getFreeHeap()+"</div><div><b>Requests</b> "+reqTotal+" ("+reqOk+" ok / "+reqFail+" fail)</div><div><b>Providers</b> "+g_providerCount+"</div></div></div></section>"

  + "<section class=\"page\" id=\"page-providers\"><div class=card><h2>🔑 API Keys — Require API key: "+String(g_localToken.length()?"ON":"OFF")+"</h2>"
  + "<div class=row><span class=badge>Local Token</span><div class=mono>"+tokenMask+"</div><form method=POST action=/admin/token/generate style='display:inline'><button class=btn>Generate</button></form><form method=POST action=/admin/token/clear style='display:inline'><button class='btn ghost'>Clear</button></form></div>"
  + (g_localToken.length()?"<div class=mono style='font-size:11px;word-break:break-all'>"+g_localToken+"</div>":"<div class=small>Open — client tidak perlu Authorization</div>")
  + "</div>"

  + "<div class=card><h2>🔌 Providers</h2>"
  + providersHtml
  + "<form method=POST action=/admin/providers/add>"
  + "<input class=input name=id placeholder='id slug (mis. deepseek)'>"
  + "<input class=input name=name placeholder='Nama (mis. DeepSeek)'>"
  + "<input class=input name=url placeholder='Base URL (mis. https://api.deepseek.com)'>"
  + "<input class=input name=key placeholder='API Key sk-...'>"
  + "<button class=btn>Add / Update Provider</button> <span class=small>id yang sudah ada akan di-update (set)</span>"
  + "</form></div></section>"

  + "<section class=\"page\" id=\"page-policies\"><div class=card><h2>🧩 Policies — Routing</h2><div class=small>Routing model → provider: (1) prefix <code>&lt;id&gt;-*</code> / <code>&lt;id&gt;/*</code>, (2) exact match hasil fetch models, (3) fallback provider pertama yang punya key.</div><div class=small>Contoh: model <code>deepseek-chat</code> → provider <code>deepseek</code>; <code>openrouter-auto</code> → <code>openrouter</code>.</div></div></section>"

  + "<section class=\"page\" id=\"page-usage\"><div class=card><h2>📊 Observe — Usage</h2><div class=kv><div><b>Total</b> "+reqTotal+"</div><div><b>OK</b> "+reqOk+"</div><div><b>Fail</b> "+reqFail+"</div><div><b>Heap</b> "+ESP.getFreeHeap()+"</div></div><div class=small>GET <code>/health</code> · <code>/admin/status</code> untuk JSON</div></div></section>"

  + "<section class=\"page\" id=\"page-tools\"><div class=card><h2>🔧 Tools</h2><div class=small><code>curl http://"+ip+"/health</code> · <code>curl -H \"Authorization: Bearer TOKEN\" http://"+ip+"/v1/models</code> · <code>POST /v1/chat/completions</code></div><div class=small>OpenAI SDK: <code>OPENAI_BASE_URL=http://"+ip+"/v1</code></div></div></section>"

  + "<section class=\"page\" id=\"page-settings\"><div class=card><h2>📶 Wi-Fi</h2>"
  + "<div class=small>"+(apMode?"Mode AP aktif — set SSID/password lalu Save untuk reconnect ke Wi-Fi rumah.":"SSID: <code>"+g_wifiSsid+"</code>")+"</div>"
  + "<form method=POST action=/admin/wifi><div class=row><input class=input name=ssid placeholder='WiFi SSID' value='"+g_wifiSsid+"'><input class=input name=pass placeholder='WiFi Password'><button class=btn>Save & Reconnect</button></div></form>"
  + "</div>"
  + "<div class=card><h2>🛡 Settings</h2>"
  + "<form method=POST action=/admin/password><div class=row><input class=input name=new_pass placeholder='Ganti password dashboard (default 123456)'><button class=btn>Update Password</button></div></form>"
  + "</div>"
  + "<div class=card><h2>ℹ️ About</h2><div class=kv><div><b>Version</b> nixroute 1.1.0</div><div><b>IP</b> "+ip+"</div><div><b>Uptime</b> "+(millis()/1000)+"s</div><div><b>Heap</b> "+ESP.getFreeHeap()+"</div></div></div></section>"

  + "<script>"
  + "function getPage(){var p=location.pathname.split('/').pop();if(!p||p==='dashboard'||p==='endpoint')return 'endpoint';return p;}"
  + "function showPage(p){var s=document.querySelectorAll('.page');for(var i=0;i<s.length;i++)s[i].classList.remove('on');var el=document.getElementById('page-'+p)||document.getElementById('page-endpoint');if(el)el.classList.add('on');var a=document.querySelectorAll('.rail a[data-page]');for(var j=0;j<a.length;j++){if(a[j].getAttribute('data-page')===p)a[j].classList.add('on');else a[j].classList.remove('on');}}"
  + "function nav(p){try{history.pushState({page:p},'','/dashboard/'+p)}catch(e){location.href='/dashboard/'+p}showPage(p);}"
  + "function toggleTheme(){var d=document.documentElement.classList.toggle('dark');try{localStorage.setItem('nixroute-theme',d?'dark':'light')}catch(e){}}"
  + "function copyEndpoint(t){try{navigator.clipboard.writeText(t)}catch(e){prompt('Copy:',t)}}"
  + "window.addEventListener('popstate',function(){showPage(getPage())});"
  + "try{if(localStorage.getItem('nixroute-theme')==='dark'||(!localStorage.getItem('nixroute-theme')&&matchMedia('(prefers-color-scheme:dark)').matches))document.documentElement.classList.add('dark')}catch(e){}"
  + "showPage(getPage());"
  + "</script>"
  + "</div></div></body></html>";
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"text/html",html);
}

// ---- Admin: providers ----
void handleProviderAdd(){
  if(!isAuthenticated()){ server.send(401,"text/plain","unauthorized"); return; }
  String id=server.arg("id"), name=server.arg("name"), url=server.arg("url"), key=server.arg("key");
  id.trim(); name.trim(); url.trim(); key.trim();
  if(id.length() && url.length()){
    int idx=findProvider(id);
    if(idx<0){
      if(g_providerCount>=MAX_PROVIDERS){ server.sendHeader("Location","/dashboard/providers"); server.send(303,"",""); return; }
      idx=g_providerCount++;
      g_providers[idx].id=id;
    }
    if(name.length()) g_providers[idx].name=name;
    g_providers[idx].url=url;
    if(key.length()) g_providers[idx].key=key;
    saveProviders();
  }
  server.sendHeader("Location","/dashboard/providers"); server.send(303,"","");
}
void handleProviderRemove(){
  if(!isAuthenticated()){ server.send(401,"text/plain","unauthorized"); return; }
  String id=server.arg("id");
  int idx=findProvider(id);
  if(idx>=0){
    for(int i=idx;i<g_providerCount-1;i++) g_providers[i]=g_providers[i+1];
    g_providerCount--;
    saveProviders();
    prefs.begin("gateway",false); prefs.remove(("models_"+id).c_str()); prefs.end();
  }
  server.sendHeader("Location","/dashboard/providers"); server.send(303,"","");
}
void handleProviderFetch(){
  if(!isAuthenticated()){ server.send(401,"text/plain","unauthorized"); return; }
  String id=server.arg("id");
  int idx=findProvider(id);
  if(idx>=0){ int n=fetchModels(idx); Serial.printf("fetch models %s -> %d\n",id.c_str(),n); }
  server.sendHeader("Location","/dashboard/providers"); server.send(303,"","");
}
void handleTokenGen(){ String t=genToken(32); saveKey("local_token",t); g_localToken=t; server.sendHeader("Location","/dashboard/providers"); server.send(303,"",""); }
void handleTokenClear(){ saveKey("local_token",""); g_localToken=""; server.sendHeader("Location","/dashboard/providers"); server.send(303,"",""); }
void handlePasswordPost(){
  if(!isAuthenticated()){ server.send(401,"text/plain","unauthorized"); return; }
  String np=server.arg("new_pass");
  if(np.length()>=3){ saveKey("admin_pass",np); g_adminPass=np; }
  server.sendHeader("Location","/dashboard/settings"); server.send(303,"","");
}
void handleWifiPost(){
  if(!isAuthenticated()){ server.send(401,"text/plain","unauthorized"); return; }
  String ssid=server.arg("ssid"), pass=server.arg("pass");
  if(ssid.length()){
    prefs.begin("gateway",false);
    prefs.putString("wifi_ssid",ssid);
    prefs.putString("wifi_pass",pass);
    prefs.end();
    g_wifiSsid=ssid; g_wifiPass=pass;
    server.sendHeader("Location","/dashboard/settings");
    server.send(303,"","");
    delay(200);
    ESP.restart();
    return;
  }
  server.sendHeader("Location","/dashboard/settings"); server.send(303,"","");
}

void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n=== NixRoute ESP32 v1.1.0 ===");
  loadConfig();
  Serial.printf("admin %s local %s wifi %s providers %d\n", g_adminPass.c_str(), g_localToken.length()?"set":"open", g_wifiSsid.length()?g_wifiSsid.c_str():"(none)", g_providerCount);

  if(g_wifiSsid.length()){
    WiFi.mode(WIFI_STA); WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
    Serial.print("Connecting WiFi");
    int t=0; while(WiFi.status()!=WL_CONNECTED && t<30){ delay(500); Serial.print("."); t++; }
    if(WiFi.status()==WL_CONNECTED) Serial.printf("\nWiFi OK IP %s RSSI %d heap %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
    else Serial.printf("\nWiFi FAIL %d\n", WiFi.status());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("NixRoute-Setup","12345678");
    Serial.printf("No WiFi configured — AP mode 'NixRoute-Setup' (pw 12345678) IP %s\n", WiFi.softAPIP().toString().c_str());
  }

  server.on("/favicon.svg", HTTP_GET, handleFavicon);
  server.on("/nixroute.svg", HTTP_GET, handleNixrouteSvg);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleRoot);
  server.on("/dashboard/endpoint", HTTP_GET, handleRoot);
  server.on("/dashboard/providers", HTTP_GET, handleRoot);
  server.on("/dashboard/policies", HTTP_GET, handleRoot);
  server.on("/dashboard/usage", HTTP_GET, handleRoot);
  server.on("/dashboard/tools", HTTP_GET, handleRoot);
  server.on("/dashboard/settings", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/admin/login", HTTP_POST, handleLoginPost);
  server.on("/admin/logout", HTTP_GET, handleLogout);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/v1/models", HTTP_GET, handleModels);
  server.on("/admin/status", HTTP_GET, handleHealth);
  server.on("/v1/chat/completions", HTTP_POST, handleChat);
  server.on("/admin/providers/add", HTTP_POST, handleProviderAdd);
  server.on("/admin/providers/remove", HTTP_POST, handleProviderRemove);
  server.on("/admin/providers/fetch", HTTP_POST, handleProviderFetch);
  server.on("/admin/token/generate", HTTP_POST, handleTokenGen);
  server.on("/admin/token/clear", HTTP_POST, handleTokenClear);
  server.on("/admin/password", HTTP_POST, handlePasswordPost);
  server.on("/admin/wifi", HTTP_POST, handleWifiPost);
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

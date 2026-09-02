// ESP32-WROOM-32 AI API Router — Arduino IDE version (SuprimX)
// Board: ESP32 Dev Module (WROOM-32), Upload Speed 115200, Flash 4MB
// Libs: ArduinoJson (install via Library Manager)
// Isi WIFI_SSID/PASS dan DEEPSEEK_KEY di bawah, lalu Upload di Arduino IDE.

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// === CONFIG — GANTI DISINI ===
const char* WIFI_SSID = "SuprimX";
const char* WIFI_PASS = "wooting60he+";
const char* LOCAL_TOKEN = ""; // kosong = open, isi = butuh Authorization: Bearer <token>
const char* DEEPSEEK_API_KEY = ""; // isi sk-... kalau kosong proxy akan 500
const char* DEEPSEEK_URL = "https://api.deepseek.com/v1/chat/completions";
// ==============================

WebServer server(80);
unsigned long bootMs;
uint32_t reqTotal=0, reqOk=0, reqFail=0;

bool authCheck() {
  if (LOCAL_TOKEN[0]=='\0') return true;
  if (!server.hasHeader("Authorization")) return false;
  String h = server.header("Authorization");
  String need = String("Bearer ") + LOCAL_TOKEN;
  if (h.length()!=need.length()) return false;
  // constant-time
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
  char ip[16]; String s = WiFi.localIP().toString();
  s.toCharArray(ip,16);
  bool conn = WiFi.status()==WL_CONNECTED;
  String j = String("{\"status\":\"") + (conn?"ok":"wifi_disconnected") + "\",\"uptime_s\":" + (millis()/1000)
           + ",\"wifi_connected\":" + (conn?"true":"false") + ",\"ip\":\"" + s + "\",\"rssi\":" + WiFi.RSSI()
           + ",\"free_heap\":" + ESP.getFreeHeap() + ",\"requests_total\":" + reqTotal + "}";
  sendJson(200,j);
}

void handleModels(){
  if(!authCheck()){ sendJson(401,"{\"error\":{\"message\":\"unauthorized\"}}"); return; }
  sendJson(200,"{\"object\":\"list\",\"data\":[{\"id\":\"deepseek-chat\",\"object\":\"model\",\"owned_by\":\"deepseek\"},{\"id\":\"deepseek-reasoner\",\"object\":\"model\",\"owned_by\":\"deepseek\"}]}");
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
  if(String(DEEPSEEK_API_KEY)==""){ reqFail++; sendJson(500,"{\"error\":{\"message\":\"provider API key not configured (isi DEEPSEEK_API_KEY di .ino)\",\"type\":\"config\"}}"); return; }
  if(!server.hasArg("plain")){
    // WebServer arg plain = body
  }
  String body = server.arg("plain");
  if(body.length()==0){ reqFail++; sendJson(400,"{\"error\":{\"message\":\"empty body\"}}"); return; }
  if(body.length()>8192){ reqFail++; sendJson(413,"{\"error\":{\"message\":\"payload too large\"}}"); return; }

  // extract stream flag (simple)
  bool isStream = body.indexOf("\"stream\":true")>=0 || body.indexOf("\"stream\": true")>=0;

  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure(); // pakai bundle penuh di Arduino IDE berat; insecure dulu untuk test LAN (ganti setCACert kalau mau strict)
  HTTPClient https;
  https.begin(*client, DEEPSEEK_URL);
  https.addHeader("Content-Type","application/json");
  https.addHeader("Authorization", String("Bearer ")+DEEPSEEK_API_KEY);
  https.addHeader("Accept", isStream?"text/event-stream":"application/json");
  https.setTimeout(15000);

  int code = https.POST(body);
  String resp = https.getString();
  String ctype = https.header("Content-Type");
  https.end(); delete client;

  if(code>=200 && code<300){
    reqOk++;
    if(isStream){
      server.sendHeader("Access-Control-Allow-Origin","*");
      server.sendHeader("Cache-Control","no-cache");
      server.sendHeader("Connection","keep-alive");
      server.send(200, "text/event-stream", resp);
    } else {
      sendJson(200, resp.length()?resp:"{}");
    }
  } else {
    reqFail++;
    if(resp.length() && resp[0]=='{') sendJson(code>0?code:502, resp);
    else sendJson(code>0?code:502, String("{\"error\":{\"message\":\"upstream_error code ")+code+"\",\"type\":\"upstream\"}}");
  }
  Serial.printf("chat len=%d code=%d heap=%d\n", body.length(), code, ESP.getFreeHeap());
}

void handleNotFound(){
  sendJson(404,"{\"error\":{\"message\":\"not found\",\"type\":\"not_found\"}}");
}

void setup(){
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32 Router Arduino ===");
  Serial.printf("SSID %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  int tries=0;
  while(WiFi.status()!=WL_CONNECTED && tries<30){
    delay(500); Serial.print("."); tries++;
  }
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("\nWiFi OK IP %s RSSI %d heap %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap());
  } else {
    Serial.printf("\nWiFi FAIL status %d\n", WiFi.status());
  }

  server.on("/health", HTTP_GET, handleHealth);
  server.on("/v1/models", HTTP_GET, handleModels);
  server.on("/admin/status", HTTP_GET, handleHealth);
  server.on("/v1/chat/completions", HTTP_POST, handleChat);
  server.on("/v1/chat/completions", HTTP_OPTIONS, handleOptions);
  server.on("/health", HTTP_OPTIONS, handleOptions);
  server.on("/v1/models", HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);
  const char* hdrKeys[] = {"Authorization"};
  server.collectHeaders(hdrKeys, 1);
  server.begin();
  Serial.printf("HTTP :80 started heap %d\n", ESP.getFreeHeap());
  bootMs=millis();
}

void loop(){
  server.handleClient();
  static unsigned long last=0;
  if(millis()-last>10000){
    last=millis();
    Serial.printf("heartbeat uptime=%lus heap=%d wifi=%d ip=%s\n", millis()/1000, ESP.getFreeHeap(), WiFi.status()==WL_CONNECTED, WiFi.localIP().toString().c_str());
  }
}

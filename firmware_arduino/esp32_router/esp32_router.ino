// ESP32-WROOM-32 — AI API Router (Arduino, DOIT DEVKIT V1)
// OpenAI-compatible gateway: point any OpenAI SDK at http://<ip>/v1
// Dashboard: http://<ip>/  (login default 123456)
// Providers are OpenAI-compatible endpoints; each has name + base URL + API key.
// On save, the firmware fetches the provider's model list and namespaces each
// model as "<provider>/<model>" so routing is unambiguous.

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>

#define VERSION          "2.0.0"
#define MAX_PROVIDERS    16
#define MAX_MODELS_CACHE 200

struct Provider { String id, name, url, key; };

Preferences prefs;
String g_wifiSsid, g_wifiPass, g_localToken, g_adminPass;
Provider g_providers[MAX_PROVIDERS];
int g_providerCount = 0;
String g_providerModels[MAX_PROVIDERS];   // comma-separated raw model ids
WebServer server(80);
uint32_t reqTotal = 0, reqOk = 0, reqFail = 0;
unsigned long bootMs = 0;
String g_lastFetchError;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
String maskKey(const String& k) {
  if (k.length() == 0) return "";
  if (k.length() <= 8) return "***";
  return k.substring(0, 4) + "***" + k.substring(k.length() - 4);
}

String genToken(int len = 32) {
  static const char* cs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String s;
  s.reserve(len);
  for (int i = 0; i < len; i++) s += cs[esp_random() % 62];
  return "sk-local-" + s;
}

String slugify(const String& s) {
  String r;
  r.reserve(s.length());
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c)) {
      r += tolower((unsigned char)c);
    } else if (c == ' ' || c == '-' || c == '_' || c == '.') {
      if (r.length() && r[r.length() - 1] != '-') r += '-';
    }
  }
  while (r.length() && r[r.length() - 1] == '-') r = r.substring(0, r.length() - 1);
  if (!r.length()) r = "provider";
  return r;
}

// Normalize a base URL to its "/v1" root (strip /chat/completions, trailing slashes).
String apiRoot(const String& url) {
  String u = url;
  if (u.endsWith("/chat/completions")) u = u.substring(0, u.length() - 17);
  while (u.length() && u.endsWith("/")) u = u.substring(0, u.length() - 1);
  if (!u.endsWith("/v1")) u += "/v1";
  return u;
}

int findProvider(const String& id) {
  for (int i = 0; i < g_providerCount; i++) if (g_providers[i].id == id) return i;
  return -1;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void saveProviders() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < g_providerCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = g_providers[i].id;
    o["name"] = g_providers[i].name;
    o["url"] = g_providers[i].url;
    o["key"] = g_providers[i].key;
  }
  String raw;
  serializeJson(doc, raw);
  prefs.begin("gateway", false);
  prefs.putString("providers", raw);
  prefs.end();
}

void loadProviders() {
  prefs.begin("gateway", true);
  String raw = prefs.getString("providers", "");
  prefs.end();
  g_providerCount = 0;
  if (raw.length()) {
    JsonDocument doc;
    if (!deserializeJson(doc, raw)) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonVariant v : arr) {
        if (g_providerCount >= MAX_PROVIDERS) break;
        JsonObject o = v.as<JsonObject>();
        Provider& p = g_providers[g_providerCount++];
        p.id = o["id"] | "";
        p.name = o["name"] | p.id.c_str();
        p.url = o["url"] | "";
        p.key = o["key"] | "";
      }
    }
  }
  for (int i = 0; i < g_providerCount; i++) {
    String k = "models_" + g_providers[i].id;
    prefs.begin("gateway", true);
    g_providerModels[i] = prefs.getString(k.c_str(), "");
    prefs.end();
  }
}

void loadConfig() {
  prefs.begin("gateway", true);
  g_wifiSsid = prefs.getString("wifi_ssid", "");
  g_wifiPass = prefs.getString("wifi_pass", "");
  g_localToken = prefs.getString("local_token", "");
  g_adminPass = prefs.getString("admin_pass", "123456");
  prefs.end();
  loadProviders();
}

void saveKey(const char* k, const String& v) {
  prefs.begin("gateway", false);
  prefs.putString(k, v);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Model cache helpers
// ---------------------------------------------------------------------------
int countModels(int idx) {
  String l = g_providerModels[idx];
  if (!l.length()) return 0;
  int n = 1;
  for (unsigned i = 0; i < l.length(); i++) if (l[i] == ',') n++;
  return n;
}

bool modelInProvider(int idx, const String& model) {
  String l = g_providerModels[idx];
  int start = 0;
  while (start < (int)l.length()) {
    int comma = l.indexOf(',', start);
    String m = (comma < 0) ? l.substring(start) : l.substring(start, comma);
    if (m == model) return true;
    if (comma < 0) break;
    start = comma + 1;
  }
  return false;
}

// Returns the provider index for a model, and (optionally) the upstream model
// name with any "<provider>/" or "<provider>-" prefix stripped.
int routeProvider(const String& model, String* upstream) {
  if (upstream) *upstream = model;

  // 1. explicit "<provider>/<model>" namespace
  int slash = model.indexOf('/');
  if (slash > 0) {
    String prefix = model.substring(0, slash);
    int idx = findProvider(prefix);
    if (idx >= 0) {
      if (upstream) *upstream = model.substring(slash + 1);
      return idx;
    }
  }

  // 2. exact match against a provider's fetched models
  for (int i = 0; i < g_providerCount; i++) {
    if (modelInProvider(i, model)) return i;
  }

  // 3. "<provider>-" prefix
  for (int i = 0; i < g_providerCount; i++) {
    String p = g_providers[i].id + "-";
    if (model.startsWith(p)) {
      if (upstream) *upstream = model.substring(p.length());
      return i;
    }
  }

  // 4. fallback: first provider that has a key
  for (int i = 0; i < g_providerCount; i++) if (g_providers[i].key.length()) return i;

  return g_providerCount ? 0 : -1;
}

// Fetch a provider's model list (GET /models) and cache it. Returns model
// count (>=0) or -1 on error (message in g_lastFetchError).
int fetchModels(int idx) {
  g_lastFetchError = "";
  String root = apiRoot(g_providers[idx].url);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, root + "/models")) {
    g_lastFetchError = "cannot connect to " + root;
    return -1;
  }
  http.addHeader("Authorization", "Bearer " + g_providers[idx].key);
  http.setTimeout(20000);
  int code = http.GET();
  String body = http.getString();
  http.end();

  if (code < 200 || code >= 300) {
    g_lastFetchError = "HTTP " + String(code);
    if (body.length()) {
      String s = body.substring(0, 200);
      s.replace("\n", " ");
      g_lastFetchError = g_lastFetchError + " — " + s;
    }
    return -1;
  }

  JsonDocument doc;
  JsonDocument filter;
  filter["data"][0]["id"] = true;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    g_lastFetchError = "invalid JSON — " + String(err.c_str());
    return -2;
  }

  String ids = "";
  int count = 0;
  JsonArray data = doc["data"].as<JsonArray>();
  for (JsonVariant v : data) {
    const char* id = v["id"];
    if (id) {
      if (ids.length()) ids += ",";
      ids += id;
      if (++count >= MAX_MODELS_CACHE) break;
    }
  }
  if (count == 0) g_lastFetchError = "no models in response";

  g_providerModels[idx] = ids;
  String k = "models_" + g_providers[idx].id;
  prefs.begin("gateway", false);
  prefs.putString(k.c_str(), ids);
  prefs.end();
  return count;
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------
bool isAuthenticated() {
  if (!server.hasHeader("Cookie")) return false;
  return server.header("Cookie").indexOf("esp_auth=ok") >= 0;
}

bool authCheck() {
  if (g_localToken.length() == 0) return true;
  if (!server.hasHeader("Authorization")) return false;
  String h = server.header("Authorization");
  String need = "Bearer " + g_localToken;
  if (h.length() != need.length()) return false;
  volatile int d = 0;
  for (unsigned i = 0; i < h.length(); i++) d |= h[i] ^ need[i];
  return d == 0;
}

void sendJson(int code, const String& j) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", j);
}

void sendError(int code, const String& msg) {
  JsonDocument doc;
  doc["error"]["message"] = msg;
  String out;
  serializeJson(doc, out);
  sendJson(code, out);
}

// ---------------------------------------------------------------------------
// Public API (OpenAI-compatible)
// ---------------------------------------------------------------------------
void handleHealth() {
  String ip = WiFi.localIP().toString();
  bool conn = WiFi.status() == WL_CONNECTED;
  JsonDocument doc;
  doc["status"] = conn ? "ok" : "wifi_disconnected";
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_connected"] = conn;
  doc["ip"] = ip;
  doc["rssi"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["requests_total"] = reqTotal;
  doc["requests_ok"] = reqOk;
  doc["requests_fail"] = reqFail;
  doc["local_token_set"] = g_localToken.length() > 0;
  doc["providers"] = g_providerCount;
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleModels() {
  if (!authCheck()) { sendError(401, "unauthorized"); return; }
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["object"] = "list";
  JsonArray data = root["data"].to<JsonArray>();
  for (int i = 0; i < g_providerCount; i++) {
    String l = g_providerModels[i];
    if (l.length()) {
      int start = 0;
      while (start < (int)l.length()) {
        int comma = l.indexOf(',', start);
        String m = (comma < 0) ? l.substring(start) : l.substring(start, comma);
        JsonObject mo = data.add<JsonObject>();
        mo["id"] = g_providers[i].id + "/" + m;   // namespaced
        mo["object"] = "model";
        mo["owned_by"] = g_providers[i].id;
        if (comma < 0) break;
        start = comma + 1;
      }
    } else {
      JsonObject mo = data.add<JsonObject>();
      mo["id"] = g_providers[i].id + "/auto";
      mo["object"] = "model";
      mo["owned_by"] = g_providers[i].id;
    }
  }
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void handleOptions() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.send(204, "", "");
}

void handleChat() {
  reqTotal++;
  if (!authCheck()) { reqFail++; sendError(401, "unauthorized"); return; }
  String body = server.arg("plain");
  if (body.length() == 0) { reqFail++; sendError(400, "empty body"); return; }
  if (body.length() > 8192) { reqFail++; sendError(413, "payload too large"); return; }

  String model = "";
  int mi = body.indexOf("\"model\"");
  if (mi >= 0) {
    int q1 = body.indexOf("\"", mi + 7);
    int q2 = body.indexOf("\"", q1 + 1);
    if (q1 > 0 && q2 > q1) model = body.substring(q1 + 1, q2);
  }

  String upstreamModel;
  int idx = routeProvider(model, &upstreamModel);
  if (idx < 0) { reqFail++; sendError(500, "no provider configured"); return; }

  Provider& p = g_providers[idx];
  if (p.key.length() == 0) {
    reqFail++;
    sendError(500, "provider \"" + p.id + "\" has no API key");
    return;
  }

  // Rewrite the model field to the upstream (un-namespaced) name.
  if (upstreamModel != model) {
    int ms = body.indexOf("\"model\"");
    if (ms >= 0) {
      int q1 = body.indexOf("\"", ms + 7);
      int q2 = body.indexOf("\"", q1 + 1);
      if (q1 > 0 && q2 > q1) {
        body = body.substring(0, q1 + 1) + upstreamModel + body.substring(q2);
      }
    }
  }

  String url = apiRoot(p.url) + "/chat/completions";
  bool isStream = body.indexOf("\"stream\":true") >= 0 || body.indexOf("\"stream\": true") >= 0;

  WiFiClientSecure* client = new WiFiClientSecure;
  client->setInsecure();
  HTTPClient https;
  https.begin(*client, url);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + p.key);
  https.addHeader("Accept", isStream ? "text/event-stream" : "application/json");
  if (p.id == "openrouter") {
    https.addHeader("HTTP-Referer", "http://" + WiFi.localIP().toString());
    https.addHeader("X-Title", "ESP32 Router");
  }
  https.setTimeout(30000);
  int code = https.POST(body);
  String resp = https.getString();
  https.end();
  delete client;

  if (code >= 200 && code < 300) {
    reqOk++;
    if (isStream) {
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.sendHeader("Cache-Control", "no-cache");
      server.send(200, "text/event-stream", resp);
    } else {
      sendJson(200, resp.length() ? resp : "{}");
    }
  } else {
    reqFail++;
    if (resp.length() && resp[0] == '{') sendJson(code > 0 ? code : 502, resp);
    else sendError(code > 0 ? code : 502, "upstream error " + String(code));
  }
  Serial.printf("chat model=%s -> %s code=%d heap=%d\n", model.c_str(), p.id.c_str(), code, ESP.getFreeHeap());
}

void handleNotFound() { sendError(404, "not found"); }

// ---------------------------------------------------------------------------
// Admin JSON API
// ---------------------------------------------------------------------------
bool requireAdmin() {
  if (isAuthenticated()) return true;
  sendError(401, "unauthorized");
  return false;
}

// GET /api/state — full dashboard state (admin only)
void handleApiState() {
  if (!requireAdmin()) return;
  bool conn = WiFi.status() == WL_CONNECTED;
  bool apMode = (WiFi.getMode() == WIFI_AP);

  JsonDocument doc;
  doc["version"] = VERSION;
  doc["wifi"]["ssid"] = g_wifiSsid;
  doc["wifi"]["connected"] = conn;
  doc["wifi"]["ap_mode"] = apMode;
  doc["wifi"]["ip"] = WiFi.localIP().toString();
  doc["wifi"]["rssi"] = WiFi.RSSI();
  doc["token"]["set"] = g_localToken.length() > 0;
  doc["token"]["full"] = g_localToken;
  doc["token"]["masked"] = maskKey(g_localToken);
  doc["stats"]["uptime_s"] = millis() / 1000;
  doc["stats"]["heap"] = ESP.getFreeHeap();
  doc["stats"]["requests_total"] = reqTotal;
  doc["stats"]["requests_ok"] = reqOk;
  doc["stats"]["requests_fail"] = reqFail;

  JsonArray provs = doc["providers"].to<JsonArray>();
  for (int i = 0; i < g_providerCount; i++) {
    JsonObject o = provs.add<JsonObject>();
    o["id"] = g_providers[i].id;
    o["name"] = g_providers[i].name;
    o["url"] = g_providers[i].url;
    o["key_masked"] = maskKey(g_providers[i].key);
    o["has_key"] = g_providers[i].key.length() > 0;
    JsonArray models = o["models"].to<JsonArray>();
    String l = g_providerModels[i];
    if (l.length()) {
      int start = 0;
      while (start < (int)l.length()) {
        int comma = l.indexOf(',', start);
        String m = (comma < 0) ? l.substring(start) : l.substring(start, comma);
        models.add(g_providers[i].id + "/" + m);
        if (comma < 0) break;
        start = comma + 1;
      }
    }
  }

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

// POST /api/providers — add or update a provider, then auto-fetch its models.
// Body: { "name": "...", "url": "...", "key": "..." }  (id is slugified from name)
void handleApiProviderAdd() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) { sendError(400, "invalid JSON"); return; }

  String name = doc["name"] | "";
  String url = doc["url"] | "";
  String key = doc["key"] | "";
  name.trim();
  url.trim();
  key.trim();

  if (!name.length() || !url.length()) {
    sendError(400, "name and url are required");
    return;
  }

  String id = slugify(name);
  int idx = findProvider(id);
  if (idx < 0) {
    if (g_providerCount >= MAX_PROVIDERS) {
      sendError(409, "provider limit reached (" + String(MAX_PROVIDERS) + ")");
      return;
    }
    idx = g_providerCount++;
    g_providers[idx].id = id;
  }
  g_providers[idx].name = name;
  g_providers[idx].url = url;
  if (key.length()) g_providers[idx].key = key;   // empty key keeps existing
  saveProviders();

  // Auto-fetch models for the saved provider.
  int fetched = -1;
  if (g_providers[idx].key.length()) {
    fetched = fetchModels(idx);
  }

  JsonDocument out;
  out["ok"] = true;
  out["id"] = id;
  out["name"] = name;
  out["fetched_models"] = fetched;
  out["model_count"] = countModels(idx);
  if (fetched < 0) out["fetch_error"] = g_lastFetchError;
  String s;
  serializeJson(out, s);
  sendJson(200, s);
}

// POST /api/providers/remove — body { "id": "..." }
void handleApiProviderRemove() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String id = doc["id"] | "";
  id.trim();
  int idx = findProvider(id);
  if (idx < 0) { sendError(404, "provider not found"); return; }

  for (int i = idx; i < g_providerCount - 1; i++) {
    g_providers[i] = g_providers[i + 1];
    g_providerModels[i] = g_providerModels[i + 1];
  }
  g_providerCount--;
  saveProviders();
  prefs.begin("gateway", false);
  prefs.remove(("models_" + id).c_str());
  prefs.end();

  sendJson(200, "{\"ok\":true}");
}

// POST /api/providers/fetch — body { "id": "..." }
void handleApiProviderFetch() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String id = doc["id"] | "";
  id.trim();
  int idx = findProvider(id);
  if (idx < 0) { sendError(404, "provider not found"); return; }

  int n = fetchModels(idx);
  JsonDocument out;
  out["ok"] = n >= 0;
  out["count"] = n;
  if (n < 0) out["error"] = g_lastFetchError;
  String s;
  serializeJson(out, s);
  sendJson(200, s);
}

void handleApiTokenGenerate() {
  if (!requireAdmin()) return;
  String t = genToken(32);
  saveKey("local_token", t);
  g_localToken = t;
  JsonDocument out;
  out["ok"] = true;
  out["token"] = t;
  String s;
  serializeJson(out, s);
  sendJson(200, s);
}

void handleApiTokenClear() {
  if (!requireAdmin()) return;
  saveKey("local_token", "");
  g_localToken = "";
  sendJson(200, "{\"ok\":true}");
}

void handleApiPassword() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String np = doc["password"] | "";
  np.trim();
  if (np.length() < 3) { sendError(400, "password too short (min 3 chars)"); return; }
  saveKey("admin_pass", np);
  g_adminPass = np;
  sendJson(200, "{\"ok\":true}");
}

void handleApiWifi() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String ssid = doc["ssid"] | "";
  String pass = doc["pass"] | "";
  ssid.trim();
  if (!ssid.length()) { sendError(400, "ssid required"); return; }
  prefs.begin("gateway", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.end();
  g_wifiSsid = ssid;
  g_wifiPass = pass;
  sendJson(200, "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// Auth pages (login / logout)
// ---------------------------------------------------------------------------
void handleLogin() {
  String html = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Login — ESP32 Router</title><style>
:root{--bg:#0f1117;--surface:#171a21;--border:#262b36;--text:#f2f4f8;--muted:#8a93a5;--accent:#6366f1;--accent-h:#4f46e5}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--text);display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px}
.card{width:100%;max-width:360px;background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:32px}
.logo{display:flex;align-items:center;gap:12px;margin-bottom:8px}
.logo .mark{width:40px;height:40px;border-radius:10px;background:linear-gradient(135deg,#6366f1,#8b5cf6);display:flex;align-items:center;justify-content:center;font-weight:800;color:#fff;font-size:18px}
h1{font-size:18px;font-weight:700;margin:0} .sub{font-size:13px;color:var(--muted);margin:4px 0 24px}
label{font-size:12px;font-weight:600;display:block;margin:14px 0 6px}
input{width:100%;padding:11px 12px;border:1px solid var(--border);border-radius:10px;font-size:14px;background:#0d0f15;color:var(--text);outline:none}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(99,102,241,.2)}
button{width:100%;padding:11px;border-radius:10px;border:0;background:var(--accent);color:#fff;font-weight:600;font-size:14px;cursor:pointer;margin-top:18px}
button:hover{background:var(--accent-h)}
.hint{font-size:11px;color:var(--muted);margin-top:14px;text-align:center}
code{background:#0d0f15;border:1px solid var(--border);padding:1px 6px;border-radius:6px;font-size:11px}
</style></head><body><div class="card"><div class="logo"><div class="mark">R</div><div><h1>ESP32 Router</h1><div class="sub">AI API Gateway</div></div></div>
<p class="sub">Masuk ke dashboard. Password default <code>123456</code>.</p>
<form method="POST" action="/admin/login"><label>Password</label><input name="password" type="password" placeholder="••••••" required autofocus><button>Masuk</button></form>
<p class="hint">Ganti password di Settings setelah login.</p></div></body></html>)HTML";
  server.send(200, "text/html", html);
}

void handleLoginPost() {
  String p = server.arg("password");
  if (p == g_adminPass) {
    server.sendHeader("Set-Cookie", "esp_auth=ok; Path=/; Max-Age=86400");
    server.sendHeader("Location", "/");
    server.send(303, "", "");
  } else {
    server.send(200, "text/html", "<p>Password salah. <a href=/login>Kembali</a></p>");
  }
}

void handleLogout() {
  server.sendHeader("Set-Cookie", "esp_auth=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(303, "", "");
}

// ---------------------------------------------------------------------------
// Dashboard SPA
// ---------------------------------------------------------------------------
void handleRoot() {
  if (!isAuthenticated()) {
    server.sendHeader("Location", "/login");
    server.send(303, "", "");
    return;
  }
  String html = R"HTML(<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 Router</title><style>
:root{--bg:#f6f7f9;--surface:#ffffff;--surface-2:#f1f3f6;--border:#e3e6eb;--border-2:#d6dae1;--text:#101828;--muted:#667085;--subtle:#98a2b3;--accent:#6366f1;--accent-h:#4f46e5;--danger:#dc2626;--ok:#16a34a;--accent-soft:#eef0ff;--radius:12px;--shadow:0 1px 2px rgba(16,24,40,.05)}
.dark{--bg:#0f1117;--surface:#171a21;--surface-2:#1e222b;--border:#262b36;--border-2:#323945;--text:#f2f4f8;--muted:#8a93a5;--subtle:#5d6675;--accent:#818cf8;--accent-h:#6366f1;--accent-soft:#1e1b3a}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--text);font-size:14px}
.app{display:flex;min-height:100vh}
.rail{width:220px;background:var(--surface);border-right:1px solid var(--border);display:flex;flex-direction:column;padding:16px 12px;gap:4px;position:sticky;top:0;height:100vh}
.brand{display:flex;align-items:center;gap:10px;padding:4px 8px 18px;font-weight:700;font-size:15px}
.brand .mark{width:32px;height:32px;border-radius:8px;background:linear-gradient(135deg,#6366f1,#8b5cf6);display:flex;align-items:center;justify-content:center;color:#fff;font-weight:800}
.nav{display:flex;flex-direction:column;gap:2px}
.nav a{display:flex;align-items:center;gap:10px;padding:9px 12px;border-radius:9px;color:var(--muted);text-decoration:none;font-weight:500;cursor:pointer}
.nav a:hover{background:var(--surface-2);color:var(--text)}
.nav a.on{background:var(--accent-soft);color:var(--accent);font-weight:600}
.nav a .ico{width:18px;text-align:center}
.rail .foot{margin-top:auto;padding:8px;font-size:12px;color:var(--subtle)}
.rail .foot a{color:var(--subtle);text-decoration:none}
.main{flex:1;min-width:0}
.header{position:sticky;top:0;background:var(--surface);border-bottom:1px solid var(--border);padding:14px 24px;display:flex;justify-content:space-between;align-items:center;z-index:5}
.header h1{font-size:16px;font-weight:700;margin:0}
.header .right{display:flex;align-items:center;gap:10px}
.pill{font-size:11px;font-weight:600;padding:4px 10px;border-radius:999px;border:1px solid var(--border-2)}
.pill.ok{color:var(--ok);border-color:rgba(22,163,74,.3);background:rgba(22,163,74,.08)}
.pill.warn{color:#b45309;border-color:rgba(180,83,9,.3);background:rgba(180,83,9,.08)}
.iconbtn{width:34px;height:34px;border-radius:9px;border:1px solid var(--border-2);background:var(--surface);color:var(--text);font-size:15px;cursor:pointer;display:flex;align-items:center;justify-content:center}
.iconbtn:hover{background:var(--surface-2)}
.wrap{max-width:920px;margin:0 auto;padding:24px}
.view{display:none}.view.on{display:block}
.card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:20px;margin-bottom:16px;box-shadow:var(--shadow)}
.card h2{font-size:14px;font-weight:700;margin:0 0 14px;display:flex;align-items:center;gap:8px}
.card h2 .count{font-size:11px;font-weight:600;color:var(--muted);background:var(--surface-2);padding:2px 8px;border-radius:999px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:12px}
.stat{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px;box-shadow:var(--shadow)}
.stat .lbl{font-size:12px;color:var(--muted)}
.stat .val{font-size:22px;font-weight:700;margin-top:4px}
.stat .val.mono{font-family:ui-monospace,monospace;font-size:14px;word-break:break-all}
label{font-size:12px;font-weight:600;display:block;margin:12px 0 6px;color:var(--muted)}
input,select{width:100%;padding:10px 12px;border:1px solid var(--border-2);border-radius:9px;font-size:14px;background:var(--surface);color:var(--text);outline:none}
input:focus,select:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(99,102,241,.15)}
.btn{padding:9px 14px;border-radius:9px;border:1px solid var(--accent);background:var(--accent);color:#fff;font-size:13px;font-weight:600;cursor:pointer}
.btn:hover{background:var(--accent-h)}
.btn.ghost{background:var(--surface);color:var(--text);border-color:var(--border-2)}
.btn.ghost:hover{background:var(--surface-2)}
.btn.danger{background:transparent;color:var(--danger);border-color:var(--danger)}
.btn.sm{padding:6px 10px;font-size:12px}
.btn:disabled{opacity:.5;cursor:default}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.spacer{flex:1}
.provider{border:1px solid var(--border);border-radius:var(--radius);padding:16px;margin-bottom:12px}
.provider .head{display:flex;align-items:center;gap:10px;margin-bottom:8px}
.provider .name{font-weight:700}
.provider .id{font-family:ui-monospace,monospace;font-size:12px;color:var(--accent)}
.provider .url{font-family:ui-monospace,monospace;font-size:12px;color:var(--muted);word-break:break-all;margin-bottom:8px}
.provider .meta{font-size:12px;color:var(--subtle)}
.models{margin-top:10px;display:flex;flex-wrap:wrap;gap:6px}
.chip{font-family:ui-monospace,monospace;font-size:11px;padding:3px 8px;border-radius:6px;background:var(--surface-2);border:1px solid var(--border);color:var(--muted)}
.chip.key{color:var(--ok)}
.empty{color:var(--subtle);font-size:13px;padding:20px;text-align:center}
.mono{font-family:ui-monospace,monospace}
.kv{display:grid;grid-template-columns:1fr 1fr;gap:10px;font-size:13px}
.kv .lbl{color:var(--muted);font-size:12px}
.kv .val{word-break:break-all}
#toast{position:fixed;bottom:20px;right:20px;display:flex;flex-direction:column;gap:8px;z-index:100}
.toast{background:var(--surface);border:1px solid var(--border-2);border-radius:10px;padding:12px 16px;font-size:13px;box-shadow:0 8px 24px rgba(0,0,0,.15);max-width:340px}
.toast.error{border-color:var(--danger)}
.toast.ok{border-color:var(--ok)}
@media(max-width:760px){.rail{display:none}.wrap{padding:16px}}
</style></head><body>
<div class="app">
  <aside class="rail">
    <div class="brand"><div class="mark">R</div>ESP32 Router</div>
    <nav class="nav">
      <a data-view="overview" onclick="show('overview')"><span class="ico">◉</span>Overview</a>
      <a data-view="providers" onclick="show('providers')"><span class="ico">🔌</span>Providers</a>
      <a data-view="models" onclick="show('models')"><span class="ico">🧠</span>Models</a>
      <a data-view="settings" onclick="show('settings')"><span class="ico">⚙️</span>Settings</a>
    </nav>
    <div class="foot"><span id="foot-ver"></span><br><a href="/admin/logout">Logout</a></div>
  </aside>
  <main class="main">
    <div class="header">
      <h1 id="header-title">Overview</h1>
      <div class="right">
        <span class="pill" id="wifi-pill">—</span>
        <button class="iconbtn" onclick="toggleTheme()" title="Dark mode">🌓</button>
        <a href="/admin/logout" class="iconbtn" title="Logout" style="text-decoration:none">⏻</a>
      </div>
    </div>
    <div class="wrap">

      <section class="view" id="view-overview">
        <div class="card"><h2>Status</h2>
          <div class="grid" id="overview-grid"></div>
        </div>
        <div class="card"><h2>Endpoint</h2>
          <div class="row"><code class="mono" id="endpoint-url" style="padding:10px 12px;background:var(--surface-2);border-radius:8px"></code>
          <button class="btn ghost" onclick="copyText(STATE.wifi.ip?('http://'+STATE.wifi.ip+'/v1'):'')">Copy</button></div>
        </div>
      </section>

      <section class="view" id="view-providers">
        <div class="card"><h2>Add Provider</h2>
          <div class="row">
            <div style="flex:1;min-width:160px"><label>Nama</label><input id="p-name" placeholder="mis. Baroq"></div>
            <div style="flex:2;min-width:220px"><label>Base URL</label><input id="p-url" placeholder="https://api.example.com"></div>
          </div>
          <label>API Key</label><input id="p-key" type="password" placeholder="sk-...">
          <div class="row" style="margin-top:14px">
            <button class="btn" id="p-save" onclick="saveProvider()">Simpan &amp; Fetch Models</button>
            <span class="spacer"></span>
            <span style="font-size:12px;color:var(--subtle)" id="p-id-preview"></span>
          </div>
        </div>
        <div class="card"><h2>Providers <span class="count" id="prov-count"></span></h2>
          <div id="provider-list"></div>
        </div>
      </section>

      <section class="view" id="view-models">
        <div class="card"><h2>Semua Model <span class="count" id="model-count"></span></h2>
          <div class="models" id="model-list"></div>
        </div>
      </section>

      <section class="view" id="view-settings">
        <div class="card"><h2>Local API Key</h2>
          <div class="kv" id="token-kv"></div>
          <div class="row" style="margin-top:14px">
            <button class="btn" onclick="generateToken()">Generate</button>
            <button class="btn ghost" onclick="clearToken()">Clear</button>
          </div>
        </div>
        <div class="card"><h2>Wi-Fi</h2>
          <label>SSID</label><input id="w-ssid" placeholder="WiFi SSID">
          <label>Password</label><input id="w-pass" type="password" placeholder="WiFi Password">
          <div class="row" style="margin-top:14px"><button class="btn" onclick="saveWifi()">Save &amp; Reconnect</button></div>
        </div>
        <div class="card"><h2>Admin Password</h2>
          <label>Password baru</label><input id="a-pass" type="password" placeholder="min 3 karakter">
          <div class="row" style="margin-top:14px"><button class="btn" onclick="savePassword()">Update</button></div>
        </div>
      </section>

    </div>
  </main>
</div>
<div id="toast"></div>
<script>
var STATE=null;
function $(s){return document.querySelector(s)}
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]})}
function toast(msg,type){var t=document.createElement('div');t.className='toast '+(type||'');t.textContent=msg;document.getElementById('toast').appendChild(t);setTimeout(function(){t.remove()},4000)}
async function api(path,opts){opts=opts||{};var r=await fetch(path,opts);if(r.status===401){location.href='/login';throw new Error('unauthorized')}var d=await r.json().catch(function(){return{}});if(!r.ok)throw new Error(d.error||('HTTP '+r.status));return d}
function post(path,body){return api(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})})}

function show(v){
  var views=document.querySelectorAll('.view');for(var i=0;i<views.length;i++)views[i].classList.remove('on');
  var el=document.getElementById('view-'+v);if(el)el.classList.add('on');
  var navs=document.querySelectorAll('.nav a');for(var j=0;j<navs.length;j++)navs[j].classList.toggle('on',navs[j].getAttribute('data-view')===v);
  var titles={overview:'Overview',providers:'Providers',models:'Models',settings:'Settings'};
  document.getElementById('header-title').textContent=titles[v]||'Overview';
  try{history.replaceState(null,'','#/'+v)}catch(e){}
}

function toggleTheme(){var d=document.documentElement.classList.toggle('dark');try{localStorage.setItem('theme',d?'dark':'light')}catch(e){}}
function copyText(t){if(!t)return;try{navigator.clipboard.writeText(t);toast('Copied','ok')}catch(e){prompt('Copy:',t)}}

function renderOverview(){
  var w=STATE.wifi,s=STATE.stats;
  var g=document.getElementById('overview-grid');
  g.innerHTML='';
  function stat(l,v,mono){var d=document.createElement('div');d.className='stat';d.innerHTML='<div class="lbl">'+esc(l)+'</div><div class="val'+(mono?' mono':'')+'">'+esc(v)+'</div>';g.appendChild(d)}
  stat('IP Address',w.ip||'—',true);
  stat('Wi-Fi',w.ap_mode?'AP Mode':(w.connected?w.ssid||'connected':'disconnected'));
  stat('Uptime',s.uptime_s+'s');
  stat('Free Heap',s.heap+' B');
  stat('Requests',s.requests_total+' ('+s.requests_ok+' ok / '+s.requests_fail+' fail)');
  stat('Providers',STATE.providers.length);
  var pill=document.getElementById('wifi-pill');
  if(w.ap_mode){pill.textContent='AP Mode';pill.className='pill warn'}
  else if(w.connected){pill.textContent='Online';pill.className='pill ok'}
  else{pill.textContent='Offline';pill.className='pill warn'}
  document.getElementById('endpoint-url').textContent='http://'+(w.ip||'<ip>')+'/v1';
}

function renderProviders(){
  document.getElementById('prov-count').textContent=STATE.providers.length;
  var list=document.getElementById('provider-list');
  list.innerHTML='';
  if(!STATE.providers.length){list.innerHTML='<div class="empty">Belum ada provider. Tambahkan di atas.</div>';return}
  STATE.providers.forEach(function(p){
    var d=document.createElement('div');d.className='provider';
    var modelsHtml='';
    if(p.models&&p.models.length){modelsHtml='<div class="models">'+p.models.map(function(m){return '<span class="chip">'+esc(m)+'</span>'}).join('')+'</div>'}
    else{modelsHtml='<div class="empty" style="padding:8px;text-align:left">Belum ada model — klik Fetch Models.</div>'}
    d.innerHTML='<div class="head"><span class="name">'+esc(p.name)+'</span><span class="id">'+esc(p.id)+'</span>'+
      '<span class="spacer"></span>'+
      '<button class="btn ghost sm" onclick="fetchModels(\''+p.id+'\')">Fetch Models</button>'+
      '<button class="btn ghost sm" onclick="editProvider(\''+p.id+'\')">Edit</button>'+
      '<button class="btn danger sm" onclick="removeProvider(\''+p.id+'\')">Hapus</button></div>'+
      '<div class="url">'+esc(p.url)+'</div>'+
      '<div class="meta">API Key: '+(p.has_key?('<span class="chip key">'+esc(p.key_masked)+'</span>'):'<span style="color:var(--danger)">tidak diisi</span>')+' · '+p.models.length+' model</div>'+
      modelsHtml;
    list.appendChild(d);
  });
}

function renderModels(){
  var all=[];
  STATE.providers.forEach(function(p){if(p.models)p.models.forEach(function(m){all.push(m)})});
  document.getElementById('model-count').textContent=all.length;
  var el=document.getElementById('model-list');
  el.innerHTML='';
  if(!all.length){el.innerHTML='<div class="empty">Belum ada model. Fetch models dari provider.</div>';return}
  all.forEach(function(m){var s=document.createElement('span');s.className='chip';s.textContent=m;el.appendChild(s)});
}

function renderSettings(){
  var tk=STATE.token;
  var kv=document.getElementById('token-kv');
  kv.innerHTML='<div><div class="lbl">Status</div><div class="val">'+(tk.set?'Aktif (wajib Authorization)':'Nonaktif (open)')+'</div></div>'+
    '<div><div class="lbl">Token</div><div class="val mono">'+(tk.set?esc(tk.full):'—')+'</div></div>';
  document.getElementById('w-ssid').value=STATE.wifi.ssid||'';
  document.getElementById('w-pass').value='';
}

function render(){
  renderOverview();renderProviders();renderModels();renderSettings();
  document.getElementById('foot-ver').textContent='v'+STATE.version;
}

async function load(){
  try{STATE=await api('/api/state');render()}catch(e){toast('Gagal memuat: '+e.message,'error')}
}

function slug(name){return String(name||'').toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-+|-+$/g,'')||'provider'}
document.getElementById('p-name').addEventListener('input',function(){document.getElementById('p-id-preview').textContent='id: '+slug(this.value)});

async function saveProvider(){
  var name=document.getElementById('p-name').value.trim();
  var url=document.getElementById('p-url').value.trim();
  var key=document.getElementById('p-key').value.trim();
  if(!name||!url){toast('Nama dan Base URL wajib diisi','error');return}
  var btn=document.getElementById('p-save');btn.disabled=true;btn.textContent='Menyimpan & fetching...';
  try{
    var r=await post('/api/providers',{name:name,url:url,key:key});
    if(r.fetched_models>=0)toast('Provider "'+r.name+'" disimpan — '+r.fetched_models+' model di-fetch','ok');
    else if(r.fetch_error)toast('Provider disimpan, tapi fetch gagal: '+r.fetch_error,'error');
    else toast('Provider "'+r.name+'" disimpan (key kosong, fetch dilewati)','ok');
    document.getElementById('p-name').value='';document.getElementById('p-url').value='';document.getElementById('p-key').value='';
    document.getElementById('p-id-preview').textContent='';
    await load();
  }catch(e){toast(e.message,'error')}
  btn.disabled=false;btn.textContent='Simpan & Fetch Models';
}

async function fetchModels(id){
  toast('Fetching models untuk '+id+'...');
  try{var r=await post('/api/providers/fetch',{id:id});if(r.ok)toast(id+': '+r.count+' model di-fetch','ok');else toast(id+': '+r.error,'error');await load()}catch(e){toast(e.message,'error')}
}

async function removeProvider(id){
  if(!confirm('Hapus provider "'+id+'"?'))return;
  try{await post('/api/providers/remove',{id:id});toast('Provider dihapus','ok');await load()}catch(e){toast(e.message,'error')}
}

function editProvider(id){
  var p=STATE.providers.find(function(x){return x.id===id});if(!p)return;
  document.getElementById('p-name').value=p.name;
  document.getElementById('p-url').value=p.url;
  document.getElementById('p-key').value='';
  document.getElementById('p-id-preview').textContent='id: '+p.id+' (update)';
  show('providers');document.getElementById('p-name').focus();
}

async function generateToken(){
  try{var r=await post('/api/token/generate');toast('Token dibuat','ok');await load()}catch(e){toast(e.message,'error')}
}
async function clearToken(){
  try{await post('/api/token/clear');toast('Token dihapus','ok');await load()}catch(e){toast(e.message,'error')}
}
async function saveWifi(){
  var s=document.getElementById('w-ssid').value.trim();
  var p=document.getElementById('w-pass').value;
  if(!s){toast('SSID wajib diisi','error');return}
  try{await post('/api/wifi',{ssid:s,pass:p});toast('Menyimpan & restart...','ok')}catch(e){toast(e.message,'error')}
}
async function savePassword(){
  var p=document.getElementById('a-pass').value;
  if(p.length<3){toast('Password min 3 karakter','error');return}
  try{await post('/api/password',{password:p});toast('Password diubah','ok');document.getElementById('a-pass').value=''}catch(e){toast(e.message,'error')}
}

(function init(){
  try{var t=localStorage.getItem('theme');if(t==='dark'||(!t&&matchMedia('(prefers-color-scheme:dark)').matches))document.documentElement.classList.add('dark')}catch(e){}
  var v=(location.hash||'').replace('#/','');if(['overview','providers','models','settings'].indexOf(v)<0)v='overview';
  show(v);load();
  setInterval(load,30000);
})();
</script></body></html>)HTML";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== ESP32 Router v%s ===\n", VERSION);
  bootMs = millis();
  loadConfig();
  Serial.printf("admin %s | local token %s | wifi %s | providers %d\n",
                g_adminPass.c_str(),
                g_localToken.length() ? "set" : "open",
                g_wifiSsid.length() ? g_wifiSsid.c_str() : "(none)",
                g_providerCount);

  if (g_wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
    Serial.print("Connecting WiFi");
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t < 30) { delay(500); Serial.print("."); t++; }
    if (WiFi.status() == WL_CONNECTED)
      Serial.printf("\nWiFi OK IP %s RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else
      Serial.printf("\nWiFi FAIL %d\n", WiFi.status());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32Router-Setup", "12345678");
    Serial.printf("No WiFi configured — AP 'ESP32Router-Setup' (pw 12345678) IP %s\n", WiFi.softAPIP().toString().c_str());
  }

  // Public / OpenAI-compatible
  server.on("/", HTTP_GET, handleRoot);
  server.on("/dashboard", HTTP_GET, handleRoot);
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/admin/login", HTTP_POST, handleLoginPost);
  server.on("/admin/logout", HTTP_GET, handleLogout);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/admin/status", HTTP_GET, handleHealth);
  server.on("/v1/models", HTTP_GET, handleModels);
  server.on("/v1/chat/completions", HTTP_POST, handleChat);

  // Admin JSON API
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/providers", HTTP_POST, handleApiProviderAdd);
  server.on("/api/providers/remove", HTTP_POST, handleApiProviderRemove);
  server.on("/api/providers/fetch", HTTP_POST, handleApiProviderFetch);
  server.on("/api/token/generate", HTTP_POST, handleApiTokenGenerate);
  server.on("/api/token/clear", HTTP_POST, handleApiTokenClear);
  server.on("/api/password", HTTP_POST, handleApiPassword);
  server.on("/api/wifi", HTTP_POST, handleApiWifi);

  // CORS preflight
  server.on("/v1/chat/completions", HTTP_OPTIONS, handleOptions);
  server.on("/health", HTTP_OPTIONS, handleOptions);
  server.on("/v1/models", HTTP_OPTIONS, handleOptions);

  server.onNotFound(handleNotFound);
  const char* hk[] = {"Authorization", "Cookie"};
  server.collectHeaders(hk, 2);
  server.begin();
  Serial.printf("HTTP :80 dashboard http://%s/ heap %d\n", WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
}

void loop() {
  server.handleClient();
  static unsigned long last = 0;
  if (millis() - last > 10000) {
    last = millis();
    Serial.printf("heartbeat uptime=%lus heap=%d wifi=%d ip=%s\n",
                  millis() / 1000, ESP.getFreeHeap(),
                  WiFi.status() == WL_CONNECTED, WiFi.localIP().toString().c_str());
  }
}

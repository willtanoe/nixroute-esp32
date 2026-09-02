// NixRoute — ESP32 API Gateway for DOIT ESP32 DEVKIT V1
// OpenAI-compatible gateway with smart failover, round-robin multi-provider
// routing, per-provider metrics, and token usage tracking.
//
//   POST /v1/chat/completions   (streaming + non-streaming proxy)
//   GET  /v1/models             (provider models)
//   GET  /health                (device + provider status + usage)
//   GET  /                      (dashboard SPA from dashboard_html.h)
//
// Memory-safe: 8 KB request cap (413), chunked SSE passthrough (no full-body
// buffering), config persisted in NVS via Preferences.

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>
#include "dashboard_html.h"

#define VERSION          "3.1.0"
#define MAX_PROVIDERS    16
#define MAX_MODELS_CACHE 200
#define MAX_BODY         8192
#define MAX_USAGE_LOG    20
#define MAX_USAGE_MODELS 32

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
struct Metrics {
  uint32_t total = 0;
  uint32_t success = 0;
  uint32_t failed = 0;
  uint32_t rateLimited = 0;   // HTTP 429
  uint32_t lastLatencyMs = 0;
};

struct Provider {
  String id, name, url, key;
  bool active = true;
  Metrics m;
};

struct UsageEntry {
  char model[48];
  uint32_t promptTokens;
  uint32_t completionTokens;
  uint32_t totalTokens;
  uint32_t latencyMs;
  bool ok;
};

struct ModelUsage {
  char model[48];
  uint32_t requests;
  uint32_t tokens;
};

Preferences prefs;
String g_wifiSsid, g_wifiPass, g_localToken, g_adminPass;
Provider g_providers[MAX_PROVIDERS];
int g_providerCount = 0;
String g_providerModels[MAX_PROVIDERS];   // comma-separated raw model ids
WebServer server(80);
uint32_t g_reqTotal = 0, g_reqOk = 0, g_reqFail = 0;
uint32_t g_latencySum = 0;
uint32_t g_rr = 0;                         // round-robin cursor
unsigned long g_bootMs = 0;
String g_lastFetchError;

// usage tracking
uint32_t g_totalPrompt = 0, g_totalCompletion = 0, g_totalTokens = 0;
UsageEntry g_usageLog[MAX_USAGE_LOG];
int g_usageWrite = 0;
int g_usageCount = 0;
ModelUsage g_modelUsage[MAX_USAGE_MODELS];
int g_modelUsageCount = 0;

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
    if (isalnum((unsigned char)c)) r += (char)tolower((unsigned char)c);
    else if (c == ' ' || c == '-' || c == '_' || c == '.') {
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
// Usage tracking
// ---------------------------------------------------------------------------
void recordUsage(const char* model, uint32_t pt, uint32_t ct, uint32_t tt,
                 uint32_t lat, bool ok) {
  UsageEntry& e = g_usageLog[g_usageWrite];
  strncpy(e.model, model, sizeof(e.model) - 1);
  e.model[sizeof(e.model) - 1] = 0;
  e.promptTokens = pt;
  e.completionTokens = ct;
  e.totalTokens = tt;
  e.latencyMs = lat;
  e.ok = ok;
  g_usageWrite = (g_usageWrite + 1) % MAX_USAGE_LOG;
  if (g_usageCount < MAX_USAGE_LOG) g_usageCount++;

  if (ok && tt > 0) {
    g_totalPrompt += pt;
    g_totalCompletion += ct;
    g_totalTokens += tt;
    for (int i = 0; i < g_modelUsageCount; i++) {
      if (strcmp(g_modelUsage[i].model, model) == 0) {
        g_modelUsage[i].requests++;
        g_modelUsage[i].tokens += tt;
        return;
      }
    }
    if (g_modelUsageCount < MAX_USAGE_MODELS) {
      ModelUsage& m = g_modelUsage[g_modelUsageCount++];
      strncpy(m.model, model, sizeof(m.model) - 1);
      m.model[sizeof(m.model) - 1] = 0;
      m.requests = 1;
      m.tokens = tt;
    }
  }
}

// Extract usage from a JSON (or SSE tail) string. Returns true if found.
bool parseUsage(const String& s, uint32_t& pt, uint32_t& ct, uint32_t& tt) {
  int u = s.indexOf("\"usage\"");
  if (u < 0) return false;
  int brace = s.indexOf('{', u);
  if (brace < 0) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s.substring(brace));
  if (err) return false;
  pt = doc["prompt_tokens"] | 0;
  ct = doc["completion_tokens"] | 0;
  tt = doc["total_tokens"] | 0;
  if (!tt && (pt || ct)) tt = pt + ct;
  return tt > 0 || pt > 0 || ct > 0;
}

// ---------------------------------------------------------------------------
// Persistence (NVS)
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
    o["active"] = g_providers[i].active;
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
        Provider& p = g_providers[g_providerCount++];
        JsonObject o = v.as<JsonObject>();
        p.id = o["id"] | "";
        p.name = o["name"] | p.id.c_str();
        p.url = o["url"] | "";
        p.key = o["key"] | "";
        p.active = o["active"] | true;
      }
    }
  }
  // auto-heal: regenerate any id that drifted from slugify(name)
  bool healed = false;
  for (int i = 0; i < g_providerCount; i++) {
    String sid = slugify(g_providers[i].name);
    if (sid != g_providers[i].id) {
      g_providers[i].id = sid;
      healed = true;
    }
  }
  if (healed) saveProviders();
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

// Fetch a provider's model list (GET /models) and cache it.
// Returns count (>=0) or -1 on error (message in g_lastFetchError).
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
  if (g_providers[idx].key.length())
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

bool requireAdmin() {
  if (isAuthenticated()) return true;
  sendError(401, "unauthorized");
  return false;
}

// ---------------------------------------------------------------------------
// Routing engine
// ---------------------------------------------------------------------------
// Resolve a model to a list of candidate provider indices (round-robin ready).
// Returns candidate count; fills candidates[] and (optionally) upstreamModel.
int resolveCandidates(const String& model, int* candidates, String* upstreamModel) {
  if (upstreamModel) *upstreamModel = model;

  // 1. explicit "<provider>/<model>" namespace
  int slash = model.indexOf('/');
  if (slash > 0) {
    String prefix = model.substring(0, slash);
    int idx = findProvider(prefix);
    if (idx >= 0 && g_providers[idx].active) {
      candidates[0] = idx;
      if (upstreamModel) *upstreamModel = model.substring(slash + 1);
      return 1;
    }
  }

  // 2. exact match across providers (round-robin among matches)
  int n = 0;
  for (int i = 0; i < g_providerCount; i++)
    if (g_providers[i].active && modelInProvider(i, model)) candidates[n++] = i;
  if (n) { if (upstreamModel) *upstreamModel = model; return n; }

  // 3. "<provider>-" prefix
  for (int i = 0; i < g_providerCount; i++) {
    String p = g_providers[i].id + "-";
    if (model.startsWith(p)) {
      if (g_providers[i].active) {
        candidates[0] = i;
        if (upstreamModel) *upstreamModel = model.substring(p.length());
        return 1;
      }
    }
  }

  // 4. fallback: all active providers that have a key
  n = 0;
  for (int i = 0; i < g_providerCount; i++)
    if (g_providers[i].active && g_providers[i].key.length()) candidates[n++] = i;
  if (upstreamModel) *upstreamModel = model;
  return n;
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
  doc["heap_total"] = ESP.getHeapSize();
  doc["requests_total"] = g_reqTotal;
  doc["requests_ok"] = g_reqOk;
  doc["requests_fail"] = g_reqFail;
  doc["avg_latency_ms"] = g_reqOk ? (g_latencySum / g_reqOk) : 0;
  doc["local_token_set"] = g_localToken.length() > 0;
  doc["providers"] = g_providerCount;
  doc["tokens"]["prompt"] = g_totalPrompt;
  doc["tokens"]["completion"] = g_totalCompletion;
  doc["tokens"]["total"] = g_totalTokens;

  JsonArray provs = doc["provider_metrics"].to<JsonArray>();
  for (int i = 0; i < g_providerCount; i++) {
    JsonObject o = provs.add<JsonObject>();
    o["id"] = g_providers[i].id;
    o["active"] = g_providers[i].active;
    o["total"] = g_providers[i].m.total;
    o["success"] = g_providers[i].m.success;
    o["failed"] = g_providers[i].m.failed;
    o["rate_limited"] = g_providers[i].m.rateLimited;
    o["last_latency_ms"] = g_providers[i].m.lastLatencyMs;
  }
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
    if (!g_providers[i].active) continue;
    String l = g_providerModels[i];
    if (l.length()) {
      int start = 0;
      while (start < (int)l.length()) {
        int comma = l.indexOf(',', start);
        String m = (comma < 0) ? l.substring(start) : l.substring(start, comma);
        JsonObject mo = data.add<JsonObject>();
        mo["id"] = g_providers[i].id + "/" + m;
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

// Stream the upstream response body directly to the client (chunked SSE),
// while capturing the trailing data to extract token usage.
void streamResponse(HTTPClient& http, uint32_t* pt, uint32_t* ct, uint32_t* tt) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-cache");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/event-stream", "");
  NetworkClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  String tail;
  tail.reserve(2048);
  if (stream) {
    while (stream->connected() || stream->available()) {
      size_t avail = stream->available();
      if (avail) {
        if (avail > sizeof(buf)) avail = sizeof(buf);
        int r = stream->read(buf, avail);
        if (r > 0) {
          server.sendContent(reinterpret_cast<const char*>(buf), (size_t)r);
          tail.concat(reinterpret_cast<const char*>(buf), (unsigned)r);
          if (tail.length() > 2048) tail.remove(0, tail.length() - 2048);
        }
      } else {
        delay(1);
      }
    }
  }
  if (!parseUsage(tail, *pt, *ct, *tt)) { *pt = *ct = *tt = 0; }
}

// Attempt one upstream request. Returns HTTP status code.
int attemptRequest(int idx, const String& body, const String& upstreamModel, bool isStream) {
  Provider& p = g_providers[idx];
  String url = apiRoot(p.url) + "/chat/completions";

  // rewrite the "model" field to the upstream (un-namespaced) name
  String sendBody = body;
  int mi = sendBody.indexOf("\"model\"");
  if (mi >= 0) {
    int q1 = sendBody.indexOf('"', mi + 7);
    int q2 = q1 >= 0 ? sendBody.indexOf('"', q1 + 1) : -1;
    if (q1 > 0 && q2 > q1)
      sendBody = sendBody.substring(0, q1 + 1) + upstreamModel + sendBody.substring(q2);
  }
  // request usage in streaming responses
  if (isStream && sendBody.indexOf("stream_options") < 0) {
    int last = sendBody.lastIndexOf('}');
    if (last > 0)
      sendBody = sendBody.substring(0, last) + ",\"stream_options\":{\"include_usage\":true}" + sendBody.substring(last);
  }

  String fullModel = p.id + "/" + upstreamModel;
  unsigned long t0 = millis();
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    p.m.total++;
    p.m.failed++;
    recordUsage(fullModel.c_str(), 0, 0, 0, 0, false);
    return 0;  // connection failure → treat as 0 (fail over)
  }
  http.addHeader("Content-Type", "application/json");
  if (p.key.length()) http.addHeader("Authorization", "Bearer " + p.key);
  http.addHeader("Accept", isStream ? "text/event-stream" : "application/json");
  if (p.id == "openrouter") {
    http.addHeader("HTTP-Referer", "http://" + WiFi.localIP().toString());
    http.addHeader("X-Title", "NixRoute");
  }
  http.setTimeout(30000);
  int code = http.POST(sendBody);
  unsigned long lat = millis() - t0;

  p.m.lastLatencyMs = lat;
  p.m.total++;

  if (code >= 200 && code < 300) {
    p.m.success++;
    uint32_t pt = 0, ct = 0, tt = 0;
    if (isStream) {
      streamResponse(http, &pt, &ct, &tt);
    } else {
      String resp = http.getString();
      parseUsage(resp, pt, ct, tt);
      sendJson(200, resp.length() ? resp : "{}");
    }
    recordUsage(fullModel.c_str(), pt, ct, tt, lat, true);
  } else {
    p.m.failed++;
    if (code == 429) p.m.rateLimited++;
    http.getString();  // drain body, free connection
    recordUsage(fullModel.c_str(), 0, 0, 0, lat, false);
  }
  http.end();
  return code;
}

void handleChat() {
  g_reqTotal++;
  if (!authCheck()) { g_reqFail++; sendError(401, "unauthorized"); return; }

  // early payload cap via Content-Length
  if (server.hasHeader("Content-Length")) {
    long cl = server.header("Content-Length").toInt();
    if (cl > MAX_BODY) { g_reqFail++; sendError(413, "payload too large"); return; }
  }
  String body = server.arg("plain");
  if (body.length() == 0) { g_reqFail++; sendError(400, "empty body"); return; }
  if (body.length() > MAX_BODY) { g_reqFail++; sendError(413, "payload too large"); return; }

  // extract model
  String model = "";
  int mi = body.indexOf("\"model\"");
  if (mi >= 0) {
    int q1 = body.indexOf('"', mi + 7);
    int q2 = q1 >= 0 ? body.indexOf('"', q1 + 1) : -1;
    if (q1 > 0 && q2 > q1) model = body.substring(q1 + 1, q2);
  }
  if (!model.length()) { g_reqFail++; sendError(400, "missing model"); return; }

  bool isStream = body.indexOf("\"stream\":true") >= 0 || body.indexOf("\"stream\": true") >= 0;

  int candidates[MAX_PROVIDERS];
  String upstreamModel;
  int n = resolveCandidates(model, candidates, &upstreamModel);
  if (n == 0) { g_reqFail++; sendError(500, "no active provider available"); return; }

  // round-robin starting offset across the candidate set
  int start = g_rr++ % n;

  int lastCode = 0;
  bool ok = false;
  unsigned long t0 = millis();
  for (int k = 0; k < n; k++) {
    int idx = candidates[(start + k) % n];
    int code = attemptRequest(idx, body, upstreamModel, isStream);
    if (code >= 200 && code < 300) {
      ok = true;
      break;
    }
    lastCode = code;
    // fail over on rate-limit (429) or server/connectivity errors (5xx / 0)
    if (code == 429 || code >= 500 || code == 0) continue;
    // other client errors (4xx) are not retried
    break;
  }

  if (ok) {
    g_reqOk++;
    g_latencySum += millis() - t0;
  } else {
    g_reqFail++;
    if (lastCode == 429) sendError(429, "all providers rate-limited");
    else if (lastCode >= 500 || lastCode == 0) sendError(502, "all providers failed");
    else sendError(lastCode > 0 ? lastCode : 502, "upstream error");
  }
  Serial.printf("chat model=%s candidates=%d code=%d heap=%d\n",
                model.c_str(), n, lastCode, ESP.getFreeHeap());
}

void handleNotFound() { sendError(404, "not found"); }

// ---------------------------------------------------------------------------
// Admin JSON API
// ---------------------------------------------------------------------------
// GET /api/state — full dashboard state
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
  doc["stats"]["heap_total"] = ESP.getHeapSize();
  doc["stats"]["requests_total"] = g_reqTotal;
  doc["stats"]["requests_ok"] = g_reqOk;
  doc["stats"]["requests_fail"] = g_reqFail;
  doc["stats"]["avg_latency_ms"] = g_reqOk ? (g_latencySum / g_reqOk) : 0;

  JsonObject usg = doc["usage"].to<JsonObject>();
  usg["prompt_tokens"] = g_totalPrompt;
  usg["completion_tokens"] = g_totalCompletion;
  usg["total_tokens"] = g_totalTokens;

  JsonArray log = usg["recent"].to<JsonArray>();
  for (int i = 0; i < g_usageCount; i++) {
    int idx = (g_usageWrite - 1 - i + MAX_USAGE_LOG) % MAX_USAGE_LOG;
    UsageEntry& e = g_usageLog[idx];
    JsonObject o = log.add<JsonObject>();
    o["model"] = e.model;
    o["prompt_tokens"] = e.promptTokens;
    o["completion_tokens"] = e.completionTokens;
    o["total_tokens"] = e.totalTokens;
    o["latency_ms"] = e.latencyMs;
    o["ok"] = e.ok;
  }

  JsonArray mu = usg["models"].to<JsonArray>();
  for (int i = 0; i < g_modelUsageCount; i++) {
    JsonObject o = mu.add<JsonObject>();
    o["model"] = g_modelUsage[i].model;
    o["requests"] = g_modelUsage[i].requests;
    o["tokens"] = g_modelUsage[i].tokens;
  }

  JsonArray provs = doc["providers"].to<JsonArray>();
  for (int i = 0; i < g_providerCount; i++) {
    JsonObject o = provs.add<JsonObject>();
    o["id"] = g_providers[i].id;
    o["name"] = g_providers[i].name;
    o["url"] = g_providers[i].url;
    o["key_masked"] = maskKey(g_providers[i].key);
    o["has_key"] = g_providers[i].key.length() > 0;
    o["active"] = g_providers[i].active;
    JsonObject mm = o["metrics"].to<JsonObject>();
    mm["total"] = g_providers[i].m.total;
    mm["success"] = g_providers[i].m.success;
    mm["failed"] = g_providers[i].m.failed;
    mm["rate_limited"] = g_providers[i].m.rateLimited;
    mm["last_latency_ms"] = g_providers[i].m.lastLatencyMs;
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

// POST /api/providers — add/update provider {name,url,key,active}
void handleApiProviderAdd() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { sendError(400, "invalid JSON"); return; }

  String name = doc["name"] | "";
  String url = doc["url"] | "";
  String key = doc["key"] | "";
  bool active = doc["active"] | true;
  name.trim(); url.trim(); key.trim();

  if (!name.length() || !url.length()) { sendError(400, "name and url are required"); return; }

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
  g_providers[idx].active = active;
  if (key.length()) g_providers[idx].key = key;   // empty key keeps existing
  saveProviders();

  int fetched = -1;
  if (g_providers[idx].key.length()) fetched = fetchModels(idx);

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

// POST /api/providers/remove — {id}
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

// POST /api/providers/toggle — {id, active}
void handleApiProviderToggle() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String id = doc["id"] | "";
  bool active = doc["active"] | true;
  id.trim();
  int idx = findProvider(id);
  if (idx < 0) { sendError(404, "provider not found"); return; }
  g_providers[idx].active = active;
  saveProviders();
  sendJson(200, "{\"ok\":true}");
}

// POST /api/providers/fetch — {id}
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
  String html = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 500 500'%3E%3Crect x='70' y='70' width='92' height='375' rx='46' fill='%230c1a30'/%3E%3Crect x='338' y='70' width='92' height='375' rx='46' fill='%230c1a30'/%3E%3Cline x1='125' y1='130' x2='375' y2='380' stroke='%230c1a30' stroke-width='96' stroke-linecap='round'/%3E%3Cline x1='125' y1='130' x2='375' y2='380' stroke='%23ffffff' stroke-width='18' stroke-linecap='round'/%3E%3Ccircle cx='125' cy='130' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='125' cy='130' r='16' fill='%2300a8b5'/%3E%3Ccircle cx='250' cy='255' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='250' cy='255' r='16' fill='%2300a8b5'/%3E%3Ccircle cx='375' cy='380' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='375' cy='380' r='16' fill='%2300a8b5'/%3E%3C/svg%3E"><title>Login — NixRoute</title><style>
:root{--bg:#0f172a;--surface:#1e293b;--border:#334155;--text:#e2e8f0;--muted:#94a3b8;--accent:#00a8b5;--accent-h:#14c1cf}
*{box-sizing:border-box}body{margin:0;font-family:ui-sans-serif,system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px}
.card{width:100%;max-width:360px;background:var(--surface);border:1px solid var(--border);border-radius:16px;padding:32px}
.logo{display:flex;align-items:center;gap:12px;margin-bottom:6px}
.logo svg{width:40px;height:40px;flex-shrink:0}
h1{font-size:18px;font-weight:700;margin:0}.sub{font-size:13px;color:var(--muted);margin:4px 0 24px}
label{font-size:12px;font-weight:600;display:block;margin:14px 0 6px}
input{width:100%;padding:11px 12px;border:1px solid var(--border);border-radius:10px;font-size:14px;background:#0d1526;color:var(--text);outline:none}
input:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(0,168,181,.2)}
button{width:100%;padding:11px;border-radius:10px;border:0;background:var(--accent);color:#fff;font-weight:600;font-size:14px;cursor:pointer;margin-top:18px}
button:hover{background:var(--accent-h)}
.hint{font-size:11px;color:var(--muted);margin-top:14px;text-align:center}
code{background:#0d1526;border:1px solid var(--border);padding:1px 6px;border-radius:6px;font-size:11px}
</style></head><body><div class="card"><div class="logo"><svg viewBox="0 0 500 500" xmlns="http://www.w3.org/2000/svg"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><path d="M416 85l26-26m0 0h-24m24 0v24" fill="none" stroke="#0c1a30" stroke-width="15" stroke-linecap="round" stroke-linejoin="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#ffffff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg><div><h1>NixRoute</h1><div class="sub">ESP32 API Gateway</div></div></div>
<p class="sub">Sign in to the dashboard. Default password <code>123456</code>.</p>
<form method="POST" action="/admin/login"><label>Password</label><input name="password" type="password" placeholder="••••••" required autofocus><button>Sign In</button></form>
<p class="hint">Change the password in Settings after login.</p></div></body></html>)HTML";
  server.send(200, "text/html", html);
}

void handleLoginPost() {
  String p = server.arg("password");
  if (p == g_adminPass) {
    server.sendHeader("Set-Cookie", "esp_auth=ok; Path=/; Max-Age=86400");
    server.sendHeader("Location", "/");
    server.send(303, "", "");
  } else {
    server.send(200, "text/html", "<body style='background:#0f172a;color:#e2e8f0;font-family:sans-serif;text-align:center;padding-top:60px'><p>Wrong password. <a href=/login style='color:#818cf8'>Back</a></p></body>");
  }
}

void handleLogout() {
  server.sendHeader("Set-Cookie", "esp_auth=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(303, "", "");
}

// ---------------------------------------------------------------------------
// Dashboard SPA (served from PROGMEM)
// ---------------------------------------------------------------------------
void handleRoot() {
  if (!isAuthenticated()) {
    server.sendHeader("Location", "/login");
    server.send(303, "", "");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== NixRoute v%s ===\n", VERSION);
  g_bootMs = millis();
  loadConfig();
  Serial.printf("admin %s | token %s | wifi %s | providers %d\n",
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
    WiFi.softAP("NixRoute-Setup", "12345678");
    Serial.printf("No WiFi configured — AP 'NixRoute-Setup' (pw 12345678) IP %s\n",
                  WiFi.softAPIP().toString().c_str());
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
  server.on("/api/providers/toggle", HTTP_POST, handleApiProviderToggle);
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
  Serial.printf("HTTP :80 dashboard http://%s/ heap %d\n",
                WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
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

// NixRoute — ESP32 API Gateway for DOIT ESP32 DEVKIT V1
// OpenAI-compatible gateway with smart failover, round-robin multi-provider
// routing, per-provider metrics, and token usage tracking.
//
//   POST /v1/chat/completions   (streaming + non-streaming proxy)
//   GET  /v1/models             (provider models)
//   GET  /health                (device + provider status + usage)
//   GET  /                      (dashboard SPA from dashboard_html.h)
//
// Dual-core FreeRTOS task pinning:
//   Core 0 — WebServer task (dashboard, admin API, auth, mDNS-ready)
//   Core 1 — Proxy engine task (HTTPS/mbedTLS upstream + SSE streaming)
// A FreeRTOS queue hands each chat request (with its client socket) from Core 0
// to Core 1, so a long stream never blocks the dashboard.
//
// Memory-safe: 8 KB request cap (413), chunked SSE passthrough (no full-body
// buffering), config persisted in NVS via Preferences.

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "mbedtls/md.h"
#include "dashboard_html.h"

#define VERSION          "3.2.1"
#define MAX_PROVIDERS    16
#define MAX_TOKENS       5
#define MAX_MODELS_CACHE 50
#define MAX_USAGE_LOG    20
#define MAX_USAGE_MODELS 32
#define PROXY_QUEUE_LEN  4

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
  int consecutiveFailures = 0;        // circuit breaker
  unsigned long coolDownUntil = 0;    // millis() timestamp when cooldown ends
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

// A chat request handed from the WebServer task (Core 0) to the proxy task
// (Core 1). Provider data is snapshotted at enqueue time so the proxy task
// never touches the provider array (which admin mutates on Core 0).
struct ProviderSnap { String id, url, key; };
struct ProxyJob {
  String model;         // requested model (for logging)
  String upstreamModel; // un-namespaced model to send upstream
  bool isStream;
  int n;
  ProviderSnap providers[MAX_PROVIDERS];
  NetworkClient client;
  volatile bool bodyDone;         // set by the raw handler once the body is complete
  volatile bool aborted;          // set by the raw handler when the upload is aborted
  volatile bool bodySent;         // set once the full body has been relayed upstream
  volatile bool responseStarted;  // set once an HTTP response head was sent to the client
};


Preferences prefs;
String g_wifiSsid, g_wifiPass, g_adminPass;
String g_adminSession;                    // random per-boot admin session token
String g_tokens[MAX_TOKENS];
int g_tokenCount = 0;
Provider g_providers[MAX_PROVIDERS];
int g_providerCount = 0;
String g_providerModels[MAX_PROVIDERS];   // comma-separated raw model ids
WebServer server(80);
DNSServer dnsServer;
bool g_apMode = false;
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

// FreeRTOS primitives
QueueHandle_t g_proxyQueue = NULL;
SemaphoreHandle_t g_statsMutex = NULL;
StreamBufferHandle_t g_bodyStream = NULL;
TaskHandle_t g_webTask = NULL, g_proxyTask = NULL;

// Zero-copy request streaming state (owned by the WebServer task, Core 0).
// The WebServer parses one request body completely before it starts the next
// one, so these are safe to reset at RAW_START.
String g_head;            // head of the request body (to locate & rewrite "model")
bool g_headDone = false;  // true once the head has been routed & streamed
bool g_ownsJob = false;   // true only while the in-flight job belongs to THIS request
String g_chatError;
int g_chatErrorCode = 0;
ProxyJob* g_currentJob = NULL;  // job handed to the proxy task (Core 1 owns it)

// WebSocket live telemetry
#define MAX_WS_CLIENTS 4
WiFiServer wsServer(81);
WiFiClient wsClients[MAX_WS_CLIENTS];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
void statsLock() { if (g_statsMutex) xSemaphoreTake(g_statsMutex, portMAX_DELAY); }
void statsUnlock() { if (g_statsMutex) xSemaphoreGive(g_statsMutex); }

// Wrap-safe deadline test for a cooldown stored as a millis() timestamp. Plain
// "<=" breaks when millis() rolls over after ~49.7 days; the signed difference
// keeps the comparison correct as long as the cooldown is far shorter than
// half the 32-bit millis() range.
inline bool inCooldown(unsigned long until) { return (int32_t)(until - millis()) > 0; }

String maskKey(const String& k) {
  if (k.length() == 0) return "";
  if (k.length() <= 8) return "***";
  return k.substring(0, 4) + "***" + k.substring(k.length() - 4);
}

String genToken() {
  static const char* cs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String s;
  s.reserve(24);
  for (int i = 0; i < 24; i++) s += cs[esp_random() % 62];
  return "nx-" + s;
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

const char* reasonPhrase(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Error";
  }
}

// ---------------------------------------------------------------------------
// Usage tracking (mutex-guarded; written by proxy task, read by admin/health)
// ---------------------------------------------------------------------------
void recordUsage(const char* model, uint32_t pt, uint32_t ct, uint32_t tt,
                 uint32_t lat, bool ok) {
  statsLock();
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
        statsUnlock();
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
  statsUnlock();
}

void bumpMetrics(const String& id, bool ok, int code, uint32_t lat) {
  statsLock();
  for (int i = 0; i < g_providerCount; i++) {
    if (g_providers[i].id == id) {
      g_providers[i].m.total++;
      g_providers[i].m.lastLatencyMs = lat;
      if (ok) g_providers[i].m.success++;
      else { g_providers[i].m.failed++; if (code == 429) g_providers[i].m.rateLimited++; }
      break;
    }
  }
  statsUnlock();
}

// Circuit breaker: on 5xx/timeout, count consecutive failures; after
// CIRCUIT_BREAKER_THRESHOLD, enter cooldown for CIRCUIT_BREAKER_COOLDOWN_MS.
#define CIRCUIT_BREAKER_THRESHOLD   3
#define CIRCUIT_BREAKER_COOLDOWN_MS 60000

void recordProviderResult(const String& id, bool ok, int code) {
  statsLock();
  for (int i = 0; i < g_providerCount; i++) {
    if (g_providers[i].id == id) {
      if (ok) {
        g_providers[i].consecutiveFailures = 0;
        g_providers[i].coolDownUntil = 0;
      } else if (code >= 500 || code == 0) {  // 5xx or connection timeout
        if (++g_providers[i].consecutiveFailures >= CIRCUIT_BREAKER_THRESHOLD) {
          g_providers[i].coolDownUntil = millis() + CIRCUIT_BREAKER_COOLDOWN_MS;
          g_providers[i].consecutiveFailures = 0;
        }
      }
      break;
    }
  }
  statsUnlock();
}

// Returns the index of the '}' that closes the object whose opening '{' is at
// `open`, walking string literals (incl. escapes) so braces inside strings are
// not counted. Returns -1 when the JSON is truncated.
int matchJsonObject(const String& s, int open) {
  int depth = 0;
  bool inStr = false;
  for (int i = open; i < (int)s.length(); i++) {
    char c = s[i];
    if (inStr) {
      if (c == '\\') i++;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{') depth++;
    else if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return -1;
}

// Extracts and parses the last "usage" object seen in the buffered text
// (which may be a plain JSON body or an SSE tail). Handles both OpenAI token
// names (prompt_tokens/completion_tokens/total_tokens) and Anthropic-style
// names (input_tokens/output_tokens).
bool parseUsage(const String& s, uint32_t& pt, uint32_t& ct, uint32_t& tt) {
  pt = ct = tt = 0;
  int u = s.lastIndexOf("\"usage\"");
  if (u < 0) return false;
  int colon = s.indexOf(':', u);
  if (colon < 0) return false;
  int v = colon + 1;
  while (v < (int)s.length() && (s[v] == ' ' || s[v] == '\t' || s[v] == '\r' || s[v] == '\n')) v++;
  if (v >= (int)s.length() || s[v] != '{') return false;
  int close = matchJsonObject(s, v);
  if (close < 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, s.substring(v, close + 1))) return false;

  pt = doc["prompt_tokens"] | 0;
  ct = doc["completion_tokens"] | 0;
  tt = doc["total_tokens"] | 0;
  if (pt == 0 && ct == 0 && tt == 0) {
    // Anthropic passthrough: input_tokens/output_tokens
    pt = doc["input_tokens"] | 0;
    ct = doc["output_tokens"] | 0;
  }
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
  g_adminPass = prefs.getString("admin_pass", "123456");
  String tok = prefs.getString("local_tokens", "");
  prefs.end();
  g_tokenCount = 0;
  int start = 0;
  while (start < (int)tok.length() && g_tokenCount < MAX_TOKENS) {
    int comma = tok.indexOf(',', start);
    String t = (comma < 0) ? tok.substring(start) : tok.substring(start, comma);
    if (t.length()) g_tokens[g_tokenCount++] = t;
    if (comma < 0) break;
    start = comma + 1;
  }
  loadProviders();
}

void saveTokens() {
  String tok = "";
  for (int i = 0; i < g_tokenCount; i++) {
    if (i) tok += ",";
    tok += g_tokens[i];
  }
  saveKey("local_tokens", tok);
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

int fetchModels(int idx) {
  g_lastFetchError = "";
  String url = apiRoot(g_providers[idx].url) + "/models";
  bool secure = url.startsWith("https://");
  int code = -1;
  String body = "";
  if (secure) {
    // The TLS client must outlive the whole request, so keep it scope-local to
    // this block along with the HTTPClient that borrows it.
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (http.begin(client, url)) {
      if (g_providers[idx].key.length())
        http.addHeader("Authorization", "Bearer " + g_providers[idx].key);
      http.setTimeout(20000);
      code = http.GET();
      body = http.getString();
      http.end();
    } else {
      g_lastFetchError = "cannot connect to " + url;
      return -1;
    }
  } else {
    HTTPClient http;  // http:// uses HTTPClient's built-in plain TCP client
    if (http.begin(url)) {
      if (g_providers[idx].key.length())
        http.addHeader("Authorization", "Bearer " + g_providers[idx].key);
      http.setTimeout(20000);
      code = http.GET();
      body = http.getString();
      http.end();
    } else {
      g_lastFetchError = "cannot connect to " + url;
      return -1;
    }
  }

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
// Pulls the value of the esp_auth cookie, or "" when absent.
String sessionCookieValue() {
  if (!server.hasHeader("Cookie")) return "";
  String c = server.header("Cookie");
  int p = c.indexOf("esp_auth=");
  if (p < 0) return "";
  int start = p + 9;
  int end = c.indexOf(';', start);
  if (end < 0) end = c.length();
  return c.substring(start, end);
}

bool isAuthenticated() {
  if (g_adminSession.length() == 0) return false;
  return sessionCookieValue() == g_adminSession;  // exact match, no substring trick
}

bool authCheck() {
  if (g_tokenCount == 0) return true;
  if (!server.hasHeader("Authorization")) return false;
  String h = server.header("Authorization");
  for (int i = 0; i < g_tokenCount; i++) {
    String need = "Bearer " + g_tokens[i];
    // constant-time compare over the longer of the two lengths (no early
    // length rejection, which would leak the token length)
    volatile int d = 0;
    size_t n = h.length() > need.length() ? h.length() : need.length();
    for (size_t j = 0; j < n; j++) {
      char a = j < h.length() ? h[j] : 0;
      char b = j < need.length() ? need[j] : 0;
      d |= a ^ b;
    }
    if (d == 0) return true;
  }
  return false;
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
  if (!isAuthenticated()) { sendError(401, "unauthorized"); return false; }
  // CSRF guard: any state-changing admin call must carry X-NixRoute: 1, which
  // the dashboard sends but a cross-site <form>/<img> cannot. GET reads stay
  // cookie-only (cross-origin scripts can't read the responses anyway).
  if (server.method() != HTTP_GET && server.header("X-NixRoute") != "1") {
    sendError(403, "forbidden");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Routing engine (runs on Core 0, in the WebServer task)
// ---------------------------------------------------------------------------
int resolveCandidates(String model, int* candidates, String* upstreamModel) {
  if (model.startsWith("nx/")) model = model.substring(3);
  if (upstreamModel) *upstreamModel = model;

  // 1. explicit "<provider>/<model>" namespace
  int slash = model.indexOf('/');
  if (slash > 0) {
    String prefix = model.substring(0, slash);
    int idx = findProvider(prefix);
    if (idx >= 0) {
      if (g_providers[idx].active && !inCooldown(g_providers[idx].coolDownUntil)) {
        candidates[0] = idx;
        if (upstreamModel) *upstreamModel = model.substring(slash + 1);
        return 1;
      }
      // The user explicitly asked for a provider that is disabled or cooling.
      // Do NOT fall through and silently send the still-namespaced model string
      // (e.g. "openai/gpt-4o") to some unrelated provider -> report it instead.
      return -1;
    }
  }

  // 2. exact match across providers (round-robin among non-cooling matches)
  int n = 0;
  for (int i = 0; i < g_providerCount; i++)
    if (g_providers[i].active && !inCooldown(g_providers[i].coolDownUntil) && modelInProvider(i, model))
      candidates[n++] = i;
  if (n) { if (upstreamModel) *upstreamModel = model; return n; }

  // 3. "<provider>-" prefix
  for (int i = 0; i < g_providerCount; i++) {
    String p = g_providers[i].id + "-";
    if (model.startsWith(p)) {
      if (g_providers[i].active && !inCooldown(g_providers[i].coolDownUntil)) {
        candidates[0] = i;
        if (upstreamModel) *upstreamModel = model.substring(p.length());
        return 1;
      }
      return -1;  // same rationale as case 1: never misroute a namespaced model
    }
  }

  // 4. fallback: all active providers with a key (skip cooling)
  n = 0;
  for (int i = 0; i < g_providerCount; i++)
    if (g_providers[i].active && g_providers[i].key.length() && !inCooldown(g_providers[i].coolDownUntil))
      candidates[n++] = i;
  if (n) { if (upstreamModel) *upstreamModel = model; return n; }

  // 5. graceful degradation: if everything is cooling, allow cooling providers
  n = 0;
  for (int i = 0; i < g_providerCount; i++)
    if (g_providers[i].active && g_providers[i].key.length()) candidates[n++] = i;
  if (upstreamModel) *upstreamModel = model;
  return n;
}

// ---------------------------------------------------------------------------
// Raw HTTP response writers (used by the proxy task on Core 1)
// ---------------------------------------------------------------------------
bool writeJson(NetworkClient& c, int code, const String& body) {
  c.printf("HTTP/1.1 %d %s\r\n", code, reasonPhrase(code));
  c.print("Content-Type: application/json\r\n");
  c.print("Access-Control-Allow-Origin: *\r\n");
  c.print("Cache-Control: no-store\r\n");
  c.printf("Content-Length: %d\r\n", body.length());
  c.print("Connection: close\r\n");
  c.print("\r\n");
  size_t w = c.write(reinterpret_cast<const uint8_t*>(body.c_str()), body.length());
  c.flush();
  return w == body.length();
}

void writeErrorRaw(NetworkClient& c, int code, const String& msg) {
  JsonDocument doc;
  doc["error"]["message"] = msg;
  String out;
  serializeJson(doc, out);
  writeJson(c, code, out);
}

// ---------------------------------------------------------------------------
// Manual HTTPS client (chunked upload + response) — used by the proxy task
// ---------------------------------------------------------------------------
void parseUrl(const String& url, String& host, uint16_t& port, String& path) {
  port = 443;
  String s = url;
  if (s.startsWith("https://")) s = s.substring(8);
  else if (s.startsWith("http://")) { s = s.substring(7); port = 80; }
  int slash = s.indexOf('/');
  if (slash >= 0) { path = s.substring(slash); s = s.substring(0, slash); }
  else path = "/";
  int colon = s.indexOf(':');
  if (colon >= 0) { host = s.substring(0, colon); port = (uint16_t)s.substring(colon + 1).toInt(); }
  else host = s;
}

// The manual HTTP client helpers are templated on the client type so the relay
// works over TLS (WiFiClientSecure) as well as plain TCP (WiFiClient) - a
// provider URL that starts with http:// is now usable too.
template <class T>
int readResponseHeaders(T& client, bool* chunked, size_t* contentLength) {
  *chunked = false;
  *contentLength = 0;
  int code = 0;
  String status = client.readStringUntil('\n');
  status.trim();
  int sp1 = status.indexOf(' ');
  if (sp1 >= 0) {
    int sp2 = status.indexOf(' ', sp1 + 1);
    String cs = (sp2 >= 0) ? status.substring(sp1 + 1, sp2) : status.substring(sp1 + 1);
    code = cs.toInt();
  }
  while (true) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;
    int d = line.indexOf(':');
    if (d < 0) continue;
    String name = line.substring(0, d);
    String value = line.substring(d + 1);
    value.trim();
    if (name.equalsIgnoreCase("Transfer-Encoding") && value.indexOf("chunked") >= 0) *chunked = true;
    else if (name.equalsIgnoreCase("Content-Length")) *contentLength = (size_t)value.toInt();
  }
  return code;
}

template <class T>
size_t readChunkSize(T& client) {
  String line = client.readStringUntil('\n');
  line.trim();
  int semi = line.indexOf(';');
  if (semi >= 0) line = line.substring(0, semi);
  return (size_t)strtoul(line.c_str(), NULL, 16);
}

// Defensive cap for buffered non-stream responses: the ESP32 has no RAM to
// buffer an arbitrarily large JSON body, so stop at a sane ceiling instead of
// crashing the whole gateway on an OOM. Returns what was read (may be partial).
#define BODY_BUF_LIMIT (192 * 1024)
template <class T>
String readBodyManual(T& client, bool chunked, size_t contentLength) {
  String body;
  body.reserve(4096);
  uint8_t buf[1024];
  bool capped = false;
  auto add = [&](const uint8_t* p, size_t n) {
    if (!capped && body.length() + n <= BODY_BUF_LIMIT && ESP.getFreeHeap() >= 24 * 1024) {
      body.concat((const char*)p, n);
    } else {
      capped = true;
    }
  };
  if (chunked) {
    while (!capped) {
      size_t sz = readChunkSize(client);
      if (sz == 0) { client.readStringUntil('\n'); break; }
      size_t remaining = sz;
      while (remaining > 0 && !capped) {
        size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        int r = client.read(buf, want);
        if (r <= 0) { remaining = 0; break; }
        add(buf, r);
        remaining -= r;
      }
      client.readStringUntil('\n');
    }
  } else if (contentLength > 0) {
    size_t remaining = contentLength;
    while (remaining > 0 && !capped) {
      size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      int r = client.read(buf, want);
      if (r <= 0) break;
      add(buf, r);
      remaining -= r;
    }
  } else {
    unsigned long startT = millis();
    while ((client.connected() || client.available()) && !capped) {
      if (client.available()) {
        int r = client.read(buf, sizeof(buf));
        if (r <= 0) break;
        add(buf, r);
        startT = millis();
      } else {
        if (millis() - startT > 5000) break;
        delay(1);
      }
    }
  }
  return body;
}

template <class T>
bool streamResponseManual(T& client, bool chunked, size_t contentLength,
                          NetworkClient& c, uint32_t* pt, uint32_t* ct, uint32_t* tt) {
  bool sent = true;
  c.print("HTTP/1.1 200 OK\r\n");
  c.print("Content-Type: text/event-stream\r\n");
  c.print("Access-Control-Allow-Origin: *\r\n");
  c.print("Cache-Control: no-cache\r\n");
  c.print("Transfer-Encoding: chunked\r\n");
  c.print("Connection: close\r\n\r\n");
  uint8_t buf[1024];
  String tail;
  tail.reserve(4096);
  size_t remaining = contentLength;
  unsigned long lastData = millis();

  // Relay one block downstream as its own HTTP chunk. Returns false when the
  // client socket died mid-stream.
  auto relayBlock = [&](const uint8_t* p, size_t n) -> bool {
    c.printf("%x\r\n", (unsigned)n);
    if (c.write(p, n) != n) return false;
    c.print("\r\n");
    c.flush();
    tail.concat((const char*)p, n);
    if (tail.length() > 4096) tail.remove(0, tail.length() - 4096);
    return true;
  };

  if (chunked) {
    // A single upstream chunk may span many buffers: consume it fully (plus its
    // CRLF) before parsing the next chunk-size line, otherwise the leftover
    // bytes of a large chunk are misread as a bogus size and the stream dies.
    for (;;) {
      size_t sz = readChunkSize(client);
      if (sz == 0) { client.readStringUntil('\n'); break; }
      size_t left = sz;
      while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : left;
        int r = client.read(buf, want);
        if (r <= 0) { sent = false; break; }
        lastData = millis();
        if (!relayBlock(buf, (size_t)r)) { sent = false; break; }
        left -= r;
      }
      if (!sent) break;
      client.readStringUntil('\n');  // chunk terminator
    }
  } else if (contentLength > 0) {
    while (remaining > 0) {
      size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      int r = client.read(buf, want);
      if (r <= 0) { sent = false; break; }
      lastData = millis();
      if (!relayBlock(buf, (size_t)r)) { sent = false; break; }
      remaining -= r;
    }
  } else {
    // No length and no chunking: read until the upstream closes, with an idle
    // timeout so a stalled peer cannot wedge the proxy task forever.
    for (;;) {
      if (!client.available()) {
        if (!client.connected() || millis() - lastData > 5000) break;
        delay(1);
        continue;
      }
      int r = client.read(buf, sizeof(buf));
      if (r <= 0) break;
      lastData = millis();
      if (!relayBlock(buf, (size_t)r)) { sent = false; break; }
    }
  }
  if (sent) { c.print("0\r\n\r\n"); c.flush(); }
  if (!parseUsage(tail, *pt, *ct, *tt)) { *pt = *ct = *tt = 0; }
  return sent;
}

// ---------------------------------------------------------------------------
// Proxy engine (runs on Core 1)
// ---------------------------------------------------------------------------
// Returns the upstream HTTP status code. Special values:
//   0  -> the connection failed before any body byte was relayed, so the caller
//        may safely fail over to the next provider (the body is still intact
//        in g_bodyStream).
//   -1 -> the upload was aborted by the client, or the socket died while the
//        body/response was in flight. The body may be partially consumed, so
//        the caller MUST NOT fail over.
template <class T>
int relayUpstream(T& client, ProxyJob* job, NetworkClient& c,
                  const String& providerId, const String& providerKey,
                  const String& host, uint16_t port, const String& path,
                  const String& fullModel, uint32_t& outPt, uint32_t& outCt,
                  uint32_t& outTt, uint32_t& latMs, bool* delivered) {
  unsigned long t0 = millis();
  if (!client.connect(host.c_str(), port)) {
    latMs = millis() - t0;
    return 0;  // body not consumed yet -> retryable
  }

  // request headers (chunked upload)
  client.printf("POST %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", host.c_str());
  client.print("Content-Type: application/json\r\n");
  if (providerKey.length()) {
    client.print("Authorization: Bearer ");
    client.print(providerKey);
    client.print("\r\n");
  }
  client.print(job->isStream ? "Accept: text/event-stream\r\n" : "Accept: application/json\r\n");
  if (providerId == "openrouter") {
    client.print("HTTP-Referer: http://" + WiFi.localIP().toString() + "\r\n");
    client.print("X-Title: NixRoute\r\n");
  }
  client.print("Transfer-Encoding: chunked\r\n");
  client.print("Connection: close\r\n\r\n");

  // stream the request body directly using chunked encoding
  uint8_t buf[1024];
  for (;;) {
    if (job->aborted) {  // origin client vanished mid-upload
      client.stop();
      latMs = millis() - t0;
      return -1;
    }
    size_t r = xStreamBufferReceive(g_bodyStream, buf, sizeof(buf), pdMS_TO_TICKS(200));
    if (r > 0) {
      client.printf("%x\r\n", (unsigned)r);
      if (client.write(buf, r) != r) {  // upstream socket closed mid-body
        client.stop();
        latMs = millis() - t0;
        return -1;
      }
      client.print("\r\n");
    } else if (job->bodyDone) {
      break;  // whole body relayed
    }
  }
  if (job->aborted) {  // abort raced with the final body chunk
    client.stop();
    latMs = millis() - t0;
    return -1;
  }
  client.print("0\r\n\r\n");
  job->bodySent = true;  // body consumed: no failover is possible anymore

  // read response
  bool chunked;
  size_t contentLength;
  int code = readResponseHeaders(client, &chunked, &contentLength);
  latMs = millis() - t0;

  bool ok = (code >= 200 && code < 300);
  if (ok) {
    job->responseStarted = true;  // 200 head is about to hit the client
    uint32_t pt = 0, ct = 0, tt = 0;
    bool sent = true;
    if (job->isStream) {
      sent = streamResponseManual(client, chunked, contentLength, c, &pt, &ct, &tt);
    } else {
      String resp = readBodyManual(client, chunked, contentLength);
      parseUsage(resp, pt, ct, tt);
      sent = writeJson(c, 200, resp.length() ? resp : "{}");
    }
    if (delivered) *delivered = sent;
    outPt = pt; outCt = ct; outTt = tt;
    if (!sent) {  // origin socket is gone; nothing further to relay
      client.stop();
      return -1;
    }
  } else {
    readBodyManual(client, chunked, contentLength);  // drain
  }
  client.stop();
  return code;
}

int doUpstream(const ProviderSnap& p, ProxyJob* job, NetworkClient& c,
               const String& fullModel, uint32_t& outPt, uint32_t& outCt,
               uint32_t& outTt, uint32_t& latMs, bool* delivered) {
  outPt = outCt = outTt = 0;
  if (delivered) *delivered = false;
  String url = apiRoot(p.url) + "/chat/completions";
  bool secure = url.startsWith("https://");
  String host;
  uint16_t port;
  String path;
  parseUrl(url, host, port, path);

  unsigned long t0 = millis();
  int code;
  if (secure) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(60000);  // LLM responses can take many seconds
    code = relayUpstream(client, job, c, p.id, p.key, host, port, path,
                         fullModel, outPt, outCt, outTt, latMs, delivered);
  } else {
    NetworkClient client;  // plain TCP for http:// providers
    client.setTimeout(60000);
    code = relayUpstream(client, job, c, p.id, p.key, host, port, path,
                         fullModel, outPt, outCt, outTt, latMs, delivered);
  }
  latMs = millis() - t0;
  bool ok = (code >= 200 && code < 300);
  bumpMetrics(p.id, ok, code, latMs);
  recordProviderResult(p.id, ok, code);
  return code;
}

void processProxyJob(ProxyJob* job) {
  NetworkClient& c = job->client;

  int lastCode = 0;
  bool ok = false;
  bool delivered = false;
  bool usedProvider = false;
  String usedModel;
  unsigned long t0 = millis();
  uint32_t pt = 0, ct = 0, tt = 0, latMs = 0;
  uint32_t start = g_rr++ % job->n;

  for (int k = 0; k < job->n && !job->aborted; k++) {
    const ProviderSnap& p = job->providers[(start + k) % job->n];
    String fullModel = p.id + "/" + job->upstreamModel;
    bool sent = false;
    int code = doUpstream(p, job, c, fullModel, pt, ct, tt, latMs, &sent);
    lastCode = code;
    usedProvider = true;
    usedModel = fullModel;
    if (sent) delivered = true;
    if (code >= 200 && code < 300) { ok = true; break; }
    // Fail over ONLY when the connection died before any body byte was relayed
    // (code == 0 && !bodySent). HTTP errors or post-body failures are final.
    if (code == 0 && !job->bodySent) continue;
    break;
  }

  bool aborted = job->aborted;

  // Record usage exactly once per client request.
  if (ok) {
    recordUsage(usedModel.c_str(), pt, ct, tt, latMs, delivered);
  } else if (!aborted) {
    recordUsage(job->model.c_str(), 0, 0, 0, millis() - t0, false);
  }

  if (!ok && !aborted && !job->responseStarted) {
    // Never append a JSON error after a 200/SSE head was already streamed;
    // in that case just close the connection (Connection: close is set).
    if (lastCode == 429) writeErrorRaw(c, 429, "all providers rate-limited");
    else if (lastCode >= 500 || lastCode <= 0) writeErrorRaw(c, 502, "all providers failed");
    else writeErrorRaw(c, lastCode > 0 ? lastCode : 502, "upstream error");
  }

  // broadcast real-time telemetry to the dashboard
  JsonDocument telem;
  telem["type"] = "request";
  telem["model"] = job->model;
  telem["latency_ms"] = millis() - t0;
  telem["tokens"] = ok ? tt : 0;
  telem["status"] = (ok && delivered) ? "ok" : "fail";
  String tj;
  serializeJson(telem, tj);
  broadcastWs(tj);

  Serial.printf("chat model=%s candidates=%d code=%d delivered=%d used=%d aborted=%d heap=%d core=%d\n",
                job->model.c_str(), job->n, lastCode, delivered ? 1 : 0,
                usedProvider ? 1 : 0, aborted ? 1 : 0,
                ESP.getFreeHeap(), xPortGetCoreID());
  if (ok && !delivered)
    Serial.printf("WARN: upstream 200 but client write failed (socket closed early?)\n");
  c.stop();

  statsLock();
  if (ok && delivered) {
    g_reqOk++;
    g_latencySum += millis() - t0;
  } else if (!aborted) {
    g_reqFail++;
  }
  if (g_currentJob == job) g_currentJob = NULL;
  statsUnlock();
}

void proxyTask(void*) {
  for (;;) {
    ProxyJob* job = NULL;
    if (xQueueReceive(g_proxyQueue, &job, portMAX_DELAY) == pdTRUE) {
      processProxyJob(job);
      delete job;
    }
  }
}

// ---------------------------------------------------------------------------
// WebSocket live telemetry (Core 0)
// ---------------------------------------------------------------------------
String wsAcceptKey(const String& key) {
  String k = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t sha[20];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)k.c_str(), k.length());
  mbedtls_md_finish(&ctx, sha);
  mbedtls_md_free(&ctx);

  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  for (int i = 0; i < 20; i += 3) {
    uint32_t v = (sha[i] << 16) | ((i + 1 < 20 ? sha[i + 1] : 0) << 8) | (i + 2 < 20 ? sha[i + 2] : 0);
    out += b64[(v >> 18) & 0x3F];
    out += b64[(v >> 12) & 0x3F];
    out += (i + 1 < 20) ? b64[(v >> 6) & 0x3F] : '=';
    out += (i + 2 < 20) ? b64[v & 0x3F] : '=';
  }
  return out;
}

void wsSendFrame(WiFiClient& c, const String& msg) {
  size_t len = msg.length();
  uint8_t hdr[10];
  int h = 0;
  hdr[h++] = 0x81;  // FIN + text opcode
  if (len < 126) {
    hdr[h++] = (uint8_t)len;
  } else if (len < 65536) {
    hdr[h++] = 126;
    hdr[h++] = (uint8_t)((len >> 8) & 0xFF);
    hdr[h++] = (uint8_t)(len & 0xFF);
  } else {
    hdr[h++] = 127;
    for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((len >> (i * 8)) & 0xFF);
  }
  c.write(hdr, h);
  c.write((const uint8_t*)msg.c_str(), len);
}

void broadcastWs(const String& msg) {
  // Snapshot the connected clients under the lock, then do the blocking
  // socket writes outside it so /health and /api/state never stall behind
  // a slow WebSocket client.
  WiFiClient targets[MAX_WS_CLIENTS];
  int nTargets = 0;
  statsLock();
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (wsClients[i] && wsClients[i].connected()) targets[nTargets++] = wsClients[i];
  }
  statsUnlock();
  for (int i = 0; i < nTargets; i++) wsSendFrame(targets[i], msg);
}

void wsAcceptClients() {
  WiFiClient nc = wsServer.available();
  if (!nc) return;

  // Reserve a slot up-front; if every slot is busy, reject before we ever send
  // the 101 so the browser gets a clear failure instead of a silently dropped
  // socket that it would reconnect every 3 seconds forever.
  statsLock();
  int slot = -1;
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!wsClients[i] || !wsClients[i].connected()) { slot = i; break; }
  }
  statsUnlock();
  if (slot < 0) {
    nc.print("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    nc.flush();
    nc.stop();
    return;
  }

  String key = "";
  bool upgrade = false;
  // Read the handshake headers with a timeout instead of only draining whatever
  // happens to be buffered right now (headers may arrive in several segments).
  nc.setTimeout(5000);
  unsigned long startT = millis();
  bool first = true;
  while (millis() - startT <= 5000) {
    if (!nc.available()) { delay(1); continue; }
    String line = nc.readStringUntil('\n');
    line.trim();
    if (first) { first = false; if (!line.startsWith("GET")) { nc.stop(); return; } }
    if (line.length() == 0) break;
    if (line.startsWith("Upgrade:") && line.indexOf("websocket") >= 0) upgrade = true;
    if (line.startsWith("Sec-WebSocket-Key:")) { key = line.substring(18); key.trim(); }
  }
  if (!upgrade || key.length() == 0) { nc.stop(); return; }
  nc.print("HTTP/1.1 101 Switching Protocols\r\n");
  nc.print("Upgrade: websocket\r\n");
  nc.print("Connection: Upgrade\r\n");
  nc.print("Sec-WebSocket-Accept: " + wsAcceptKey(key) + "\r\n\r\n");
  statsLock();
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (!wsClients[i] || !wsClients[i].connected()) { wsClients[i] = nc; break; }
  }
  statsUnlock();
}

// ---------------------------------------------------------------------------
// Public API (OpenAI-compatible) — Core 0
// ---------------------------------------------------------------------------
void handleHealth() {
  bool conn = WiFi.status() == WL_CONNECTED;
  bool apMode = (WiFi.getMode() == WIFI_AP);
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  // Snapshot the cross-core counters atomically before rendering.
  uint32_t rTotal, rOk, rFail, tPrompt, tComp, tTotal;
  uint32_t latSum;
  statsLock();
  rTotal = g_reqTotal; rOk = g_reqOk; rFail = g_reqFail; latSum = g_latencySum;
  tPrompt = g_totalPrompt; tComp = g_totalCompletion; tTotal = g_totalTokens;
  statsUnlock();

  JsonDocument doc;
  doc["status"] = conn || apMode ? "ok" : "wifi_disconnected";
  doc["ap_mode"] = apMode;
  doc["uptime_s"] = millis() / 1000;
  doc["wifi_connected"] = conn;
  doc["ip"] = ip;
  doc["rssi"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["heap_total"] = ESP.getHeapSize();
  doc["requests_total"] = rTotal;
  doc["requests_ok"] = rOk;
  doc["requests_fail"] = rFail;
  doc["avg_latency_ms"] = rOk ? (latSum / rOk) : 0;
  doc["local_token_set"] = g_tokenCount > 0;
  doc["providers"] = g_providerCount;
  doc["tokens"]["prompt"] = tPrompt;
  doc["tokens"]["completion"] = tComp;
  doc["tokens"]["total"] = tTotal;

  JsonArray provs = doc["provider_metrics"].to<JsonArray>();
  statsLock();
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
  statsUnlock();
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
        mo["id"] = "nx/" + g_providers[i].id + "/" + m;
        mo["object"] = "model";
        mo["owned_by"] = g_providers[i].id;
        if (comma < 0) break;
        start = comma + 1;
      }
    } else {
      JsonObject mo = data.add<JsonObject>();
      mo["id"] = "nx/" + g_providers[i].id + "/auto";
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
  server.sendHeader("Access-Control-Allow-Headers", "Authorization, Content-Type, X-NixRoute");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.send(204, "", "");
}

// Zero-copy streaming helpers (run on Core 0, in the WebServer task).
// The head is buffered until the "model" key is seen (up to MODEL_HEAD_MAX
// bytes) so requests with a large preamble before "model" are still routable.
// Once routed, the rewritten head and the rest of the body stream to the proxy
// task without ever being buffered in full.
#define MODEL_HEAD_MAX 16384
static const char* STREAM_OPTS = ",\"stream_options\":{\"include_usage\":true}";

// Robust boolean-JSON-flag lookup: matches "stream":true regardless of the
// whitespace around the colon.
bool jsonFlagTrue(const String& head, const char* key) {
  String pat = String("\"") + key + "\"";
  int k = head.indexOf(pat);
  if (k < 0) return false;
  int i = head.indexOf(':', k + pat.length());
  if (i < 0) return false;
  i++;
  while (i < (int)head.length() && (head[i] == ' ' || head[i] == '\t')) i++;
  return head.substring(i).startsWith("true");
}

// Detects "model" in g_head, rewrites it to the upstream name, injects
// stream_options, snapshots the candidate providers and enqueues the job.
// Returns true when a job is now streaming the (rewritten) head. On failure it
// records the error in g_chatError/g_chatErrorCode and returns false without
// touching the shared body pipe.
bool detectAndStream() {
  String model = "";
  int mi = g_head.indexOf("\"model\"");
  if (mi >= 0) {
    int q1 = g_head.indexOf('"', mi + 7);
    int q2 = q1 >= 0 ? g_head.indexOf('"', q1 + 1) : -1;
    if (q1 > 0 && q2 > q1) model = g_head.substring(q1 + 1, q2);
  }
  if (!model.length()) { g_chatError = "missing model"; g_chatErrorCode = 400; return false; }

  bool isStream = jsonFlagTrue(g_head, "stream");

  int candidates[MAX_PROVIDERS];
  String upstreamModel;
  int n = resolveCandidates(model, candidates, &upstreamModel);
  if (n < 0) { g_chatError = "requested provider is disabled or cooling down"; g_chatErrorCode = 503; return false; }
  if (n == 0) { g_chatError = "no active provider available"; g_chatErrorCode = 500; return false; }

  // rewrite model value in the head
  int q1 = g_head.indexOf('"', mi + 7);
  int q2 = q1 >= 0 ? g_head.indexOf('"', q1 + 1) : -1;
  if (q1 > 0 && q2 > q1)
    g_head = g_head.substring(0, q1 + 1) + upstreamModel + g_head.substring(q2);

  // inject stream_options right after the (rewritten) model value
  if (isStream && g_head.indexOf("stream_options") < 0) {
    int m2 = g_head.indexOf("\"model\"");
    int a1 = g_head.indexOf('"', m2 + 7);
    int a2 = a1 >= 0 ? g_head.indexOf('"', a1 + 1) : -1;
    if (a1 > 0 && a2 > a1)
      g_head = g_head.substring(0, a2 + 1) + STREAM_OPTS + g_head.substring(a2 + 1);
  }

  ProxyJob* job = new ProxyJob();
  job->model = model;
  job->upstreamModel = upstreamModel;
  job->isStream = isStream;
  job->n = n;
  job->bodyDone = false;
  job->aborted = false;
  job->bodySent = false;
  job->responseStarted = false;
  for (int k = 0; k < n; k++) {
    int idx = candidates[k];
    job->providers[k].id = g_providers[idx].id;
    job->providers[k].url = g_providers[idx].url;
    job->providers[k].key = g_providers[idx].key;
  }
  job->client = server.client();

  if (xQueueSend(g_proxyQueue, &job, 0) != pdTRUE) {
    delete job;
    g_chatError = "proxy busy, try again";
    g_chatErrorCode = 503;
    return false;
  }
  g_ownsJob = true;
  g_currentJob = job;

  // stream the rewritten head into the body pipe, then release the head buffer
  xStreamBufferSend(g_bodyStream, (const uint8_t*)g_head.c_str(), g_head.length(), pdMS_TO_TICKS(10000));
  g_head = "";
  return true;
}

// Raw body handler: streams the request body to the proxy task chunk-by-chunk
// without ever buffering it fully in RAM.
void handleChatRaw() {
  HTTPRaw& raw = server.raw();

  if (raw.status == RAW_START) {
    g_head = "";
    g_headDone = false;
    g_chatError = "";
    g_chatErrorCode = 0;
    g_ownsJob = false;
    // g_bodyStream/g_head/g_currentJob are single globals: only one chat upload
    // may be in flight. A second request is rejected outright and MUST NOT touch
    // the running job's state or drain the shared body pipe.
    if (g_currentJob) {
      g_chatError = "proxy busy, try again";
      g_chatErrorCode = 503;
      g_headDone = true;
      return;
    }
    // Bound how long a slow or stalled client can pin the web task (anti-WDT
    // / anti-DoS), and refuse absurd bodies up front instead of draining them.
    server.client().setTimeout(20000);
    if (server.hasHeader("Content-Length")) {
      long cl = server.header("Content-Length").toInt();
      if (cl > 0 && cl > (2L * 1024 * 1024)) {
        server.client().stop();  // drop: not worth draining megabytes
        return;
      }
    }
    // Safe to start a fresh upload only now: discard stale bytes an aborted or
    // errored previous request may have left in the shared body pipe.
    if (g_bodyStream) xStreamBufferReset(g_bodyStream);
    if (!authCheck()) { g_chatError = "unauthorized"; g_chatErrorCode = 401; g_headDone = true; }
    return;
  }

  if (raw.status == RAW_END) {
    // Signal the proxy task that this job's body is complete. Only the request
    // that actually created the job may touch it (g_ownsJob), so a rejected
    // overlapping request can never corrupt or free another job's state.
    if (g_ownsJob && g_currentJob) g_currentJob->bodyDone = true;
    if (!g_headDone && g_chatError.length() == 0) {
      g_chatError = "missing model";
      g_chatErrorCode = 400;
    }
    return;
  }

  if (raw.status == RAW_ABORTED) {
    // Client vanished before the body was fully read (library readBytes()==0).
    // Tell the proxy task to drop this job instead of waiting forever for a
    // body that will never arrive, which would wedge the single-flight slot.
    if (g_ownsJob && g_currentJob) g_currentJob->aborted = true;
    g_headDone = true;
    return;
  }

  // RAW_WRITE
  if (g_chatError.length() > 0) return;  // discard body on error

  const uint8_t* buf = raw.buf;
  size_t len = raw.currentSize;

  if (!g_headDone) {
    size_t toHead = len;
    if (g_head.length() + toHead > MODEL_HEAD_MAX) toHead = MODEL_HEAD_MAX - g_head.length();
    g_head.concat((const char*)buf, toHead);
    size_t overflow = len - toHead;

    if (g_head.indexOf("\"model\"") >= 0 || g_head.length() >= MODEL_HEAD_MAX) {
      bool started = detectAndStream();
      g_headDone = true;
      // Only relay bytes past the head window when a job actually started;
      // otherwise they would poison the shared pipe for the next request.
      if (started && overflow)
        xStreamBufferSend(g_bodyStream, buf + toHead, overflow, pdMS_TO_TICKS(10000));
    }
    // model not found yet -> keep buffering until it appears or the head cap
  } else {
    xStreamBufferSend(g_bodyStream, buf, len, pdMS_TO_TICKS(10000));
  }
}

void handleChat() {
  // Runs after the body has been fully streamed. The proxy task (Core 1) owns
  // the response; here we only surface early errors detected during streaming.
  statsLock();
  g_reqTotal++;
  statsUnlock();
  if (g_chatError.length() > 0) {
    statsLock();
    g_reqFail++;
    statsUnlock();
    sendError(g_chatErrorCode, g_chatError);
  }
}

void handleNotFound() { sendError(404, "not found"); }

// /admin/status mirrors /health but requires an authenticated admin session.
void handleAdminStatus() {
  if (!requireAdmin()) return;
  handleHealth();
}

// ---------------------------------------------------------------------------
// Admin JSON API — Core 0
// ---------------------------------------------------------------------------
void handleApiState() {
  if (!requireAdmin()) return;
  bool conn = WiFi.status() == WL_CONNECTED;
  bool apMode = (WiFi.getMode() == WIFI_AP);
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  // Snapshot the cross-core counters atomically before rendering.
  uint32_t rTotal, rOk, rFail, latSum;
  statsLock();
  rTotal = g_reqTotal; rOk = g_reqOk; rFail = g_reqFail; latSum = g_latencySum;
  statsUnlock();

  JsonDocument doc;
  doc["version"] = VERSION;
  doc["wifi"]["ssid"] = g_wifiSsid;
  doc["wifi"]["connected"] = conn;
  doc["wifi"]["ap_mode"] = apMode;
  doc["wifi"]["ip"] = ip;
  doc["wifi"]["rssi"] = WiFi.RSSI();
  doc["token"]["set"] = g_tokenCount > 0;
  doc["token"]["count"] = g_tokenCount;
  doc["token"]["max"] = MAX_TOKENS;
  JsonArray tokArr = doc["token"]["list"].to<JsonArray>();
  for (int i = 0; i < g_tokenCount; i++) tokArr.add(g_tokens[i]);
  doc["stats"]["uptime_s"] = millis() / 1000;
  doc["stats"]["heap"] = ESP.getFreeHeap();
  doc["stats"]["heap_total"] = ESP.getHeapSize();
  doc["stats"]["requests_total"] = rTotal;
  doc["stats"]["requests_ok"] = rOk;
  doc["stats"]["requests_fail"] = rFail;
  doc["stats"]["avg_latency_ms"] = rOk ? (latSum / rOk) : 0;

  statsLock();
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
    o["cooling"] = inCooldown(g_providers[i].coolDownUntil);
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
        models.add("nx/" + g_providers[i].id + "/" + m);
        if (comma < 0) break;
        start = comma + 1;
      }
    }
  }
  statsUnlock();

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

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

  // An explicit "id" marks this as an EDIT of an existing provider. Without it
  // the call is an ADD, and a name whose slug collides with an existing id must
  // be rejected instead of silently overwriting the other provider.
  String idIn = doc["id"] | "";
  idIn.trim();

  statsLock();
  int idx;
  if (idIn.length()) {
    idx = findProvider(idIn);
    if (idx < 0) {
      statsUnlock();
      sendError(404, "provider not found");
      return;
    }
  } else {
    String sid = slugify(name);
    idx = findProvider(sid);
    if (idx >= 0) {
      statsUnlock();
      sendError(409, "a provider with this name already exists - pick a distinct name");
      return;
    }
    if (g_providerCount >= MAX_PROVIDERS) {
      statsUnlock();
      sendError(409, "provider limit reached (" + String(MAX_PROVIDERS) + ")");
      return;
    }
    idx = g_providerCount++;
    g_providers[idx].id = sid;
  }
  String id = g_providers[idx].id;
  g_providers[idx].name = name;
  g_providers[idx].url = url;
  g_providers[idx].active = active;
  if (key.length()) g_providers[idx].key = key;
  saveProviders();
  statsUnlock();

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

void handleApiProviderRemove() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String id = doc["id"] | "";
  id.trim();
  statsLock();
  int idx = findProvider(id);
  if (idx < 0) { statsUnlock(); sendError(404, "provider not found"); return; }
  for (int i = idx; i < g_providerCount - 1; i++) {
    g_providers[i] = g_providers[i + 1];
    g_providerModels[i] = g_providerModels[i + 1];
  }
  g_providerCount--;
  saveProviders();
  statsUnlock();
  prefs.begin("gateway", false);
  prefs.remove(("models_" + id).c_str());
  prefs.end();
  sendJson(200, "{\"ok\":true}");
}

void handleApiProviderToggle() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String id = doc["id"] | "";
  bool active = doc["active"] | true;
  id.trim();
  statsLock();
  int idx = findProvider(id);
  if (idx < 0) { statsUnlock(); sendError(404, "provider not found"); return; }
  g_providers[idx].active = active;
  saveProviders();
  statsUnlock();
  sendJson(200, "{\"ok\":true}");
}

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

void handleApiProviderPing() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String id = doc["id"] | "";
  id.trim();
  int idx = findProvider(id);
  if (idx < 0) { sendError(404, "provider not found"); return; }

  String url = apiRoot(g_providers[idx].url) + "/models";
  bool secure = url.startsWith("https://");
  unsigned long t0 = millis();
  int code = 0;
  if (secure) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (http.begin(client, url)) {
      if (g_providers[idx].key.length())
        http.addHeader("Authorization", "Bearer " + g_providers[idx].key);
      http.setTimeout(10000);
      code = http.GET();
      http.end();
    }
  } else {
    HTTPClient http;
    if (http.begin(url)) {
      if (g_providers[idx].key.length())
        http.addHeader("Authorization", "Bearer " + g_providers[idx].key);
      http.setTimeout(10000);
      code = http.GET();
      http.end();
    }
  }
  unsigned long lat = millis() - t0;

  JsonDocument out;
  out["ok"] = (code >= 200 && code < 300);
  out["status"] = code;
  out["latency_ms"] = lat;
  String s;
  serializeJson(out, s);
  sendJson(200, s);
}

void handleApiReboot() {
  if (!requireAdmin()) return;
  sendJson(200, "{\"ok\":true}");
  delay(200);
  ESP.restart();
}

void handleApiTokenGenerate() {
  if (!requireAdmin()) return;
  if (g_tokenCount >= MAX_TOKENS) { sendError(409, "token limit reached (" + String(MAX_TOKENS) + ")"); return; }
  String t = genToken();
  g_tokens[g_tokenCount++] = t;
  saveTokens();
  JsonDocument out;
  out["ok"] = true;
  out["token"] = t;
  out["count"] = g_tokenCount;
  String s;
  serializeJson(out, s);
  sendJson(200, s);
}

void handleApiTokenDelete() {
  if (!requireAdmin()) return;
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  String tok = doc["token"] | "";
  tok.trim();
  if (!tok.length()) { sendError(400, "token required"); return; }
  for (int i = 0; i < g_tokenCount; i++) {
    if (g_tokens[i] == tok) {
      for (int j = i; j < g_tokenCount - 1; j++) g_tokens[j] = g_tokens[j + 1];
      g_tokenCount--;
      saveTokens();
      sendJson(200, "{\"ok\":true}");
      return;
    }
  }
  sendError(404, "token not found");
}

void handleApiTokenClear() {
  if (!requireAdmin()) return;
  g_tokenCount = 0;
  saveTokens();
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
  String html = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 500 500'%3E%3Crect x='70' y='70' width='92' height='375' rx='46' fill='%230c1a30'/%3E%3Crect x='338' y='70' width='92' height='375' rx='46' fill='%230c1a30'/%3E%3Cline x1='125' y1='130' x2='375' y2='380' stroke='%230c1a30' stroke-width='96' stroke-linecap='round'/%3E%3Cline x1='125' y1='130' x2='375' y2='380' stroke='%23ffffff' stroke-width='18' stroke-linecap='round'/%3E%3Ccircle cx='125' cy='130' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='125' cy='130' r='16' fill='%2300a8b5'/%3E%3Ccircle cx='250' cy='255' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='250' cy='255' r='16' fill='%2300a8b5'/%3E%3Ccircle cx='375' cy='380' r='34' fill='%230c1a30' stroke='%23ffffff' stroke-width='14'/%3E%3Ccircle cx='375' cy='380' r='16' fill='%2300a8b5'/%3E%3C/svg%3E">
<title>NixRoute — Sign in</title>
<style>
*{box-sizing:border-box}
html,body{height:100%}
body{margin:0;font-family:ui-sans-serif,system-ui,-apple-system,"SF Pro Display","Segoe UI",Roboto,sans-serif;color:#eef3ff;
 display:grid;place-items:center;overflow:hidden;padding:20px;
 background:radial-gradient(1200px 720px at 88% -12%,rgba(92,112,190,.32),transparent 62%),
   radial-gradient(900px 620px at -10% 18%,rgba(0,168,181,.20),transparent 55%),
   radial-gradient(820px 620px at 50% 118%,rgba(108,66,196,.18),transparent 60%),
   linear-gradient(180deg,#0c1126 0%,#080d1b 55%,#05070f 100%)}
.orb{position:fixed;border-radius:50%;filter:blur(80px);pointer-events:none;opacity:.5}
.orb.a{width:440px;height:440px;background:rgba(47,211,222,.20);top:-140px;right:-80px}
.orb.b{width:380px;height:380px;background:rgba(108,120,255,.20);bottom:-150px;left:-90px}
.card{position:relative;z-index:1;width:min(92vw,420px);padding:42px 38px 30px;border-radius:30px;
 background:linear-gradient(180deg,rgba(255,255,255,.095),rgba(255,255,255,.04));
 border:1px solid rgba(255,255,255,.14);
 -webkit-backdrop-filter:blur(40px) saturate(180%);backdrop-filter:blur(40px) saturate(180%);
 box-shadow:0 34px 90px rgba(2,6,20,.55),inset 0 1px 0 rgba(255,255,255,.16),inset 0 -1px 0 rgba(255,255,255,.04)}
.brand{display:flex;align-items:center;gap:14px;margin-bottom:10px}
.brand svg{width:46px;height:46px;flex:0 0 auto;filter:drop-shadow(0 8px 18px rgba(0,168,181,.35))}
h1{margin:0;font-size:21px;font-weight:700;letter-spacing:-.01em}
.sub{font-size:12.5px;color:#9fb0cf;margin-top:3px}
label{display:block;font-size:12px;font-weight:600;color:#a9b6d2;letter-spacing:.02em;margin:24px 0 9px}
input{width:100%;padding:14px 16px;border-radius:14px;border:1px solid rgba(255,255,255,.16);
 background:rgba(10,14,30,.45);color:#eef3ff;font-size:15px;outline:none;-webkit-backdrop-filter:blur(8px);backdrop-filter:blur(8px);
 transition:border-color .15s,box-shadow .15s}
input::placeholder{color:#6d7c9c}
input:focus{border-color:rgba(47,211,222,.7);box-shadow:0 0 0 4px rgba(47,211,222,.18)}
button{width:100%;margin-top:24px;padding:14px;border:0;border-radius:14px;cursor:pointer;
 font-size:15px;font-weight:700;color:#03141a;letter-spacing:.01em;
 background:linear-gradient(180deg,#4ae4ee,#00b7c4);
 box-shadow:0 12px 30px rgba(0,168,181,.38),inset 0 1px 0 rgba(255,255,255,.55)}
button:hover{filter:brightness(1.07)}
button:active{transform:scale(.985)}
.hint{font-size:12px;color:#7e8cb0;line-height:1.6;margin:26px 0 0;text-align:center}
code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:11px;color:#cfe6e8;
 background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.12);padding:1px 7px;border-radius:8px}
a{color:#3fd9e4;text-decoration:none}
@media (prefers-reduced-motion:no-preference){.card{animation:pop .45s cubic-bezier(.18,.9,.28,1.2)}}
@keyframes pop{from{opacity:0;transform:translateY(16px) scale(.975)}to{opacity:1;transform:none}}
</style></head>
<body>
<div class="orb a"></div><div class="orb b"></div>
<div class="card">
 <div class="brand"><svg viewBox="0 0 500 500" xmlns="http://www.w3.org/2000/svg"><rect x="70" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><rect x="338" y="70" width="92" height="375" rx="46" fill="#0c1a30"/><path d="M416 85l26-26m0 0h-24m24 0v24" fill="none" stroke="#0c1a30" stroke-width="15" stroke-linecap="round" stroke-linejoin="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#0c1a30" stroke-width="96" stroke-linecap="round"/><line x1="125" y1="130" x2="375" y2="380" stroke="#ffffff" stroke-width="18" stroke-linecap="round"/><circle cx="125" cy="130" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="125" cy="130" r="16" fill="#00a8b5"/><circle cx="250" cy="255" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="250" cy="255" r="16" fill="#00a8b5"/><circle cx="375" cy="380" r="34" fill="#0c1a30" stroke="#ffffff" stroke-width="14"/><circle cx="375" cy="380" r="16" fill="#00a8b5"/></svg><div><h1>NixRoute</h1><div class="sub">ESP32 AI Gateway</div></div></div>
 <form method="POST" action="/admin/login">
  <label for="pw">Password</label>
  <input id="pw" name="password" type="password" placeholder="••••••" autocomplete="current-password" required autofocus>
  <button>Sign In</button>
 </form>
 <p class="hint">Sign in to the dashboard. Default password <code>123456</code> — change it in Settings after signing in.</p>
</div>
</body></html>)HTML";
  server.send(200, "text/html", html);
}

void handleLoginPost() {
  String p = server.arg("password");
  if (p == g_adminPass) {
    server.sendHeader("Set-Cookie", "esp_auth=" + g_adminSession + "; Path=/; Max-Age=86400; HttpOnly");
    server.sendHeader("Location", "/");
    server.send(303, "", "");
  } else {
    delay(500);  // cheap throttle against password brute-forcing on the LAN
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
// Tasks
// ---------------------------------------------------------------------------
void webServerTask(void*) {
  for (;;) {
    server.handleClient();
    if (g_apMode) dnsServer.processNextRequest();
    wsAcceptClients();
  }
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
  // Random per-boot admin session token. The password itself is never logged.
  g_adminSession = genToken();
  Serial.printf("admin auth enabled | tokens %d | wifi %s | providers %d\n",
                g_tokenCount,
                g_wifiSsid.length() ? g_wifiSsid.c_str() : "(none)",
                g_providerCount);

  if (g_wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
    Serial.print("Connecting WiFi");
    int t = 0;
    while (WiFi.status() != WL_CONNECTED && t < 30) { delay(500); Serial.print("."); t++; }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nWiFi OK IP %s RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (MDNS.begin("nixroute")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS http://nixroute.local");
      }
    } else {
      Serial.printf("\nWiFi FAIL %d\n", WiFi.status());
    }
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("NixRoute-Setup", "12345678");
    g_apMode = true;
    // Captive portal: redirect every DNS name to the AP IP.
    dnsServer.start(53, "*", WiFi.softAPIP());
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
  server.on("/admin/status", HTTP_GET, handleAdminStatus);
  server.on("/v1/models", HTTP_GET, handleModels);
  server.on("/v1/chat/completions", HTTP_POST, handleChat, handleChatRaw);

  // Admin JSON API
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/providers", HTTP_POST, handleApiProviderAdd);
  server.on("/api/providers/remove", HTTP_POST, handleApiProviderRemove);
  server.on("/api/providers/toggle", HTTP_POST, handleApiProviderToggle);
  server.on("/api/providers/fetch", HTTP_POST, handleApiProviderFetch);
  server.on("/api/providers/ping", HTTP_POST, handleApiProviderPing);
  server.on("/api/reboot", HTTP_POST, handleApiReboot);
  server.on("/api/token/generate", HTTP_POST, handleApiTokenGenerate);
  server.on("/api/token/delete", HTTP_POST, handleApiTokenDelete);
  server.on("/api/token/clear", HTTP_POST, handleApiTokenClear);
  server.on("/api/password", HTTP_POST, handleApiPassword);
  server.on("/api/wifi", HTTP_POST, handleApiWifi);

  // CORS preflight
  server.on("/v1/chat/completions", HTTP_OPTIONS, handleOptions);
  server.on("/health", HTTP_OPTIONS, handleOptions);
  server.on("/v1/models", HTTP_OPTIONS, handleOptions);

  server.onNotFound(handleNotFound);
  const char* hk[] = {"Authorization", "Cookie", "Content-Length", "X-NixRoute"};
  server.collectHeaders(hk, 4);
  server.begin();
  wsServer.begin();

  // FreeRTOS primitives
  g_statsMutex = xSemaphoreCreateMutex();
  g_proxyQueue = xQueueCreate(PROXY_QUEUE_LEN, sizeof(ProxyJob*));
  g_bodyStream = xStreamBufferCreate(8192, 1);

  // Pin the WebServer to Core 0 and the proxy engine to Core 1.
  xTaskCreatePinnedToCore(webServerTask, "web", 8192, NULL, 1, &g_webTask, 0);
  xTaskCreatePinnedToCore(proxyTask, "proxy", 16384, NULL, 2, &g_proxyTask, 1);

  Serial.printf("HTTP :80 dashboard http://%s/ heap %d (web=Core0, proxy=Core1)\n",
                WiFi.localIP().toString().c_str(), ESP.getFreeHeap());
}

void loop() {
  // Work happens in the dedicated web (Core 0) and proxy (Core 1) tasks.
  vTaskDelay(portMAX_DELAY);
}

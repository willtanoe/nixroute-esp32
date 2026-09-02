# Architecture — ESP32-WROOM-32 AI API Router

_Date: 2026-09-02_
_Status: Phase 0 — decision record + blueprint_

---

## 1. Overview

```
PC — curl / OpenAI SDK / Claude Code / OpenCode
        │  OpenAI-compatible HTTP (LAN, plain)
        │  POST /v1/chat/completions, GET /health, GET /v1/models
        ▼
┌────────────────────────── ESP32-WROOM-32 ──────────────────────────┐
│  esp_http_server (httpd)  :80   Wi-Fi STA + lwIP + event loop      │
│  Gateway layer: auth, routing, provider adapter, fallback, stats   │
│  esp_http_client + esp-tls (mbedTLS)  :443  ──► upstream LLM APIs  │
│  NVS + config manager, Wi-Fi manager, stats, heap monitor         │
└────────────────────────────────────────────────────────────────────┘
        │ HTTPS (mbedTLS)  · serialized (1 upstream at a time)
        ▼
  Provider A ─ DeepSeek (OpenAI-compatible, first)
  Provider B ─ OpenRouter (OpenAI-compatible, second)
  Provider C ─ Anthropic / Gemini (adapter; non-OpenAI shape)
```

Goal: **smallest reliable embedded gateway** that lets a LAN client use `OPENAI_BASE_URL=http://esp32.local/v1` without a desktop router process.

Non-goals: full desktop router feature parity, database, JS runtime, large web dashboard, TLS termination inbound.

---

## 2. Decision Record

### D-01 · Framework: ESP-IDF via PlatformIO (not Arduino)

- **Decision**: Use ESP-IDF native (`framework = espidf`) built with PlatformIO.
- **Reason**: See `docs/research.md` §2. TLS dynamic buffers alone save ~20 KB. `esp_http_server` chunked/SSE and `esp_http_client` streaming events are first-class only in IDF; Arduino wrappers hide tuning.
- **Alternatives**: Arduino ESP32 (easier), Arduino-as-component bridge. Rejected: hides `menuconfig`, adds String heap churn, pins IDF version.
- **Trade-off**: Steeper initial scaffolding (event loops, manual Kconfig) vs long-term control.
- **Evidence**: Espressif `https_request` heap table; forum SSE thread requiring `httpd_socket_send`; Arduino shim diagram.

### D-02 · Inbound: Plain HTTP (no TLS)

- **Decision**: Gateway listens on port 80, plain HTTP, LAN-only.
- **Reason**: ESP32-WROOM-32 terminating TLS inbound would double mbedTLS footprint and CPU; LAN threat model tolerates it. Household/lab deployment is behind Wi-Fi WPA2; internet exposure via VPN if needed.
- **Alternative**: `CONFIG_HTTPD_SECURE` (HTTPS server) — rejected as excessive.
- **Consequence**: Document clearly: **do not port-forward without VPN**.

### D-03 · Concurrency: Serialized Upstream

- **Decision**: One upstream TLS session at a time, guarded by mutex/semaphore. Second concurrent `POST /v1/chat/completions` gets `429 Too Many Requests` (or queued 5 s).
- **Reason**: 520 KB SRAM; second simultaneous mbedTLS (~22 KB each with dynamic buffers + lwIP) fragments heap and starves streaming buffers.
- **Alternative**: Concurrent — rejected after heap analysis.
- **Evidence**: §4.2 heap per session; without PSRAM concurrent streams infeasible.

### D-04 · JSON Strategy: Validate Minimal, Forward Verbatim

- **Decision**: Parse only `model` + `stream`; forward original body bytes upstream. Avoid re-serialization duplication.
- **Reason**: Saves one full JSON copy (4–8 KB) + allocations.
- **Alternative**: Full parse then regenerate — rejected (heap waste).
- **Tool**: cJSON for control plane; bounded scan for proxy path; ArduinoJson pluggable.

### D-05 · JSON Library: cJSON

- **Decision**: Start with IDF-shipped cJSON.
- **Reason**: Zero extra dependency, low overhead. Sufficient for bounded extraction.
- **Alternative**: ArduinoJson (richer streaming parser) — defer, keep header-only option if validation grows.

### D-06 · Config Persistence: NVS (plain, not encrypted)

- **Decision**: Plain `nvs_flash` for Phase 1. Encrypted NVS documented as upgrade path.
- **Reason**: Simplicity, debuggability; flash encryption needs eFuse provisioning which blocks early iteration.
- **Alternative**: Encrypted NVS, SPIFFS file — deferred.
- **Risk**: Physical flash read exposes keys; documented; mitigation is flash encryption later.

### D-07 · Streaming: Handler-Blocking Chunked Forwarding

- **Decision**: Stay inside `httpd` handler for duration of stream, forwarding each `HTTP_EVENT_ON_DATA` via `httpd_resp_send_chunk`.
- **Reason**: Bounded LLM streams (seconds); no need to hijack socket. Simplest correct lifetime.
- **Alternative**: `httpd_socket_send` hijack — reserved if infinite streaming needed.
- **Consequence**: Must yield/watchdog feed in loop; handler stack sized accordingly.

### D-08 · HTTP Server Choice: IDF `esp_http_server` over PsychicHttp

- **Decision**: Use native `esp_http_server`. Evaluate PsychicHttp later only if async/WS needed.
- **Reason**: Minimal dependency, documented SSE pattern, lowest heap.
- **Evidence**: `http_server/simple` SSE example; PsychicHTTP MIT but heavier.

### D-09 · Partitions: Default + tuned OTA + minimal SPIFFS

- **Decision**: Use `default.csv` initially; tune if flash overflow (cert bundle ~100 KB). Reserve 24 KB NVS minimum.
- **Alternative**: Custom partitions 1.2 MB/1.2 MB — only if needed.

---

## 3. System Decomposition

```
firmware/
  main/
    app_main.c            # init: NVS, Wi-Fi, httpd, register routes
    wifi_manager.{c,h}    # event handlers, reconnect, stats
    http_gateway.{c,h}    # route table, httpd handle, auth middleware
    proxy_handler.{c,h}   # POST /v1/chat/completions dispatch
    provider.{c,h}        # provider_t abstraction + registry
    provider_deepseek.{c,h}
    provider_openrouter.{c,h}
    routing.{c,h}         # model → provider(s) match, priority/fallback list
    streaming.{c,h}       # chunked forwarding, SSE framing helpers
    config_manager.{c,h}  # NVS read/write, runtime config struct
    stats.{c,h}           # counters, heap, uptime, latency histogram
    auth.{c,h}            # Bearer check (LOCAL_API_TOKEN)
    json_util.{c,h}       # cJSON helpers, model/stream extraction
  components/             # (future: ArduinoJson component if needed)
  CMakeLists.txt / platformio.ini
```

### Module Contracts

| Module | Owns | Calls |
|---|---|---|
| `wifi_manager` | Wi-Fi, IP, reconnect timer, RSSI | NVS config, stats |
| `http_gateway` | httpd lifecycle, route registration, CORS | auth, proxy_handler, health, models |
| `proxy_handler` | Request validation, routing lookup, provider dispatch, response | routing, provider, auth, streaming, stats |
| `provider` | `provider_t` vtable: `transform_request`, `build_url`, `auth_header`, `is_retryable` | esp_http_client |
| `routing` | `route_table[]` — `prefix → provider_priority[]` | config_manager |
| `streaming` | Forward loop: `esp_http_client` event → `httpd_resp_send_chunk` | httpd, stats, watchdog |
| `config_manager` | `gateway_config_t` in RAM + NVS persistence | NVS |
| `stats` | Atomic counters, `esp_timer_get_time()` latency | — |
| `auth` | Bearer compare (constant-time) | config |

---

## 4. Provider Abstraction

```c
typedef struct provider {
    const char *name;               // "deepseek"
    const char *base_url;           // "https://api.deepseek.com"
    const char *models_prefix;      // "deepseek-" (match prefix)
    esp_err_t (*build_request)(const char *orig_body, size_t len, cJSON **out);
    const char* (*auth_header_value)(void); // "Bearer " + key (from NVS)
    bool (*is_retryable)(int http_status, esp_err_t err);
    bool openai_compatible;         // true → forward body verbatim
} provider_t;
```

- **Phase 1**: single `deepseek` provider (OpenAI-compatible). Register one entry.
- **Phase 2+**: generic `openai_compatible` adapter for OpenRouter, etc. Anthropic/Gemini adapters override `build_request` to translate to their wire shapes.
- Registry: `provider_registry[]`, up to 4 entries.

---

## 5. Routing Engine

```c
typedef struct route_rule {
    const char *prefix;             // "deepseek-", "claude-", "gemini-"
    const char *providers[3];       // priority ordered
    size_t provider_count;
    int timeout_ms;
} route_rule_t;
```

- Algorithm: longest-prefix match on `model` field → ordered provider list.
- Default rule `""` → `["deepseek"]` (or first configured).
- Config persisted in NVS as JSON string `routing_json` (small). compile-time default in `Kconfig`/`sdkconfig`.

Fallback trigger: see §7.

---

## 6. API Surface

### Public (OpenAI-compatible)

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/health` | none | Machine-readable: wifi, heap, uptime, stats |
| `GET` | `/v1/models` | optional Bearer | List routed models (derived from registry) |
| `POST` | `/v1/chat/completions` | optional Bearer | Proxy (+ streaming `stream:true`) |
| `ANY` | `/*` (unknown) | — | 404 JSON |

### Admin (no secrets in response)

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/admin/status` | Bearer if set | Uptime, heap, wifi, build |
| `GET` | `/admin/providers` | Bearer if set | Provider names, health, last latency |
| `GET` | `/admin/stats` | Bearer if set | Counters (total/success/fail/fallback) |

All JSON responses set `Cache-Control: no-store`. CORS header `Access-Control-Allow-Origin: *` on `/v1/*`.

Auth: if `LOCAL_API_TOKEN` non-empty, every `/v1/*` and `/admin/*` requires `Authorization: Bearer <token>` (constant-time `memcmp`). Missing/invalid → `401`. Never echo token.

---

## 7. Retry & Fallback Policy

### Retry/fallback candidates (attempt next provider)

- Transport: `ESP_ERR_ESP_TLS_CONNECTION_FAILED`, `ESP_ERR_HTTP_CONNECT`, DNS fail, TLS handshake, timeout
- HTTP: `429`, `500`, `502`, `503`, `504`

### Non-retryable (return error immediately)

- `400` malformed, `401` provider invalid key, `402` quota, `403`, `404` model not found, client `httpd` 413/422 validation

### Algorithm

```
providers = routing_lookup(model)
for i in 0..providers.len-1:
  resp, status = do_request(providers[i], request)
  if status == 200 → return resp
  if provider[i].is_retryable(status, err) && i+1 < len:
      stats.fallback_count++
      backoff 100ms * (1<<i) capped 1s
      continue
  else break
return error (502 or provider's status mapped)
```

- One retry per fallback; no more than 3 providers chained.
- Streaming fallback nuance: once first byte forwarded downstream, fallback cannot safely restart (chunks already sent). So fallback only **before** first chunk flush. If upstream streams then fails, close downstream with truncated indicator; log.
- Total timeout wall clock: sum of per-provider timeouts (e.g., 15 s each) capped 30 s.

---

## 8. Streaming Subsystem

### Non-streaming path (`stream:false` or absent)

- `esp_http_client` with buffer up to 16 KB response; read via `HTTP_EVENT_ON_DATA` accum until `HTTP_EVENT_ON_FINISH` then `httpd_resp_send` exactly once. If response >16 KB, stream as chunks anyway (same codepath).

### Streaming path (`stream:true`)

- httpd handler sets headers: `200`, `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: keep-alive`.
- Create `esp_http_client_handle_t` with `buffer_size=1024`, `buffer_size_tx=1024`, `timeout_ms=15000`.
- Event handler accumulates into `stream_ctx_t` that holds `httpd_req_t*` and calls `httpd_resp_send_chunk(req, data, len)` inside `HTTP_EVENT_ON_DATA`. Uses global `proxy_mutex` to serialize.
- On `HTTP_EVENT_ON_FINISH`: `httpd_resp_send_chunk(req, NULL, 0)` then cleanup.
- Error/early close: `esp_http_client_close` + chunk terminator.
- Watchdog: in `while(perform)` or event, `esp_task_wdt_reset()` + `vTaskDelay(pdMS_TO_TICKS(1))` every 2 s.
- Heap: one `stream_buf[1024]` on handler stack; no per-token alloc.

Measured expectation:see `docs/memory-budget.md`.

---

## 9. Wi-Fi Subsystem

- `wifi_manager_init()`:
  - `nvs_flash_init` already done.
  - `esp_netif_init` + `esp_netif_create_default_wifi_sta`.
  - `esp_wifi_init` with default; set storage RAM, mode STA.
  - Set SSID/password from `gateway_config_t` (NVS or `sdkconfig` fallback).
  - Register `WIFI_EVENT` and `IP_EVENT` handlers.
  - `esp_wifi_start()`.
- Event table:

| Event | Action |
|---|---|
| `WIFI_EVENT_STA_START` | `esp_wifi_connect()` |
| `WIFI_EVENT_STA_DISCONNECTED` (reason) | log reason, update `stats.wifi_disconnects++`, schedule reconnect (exp backoff via `esp_timer`), notify httpd remain up |
| `IP_EVENT_STA_GOT_IP` | capture IP, start httpd if not started, reset backoff |
| `IP_EVENT_STA_LOST_IP` | mark disconnected |

- Reconnect timer: `esp_timer_handle` one-shot; interval `min(30000, 1000<<attempts)`. Add `esp_wifi_scan` diagnostic in debug build.
- Stats: `wifi_connected` bool, `rssi` (`esp_wifi_sta_get_ap_info`), `ip4`, `reconnect_count`.

---

## 10. Configuration

### Source of truth

```c
typedef struct {
  char wifi_ssid[32];
  char wifi_password[64];
  char local_token[64];       // "" = open (debug)
  char deepseek_key[80];
  char openrouter_key[80];
  char routing_json[256];     // optional override
  int  http_port;             // 80
  int  upstream_timeout_ms;    // 15000
  int  max_body_bytes;         // 8192
} gateway_config_t;
```

- Load order: NVS overrides > sdkconfig/Kconfig defaults > compiled defaults (example show in `.env.example` but not inside flash).
- Write path (future: `POST /admin/config` with auth) → validate → `nvs_set_str/blob` → `nvs_commit`. For Phase 1, serial/NVS tool fills keys; document.
- All secret fields omitted from serialization (never appear in JSON).

### Persistence contract

- `config_manager_load()` on boot.
- `config_manager_save()` on NVS write.
- Validate: SSID 1–32, keys non-empty if provider enabled.

---

## 11. Security

- Inbound auth: optional `LOCAL_API_TOKEN`. When empty → open (convenience). When set → every `/v1/*` requires Bearer (timing-safe compare). Return `401 {error:"unauthorized"}` on fail, no hints.
- Provider keys: held in NVS, never sent downstream. Logs never print `Authorization` header nor `api_key`.
- Logging: `ESP_LOGI` without payload; debug payload logging behind `LOG_PAYLOADS` Kconfig guard (off by default).
- CORS: permissive for LAN clients (browser parity).
- Physical: document that without flash encryption, NVS is readable via flash dump.

---

## 12. Observability & Stats

```c
typedef struct {
  uint32_t uptime_s;
  uint32_t requests_total;
  uint32_t requests_success;
  uint32_t requests_failed;
  uint32_t fallbacks;
  int      free_heap;
  int      min_free_heap;
  int32_t  last_provider_latency_ms;
  bool     wifi_connected;
  int8_t   wifi_rssi;
  char     ip_str[16];
  uint32_t wifi_disconnects;
} gateway_stats_t;
```

- Updated atomically (heap stats read at handler edge).
- `GET /health` returns minimal subset (no latency detail).
- `GET /admin/stats` + `/admin/providers` full when authed.
- Log: connection + status + heap delta (not prompts).

---

## 13. Build & Toolchain

- PlatformIO `platform = espressif32` (>6.7), `framework = espidf`, `board = esp32dev` (WROOM-32 compatible; `esp32dev` maps to 4 MB flash).
- Framework pin: `platform_packages` + `sdkconfig.defaults` to ensure `MBEDTLS_DYNAMIC_BUFFER` etc. set irrespective of menuconfig.
- Partition: start from `default.csv` (or `huge_app` if cert bundle overflows); gate CI size check.
- Logging: default `CONFIG_LOG_DEFAULT_LEVEL_INFO`.
- Compliance: `.editorconfig`, `sdkconfig.defaults` checked in; generated `sdkconfig` ignored.

---

## 14. Failure Modes & Limits

| Failure | Behavior |
|---|---|
| Wi-Fi drop mid-request | Upstream closed, downstream `502`; reconnect job continues |
| Client disconnect mid-stream | Detect `send_chunk` error, tear down upstream |
| Upstream timeout | Retry/fallback as §7, else `504 upstream_timeout` |
| Body too large | `413 payload_too_large` before routing |
| Unknown model | `404 model_not_found` (no fallback) |
| OOM (heap < 20 KB) | Reject with `503 service_unavailable` |
| Second concurrent request during TLS | `429 too_many_requests` |
| Stack overflow | IDF canary + `CONFIG_CHECK_STACK_CANARY` |

Documented limits: **1 concurrent upstream, 8 KB request body**, streaming response unbounded via chunks, LAN plain HTTP.

---

## 15. Evolution Paths (deferred)

- Encrypted NVS + Flash Encryption provisioning script.
- PsychicHttp migration if async WS/SSE overhead justifies.
- LittleFS-backed model routing UI (small web config page) after proxy proven.
- Anthropic/Gemini adapters (Phase after multi-provider).
- Prometheus metrics (if heap allows).


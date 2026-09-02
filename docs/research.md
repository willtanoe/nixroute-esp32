# Research — ESP32-WROOM-32 AI API Router

_Date: 2026-09-02_
_Status: Phase 0 complete_

## 1. Hardware Constraints — ESP32-WROOM-32

### 1.1 Official Spec (Espressif Datasheet v3.7, NRND note)

- SoC: **ESP32-D0WDQ6**, Xtensa dual-core 32-bit LX6 @ 160/240 MHz, 600 DMIPS
- ROM: **448 KB** (boot + core functions)
- SRAM: **520 KB** total on-chip (incl. 8 KB RTC FAST + 8 KB RTC SLOW accessible in deep sleep)
- Flash: **4 MB** external SPI flash (QSPI, memory-mapped, AES-encrypted possible)
- No PSRAM on WROOM-32 (vs. WROVER 8 MB PSRAM). This is the binding constraint.
- Wi-Fi: 802.11 b/g/n up to 150 Mbps, A-MPDU/A-MSDU, WPA2/Enterprise, Station/AP/AP+STA/P2P
- Bluetooth 4.2 BR/EDR+BLE (unused in gateway, but shares radio)
- Peripherals: 34 GPIOs (6 reserved for flash), 18x ADC, UART/SPI/I²C/I²S/PWM
- Power: 5 µA deep sleep; ~160–260 mA active Wi-Fi TX
- Package: 18×25.5×3.1 mm, PCB antenna, -40 to +85 °C
- Status: **NRND (Not Recommended for New Designs)** as of 2025 — WROOM-32E successor. Still widely deployed; firmware is forward-compatible with WROOM-32E.

### 1.2 Usable Heap

- IDF v5.x reports after boot: ~260–280 KB free heap ( Wi-Fi initialized, no TLS).
- Wi-Fi static buffers: ~14–20 KB; dynamic RX/TX configurable.
- After this project's minimal stack (Wi-Fi + httpd + NVS): expect **~180–220 KB free** before any TLS.
- TLS is the dominant consumer (see §4).
- With no PSRAM, **every byte is DRAM** — no external spill. Fragmentation is fatal.
- Flash OTA requires double partition; 4 MB forces careful partitioning: ~1.2 MB app + 1.2 MB OTA + 1.5 KB NVS + SPIFFS optional.

### 1.3 Implications

- Must constrain to **one upstream TLS connection at a time** (serialized proxy).
- Cannot buffer full LLM responses (10–50 KB+); must stream.
- JSON must be incrementally parsed or bounded (request body <= 8 KB typical; clamp).
- Concurrent client handling limited to 2–4 sockets (httpd `max_open_sockets`).

---

## 2. Framework Evaluation — ESP-IDF vs Arduino ESP32

### 2.1 Relationship

Arduino-ESP32 is a **shim on top of ESP-IDF** (Espressif-maintained). Every Arduino build already includes ESP-IDF; Arduino merely exposes a subset.

```
Your code
  ├─ Arduino API (setup/loop, WiFi, HTTPClient, ArduinoJson)  ← abstraction
  └─ ESP-IDF (FreeRTOS, lwIP, mbedTLS, esp_wifi, httpd, NVS)  ← real SDK
```

### 2.2 Comparison

| Dimension | Arduino ESP32 | ESP-IDF (native) | Verdict for gateway |
|---|---|---|---|
| RAM control | Limited; WiFiClientSecure defaults hidden; Arduino String duplicates | Full `menuconfig` control: Wi-Fi buffers, lwIP, mbedTLS, heap caps | **IDF wins** |
| TLS control | Fixed bearSSL/mbedTLS wrapper; cannot set `MBEDTLS_DYNAMIC_BUFFER`, `SSL_VARIABLE_BUFFER_LENGTH` easily | Direct Kconfig: `MBEDTLS_DYNAMIC_BUFFER`, `DYNAMIC_FREE_CA_CERT`, `DYNAMIC_FREE_CONFIG_DATA` saves ~20 KB/conn | **IDF wins** |
| HTTP server | `WebServer` (sync, no chunked), `ESPAsyncWebServer` (community, heavy) | `esp_http_server` (`httpd`): native `httpd_resp_send_chunk`, SSE-capable, FreeRTOS-native | **IDF wins** |
| HTTP client | `HTTPClient` + `WiFiClientSecure` (Blocking; limited streaming) | `esp_http_client` + `esp_tls`: event-driven `HTTP_EVENT_ON_DATA`, zero-copy streaming | **IDF wins** |
| Streaming | Polling, buffering needed | `httpd_resp_send_chunk` + `esp_http_client` event loop, `httpd_socket_send` for true SSE | **IDF wins** |
| NVS / Encrypted NVS | Wrapper available but limited | First-class `nvs_flash`, encrypted NVS | Tie |
| JSON | ArduinoJson (excellent) | cJSON (ships with IDF) or ArduinoJson as component | ArduinoJson usable in both; pick one |
| Wi-Fi reconnect | `WiFi.reconnect()` simplified | Full event loop `WIFI_EVENT`/`IP_EVENT`, auto-reconnect, DHCP/DNS handling | **IDF wins** |
| Debugging | Serial only; limited JTAG | Full GDB/JTAG, `esp_log`, watchdog, heap tracing, `idf.py monitor` | **IDF wins** |
| Maintainability | Large community, but pinned IDF version, slower security patches | Tracks Espressif releases directly, long-term stable | **IDF wins** |
| Ease-of-use | Easier for blink/DIY | Steeper but manageable; 15 lines vs 1 for Wi-Fi | Arduino wins, not relevant here |

### 2.3 Bridge Option

Arduino as ESP-IDF component (`CONFIG_ARDUINO_AS_COMPONENT`) allows gradual migration. Rejected for production gateway: dual ownership adds confusion and heap overhead. If Arduino compat needed later, re-add as component.

### 2.4 Decision

**Use ESP-IDF native, built via PlatformIO (`platform = espressif32`, `framework = espidf`).**

Rationale: this is a long-running network appliance; RAM and TLS tuning dominate. ESP-IDF exposes every knob required. Arduino offers no compensating advantage for a proxy.

Toolchain: **PlatformIO + ESP-IDF** (not standalone `idf.py` to keep IDE/CI parity). PIO version 6.1.19 already installed locally.

SDK target: **ESP-IDF v5.4.x** (current stable at time of writing). Require `esp_http_server`, `esp_http_client`, `esp-tls` (mbedTLS 3.6), `nvs_flash`, `esp_wifi`.

---

## 3. HTTP Server — `esp_http_server` (httpd)

- Component: `esp_http_server` (IDF). Backed by lwIP sockets, FreeRTOS task.
- Handler signature: `esp_err_t handler(httpd_req_t *req)`
- Routing: URI + method registration (`httpd_uri_t`). Supports wildcards and query parsing.
- Reading body: `httpd_req_recv(req, buf, len)` — may require multiple calls; **Chunked Encoding not supported on ingress** (note in docs). So client must send `Content-Length` (all OpenAI SDKs do). Handle 411 if absent.
- Response: `httpd_resp_send(req, buf, len)` (complete) or `httpd_resp_send_chunk(req, buf, len)` (chunked). Must call `send_chunk(req, NULL, 0)` to terminate. After first `send_chunk`, request headers are purged.
- For long-lived SSE streaming beyond handler lifetime: `httpd_socket_send(handle, fd, buf, len, 0)` + `httpd_req_to_sockfd(req)`. Docs and esp-idf http_server/simple SSE example show this pattern. Alternative: keep handler alive and block; simpler for gateway proxy (one request = one handler execution).
- Capabilities needed: `GET /health`, `GET /v1/models`, `POST /v1/chat/completions` (with `stream` true/false), admin endpoints (optional: behind auth).
- Config: `HTTPD_DEFAULT_CONFIG()` → tune `server_port` (80), `max_open_sockets` (4), `max_uri_handlers` (8–10), `stack_size` (6–8 KB), `recv_wait_timeout`/`send_wait_timeout` (10 s), `max_resp_headers`/`max_req_headers` constrained to limit RAM.
- No TLS termination on device (plain HTTP LAN); no cert handling for inbound.

---

## 4. HTTPS Client + TLS Memory

### 4.1 Stack

- `esp_http_client` wraps `esp-tls` which wraps mbedTLS (Espressif fork). Supports `https://` with certificate bundle.
- Modes: `ESP_HTTP_CLIENT_METHOD_POST`, streaming via event handler (`HTTP_EVENT_ON_DATA` delivers chunks as received). This enables **zero-copy forwarding** upstream chunk → downstream chunk without full buffering.
- Authentication: `Authorization: Bearer <key>` header via `esp_http_client_set_header`.
- Timeouts: `timeout_ms` (overall + per-stage). Set connect + response timeouts.

### 4.2 TLS Memory Budget (measured by Espressif in `https_request` example, server cert validation on)

| Config | Heap per TLS session (approx) |
|---|---|
| Default | **42,196 B** (~41 KB) |
| `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` | ~42,120 B |
| `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n` | ~38,533 B |
| `MBEDTLS_DYNAMIC_BUFFER` + `DYNAMIC_FREE_CONFIG_DATA` + `DYNAMIC_FREE_CA_CERT` | **~22,013 B** (~21.5 KB) |

- Savings ~20 KB come from freeing cert/config after handshake and dynamically allocating TX/RX buffers.
- Recommendation: **enable all three dynamic flags** for WROOM-32. They are the single highest-ROI Kconfig.
- Hardware accel: `MBEDTLS_HARDWARE_SHA/AES/MPI/ECC = y` (default on ESP32) offloads to hardware, reducing time + slightly memory.
- Cert bundle: `MBEDTLS_CERTIFICATE_BUNDLE` with default bundle (~100 KB flash). Use `esp_crt_bundle_attach` for `esp_http_client`. Alternatively `use_global_ca_store` + bundle.
- Keep-alive / session tickets: disable for gateway (single short-lived conn per request) to save ~8–12 KB.

### 4.3 Client-Side Risks

- DNS failure, TLS handshake timeout, 429/5xx retries — handled in fallback §9.
- Memory leak risk if custom cert bundle + not freeing `esp_http_client` correctly. Must call `esp_http_client_cleanup()` per request. GitHub issue #3119: leaks after 100+ TLS reqs when custom bundle incorrectly retained — not applicable with IDF bundle attach but worth regression testing (repeated requests test).

---

## 5. JSON — cJSON vs ArduinoJson

- IDF ships **cJSON** (lightweight, zero deps). ArduinoJson is not bundled but can be added as component (header-only, heavier but richer streaming parser).
- Request size: OpenAI chat completions typically 1–8 KB JSON. Worst case (long context) up to ~16–32 KB but WROOM-32 cannot sensibly proxy huge histories; enforce `max_body` ~ 8–16 KB, return 413 if exceeded (document limitation). For larger, consider truncating or buffered streaming parse.
- Strategy: use **cJSON for control plane** (`/v1/models`, health, config) and **intentionally avoid full parse on proxy path**: extract `model` and `stream` via bounded scan (or limited cJSON parse of first ~4 KB). Forward body verbatim upstream — no need to re-serialize entire prompt (avoids duplication). Validate minimal fields, then proxy bytes as-is.
- For future admin config (NVS JSON), cJSON suffices.

---

## 6. NVS & Encrypted NVS

- NVS (`nvs_flash`) is IDF's key-value flash store. Survives reboot, wear-leveled, NOT filesystem. Suitable for: Wi-Fi creds, local token, provider keys, routing map, timeouts.
- Capacity: default partition ~24 KB usable; sufficient for gateway config (<2 KB).
- Encrypted NVS: supported via `CONFIG_NVS_ENCRYPTION` + flash encryption. Requires eFuse or security workflow. Do not enable prematurely (complicates flashing/testing). Start with plain NVS; document migration path. Keys at rest in flash are obfuscation-level without flash encryption — acceptable for LAN threat model; flag in security docs.
- API: `nvs_open`, `nvs_get_str`/`nvs_set_str`, `nvs_commit`. Wrap in `config_manager` module.
- Alternative considered: SPIFFS/LittleFS file — overkill for <2 KB key-value; adds filesystem heap.

---

## 7. SSE / Chunked Transfer — Streaming

### 7.1 Provider Side

- OpenAI-compatible streaming: response is `text/event-stream` with `data: {json}\n\n` frames, ending `data: [DONE]`. Transport is HTTP chunked (or long-lived `Transfer-Encoding: chunked`).
- `esp_http_client` delivers via `HTTP_EVENT_ON_DATA`: each TCP/mbedTLS read yields a chunk (not necessarily aligned to SSE boundary). So gateway must forward bytes as received — **do not reassemble event**.
- Need to handle chunk boundaries that split `data:` line: safe to forward raw bytes; downstream client (OpenAI SDK) handles reassembly.

### 7.2 Gateway -> Client Side

- Two viable patterns in `esp_http_server`:
  1. **Blocking handler + `httpd_resp_send_chunk` per upstream chunk**: handler stays alive for duration of upstream streaming, calling `httpd_resp_send_chunk(req, chunk, len)` inside client event loop (via polling or task). Requires care: httpd handler runs on server task; upstream fetch must be synchronous within handler (no concurrent task touching same `req`). Works; demonstrated in IDF http_server SSE example (`CONFIG_EXAMPLE_ENABLE_SSE_HANDLER`).
  2. **`httpd_socket_send` after hijacking socket**: send headers manually then push raw. Needed only for infinite streams beyond request lifetime. Overkill for bounded LLM streaming (seconds to minute); prefer (1).
- Chosen: **Pattern (1)** — bounded streaming within handler, chunked forwarding, no buffering beyond single chunk buffer (~1–2 KB).
- Headers: `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: keep-alive`, `Access-Control-Allow-Origin: *` (for local dev). For non-streaming: `Content-Type: application/json`.

### 7.3 Backpressure / Disconnect

- Client disconnect: `httpd_resp_send_chunk` returns `ESP_FAIL` / `ESP_ERR_HTTPD_RESP_SEND`; upstream `esp_http_client` must be closed immediately.
- Upstream disconnect: event `HTTP_EVENT_ON_FINISH` / error; send done chunk or error JSON downstream, then terminate.
- Timeout: guard with FreeRTOS timer / `select` timeout. Document 30 s first-byte, 60 s total streaming ceiling (configurable).
- Heap: streaming must avoid String duplication; use static `uint8_t buf[1024–2048]` shared or per-request stack.

### 7.4 Feasibility on WROOM-32

- True streaming is feasible but **not free**: each chunk incurs `send()` → lwIP → Wi-Fi. Tested implicitly by ESP-IDF camera streaming example (MJPEG chunked at ~20 FPS). LLM streaming at token rate (~10–50 tokens/s) is lower bandwidth, well within headroom.
- Risk: watchdog (`CONFIG_ESP_TASK_WDT`) timeout if handler blocks > 5 s without feeding. Must `vTaskDelay` or ensure event loop yields.

---

## 8. Wi-Fi — Reconnection Robustness

- IDF Wi-Fi stack: `esp_wifi` + event loop (`WIFI_EVENT`, `IP_EVENT`). Required handlers:
  - `WIFI_EVENT_STA_START` → `esp_wifi_connect()`
  - `WIFI_EVENT_STA_DISCONNECTED` → exponential backoff reconnect (1s → 2s → 4s → 8s capped at 30s), `esp_wifi_connect()` again; count attempts for stats.
  - `IP_EVENT_STA_GOT_IP` → mark connected, print IP, start httpd.
  - `IP_EVENT_STA_LOST_IP` / disconnect → stop or keep httpd (keep accepting; handlers will error until reconnect).
- DHCP: `IP_EVENT` handles; ensure `CONFIG_LWIP_DHCP_DOES_ARP_CHECK`.
- DNS: via lwIP; failures manifest as `esp_http_client` `ESP_ERR_ESP_TLS_CONNECTION` / `DNS` — trigger fallback.
- Watchdog-safe: event handlers must be short; reconnect attempt offloaded via FreeRTOS timer / task notification, not tight loop.
- Diagnostics: expose `wifi_connected`, `rssi`, `reconnect_count`, `ip` in `/health`.

---

## 9. Memory Fragmentation & Heap Hygiene

- FreeRTOS heap is single bucket; fragmentation after many TLS alloc/free cycles is documented risk (mbedTLS issue 3119).
- Mitigations:
  - Dynamic buffer (see §4) reduces peak but does not eliminate fragmentation.
  - Serialize requests (`xSemaphoreTake` gate) — do not overlap TLS sessions; prevents simultaneous large allocs.
  - Use bounded static buffers where possible; avoid `String` concatenations; prefer `snprintf` into fixed buffers.
  - Avoid dynamic allocation inside streaming loop; allocate `stream_buf` once per request on heap or stack.
  - After each request, log `esp_get_free_heap_size()` + `esp_get_minimum_free_heap_size()` — detect monotonic decline.
  - Document expected heap trace: see `docs/memory-budget.md`.

---

## 10. Watchdog

- IDF has two watchdogs: task WDT (TWDT) and interrupt WDT. TWDT default 5 s.
- Gateway risk: long streaming handler (>5 s) without yielding trips TWDT.
- Mitigation: in streaming loop, `vTaskDelay(1)` or `esp_task_wdt_reset()` periodically. Handler's blocking `esp_http_client_perform` with chunk events yields internally, but verify via stress test.

---

## 11. GitHub Prior Art — Reverse Proxy / API Gateway on ESP32

Search: `ESP32 HTTP proxy`, `ESP32 reverse proxy`, `ESP32 SSE`, `ESP32 API gateway`, `ESP32 OpenAI`.

| Project | Language | Relevance | License | Notes |
|---|---|---|---|---|
| `hoeken/PsychicHTTP` (PsychicHttp) | C++ / IDF | Robust async HTTP(S) server on top of IDF httpd, WebSocket/SSE, PIO compatible | MIT | Mature, event-driven, can replace IDF httpd for nicer API. **Evaluated but deferred**: adds dependency + heap; baseline IDF httpd sufficient for Phase 1. Revisit if async needed. |
| `espressif/esp-idf` examples: `http_server/simple` (SSE), `https_request`, `https_mbedtls` | C / IDF | Canonical reference for patterns used | Apache 2.0 | Used as template for our wifi+httpd+tls scaffolding. |
| Various `ESP32_VS1053_Stream` etc. (CelliesProjects) | C++/Arduino | Chunked HTTP streaming (audio) | MIT | Demonstrates chunked client handling; architecture not reusable (codec-specific). |
| No production ESP32 OpenAI/AI-proxy gateway found | — | **Gap confirms original project**: no off-the-shelf WROOM-32 AI router exists. DIY needed. | — | Do not clone blindly; reference patterns only. |

Action: if a library is adopted later (e.g., PsychicHttp), vendor into `research/` and inspect license/maintenance/heap.

---

## 12. ArduinoJson vs cJSON Decision for This Project

- Start with **cJSON** (ships with IDF, lower overhead). Provide thin helpers: `json_get_string`, `json_extract_model_stream`.
- If payload validation becomes complex, consider ArduinoJson as IDF component (header-only, heavier compile, nicer zero-copy). Keep pluggable.

---

## 13. Open Questions -> Resolved

| Question | Resolution |
|---|---|
| Can WROOM-32 terminate TLS for inbound? | No — inbound is plain HTTP LAN. Only outbound TLS. Saves ~40 KB + CPU. Document as LAN-only gateway; user must place behind VPN if internet-exposed. |
| Max concurrent proxied requests? | 1 (serialized). Queue or 429 second request. Document limit. |
| Max request body? | 8 KB default, configurable to 16 KB. Return 413 beyond. Adequate for chat completions; streaming keeps response unbounded. |
| Config persistence? | NVS plain. Encrypted NVS documented as next step. |
| Auth for inbound? | Optional `LOCAL_API_TOKEN` (Bearer). Disabled if empty; when set, all `/v1/*` require it. Keys not logged, not in `/health`. |
| Watchdog tuning? | Keep default 5 s but yield in streaming loop. |

---

## 14. Risks & Limitations (to be documented in deployment docs)

1. No PSRAM → one-at-a-time upstream; large batch jobs belong on desktop router.
2. Wi-Fi only; no Ethernet fallback.
3. NRND chip → advise WROOM-32E for new PCBs but firmware identical.
4. TLS cert bundle flash cost ~100 KB; fits in 4 MB with tuned partitions.
5. SSE streaming feasible but bounded by heap; unstable clients should retry (HTTP retry).
6. Flash encryption not enabled by default — physical access = read NVS.

---

## 15. Sources (inspected 2026-09-02)

- Espressif ESP32-WROOM-32 Datasheet v3.7 (https://documentation.espressif.com/esp32-wroom-32_datasheet_en.pdf)
- ESP-IDF Programming Guide v6.1 / v5.1 / v5.0.6 (https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html, /esp_http_server.html, /ram-usage.html, /mbedtls.html)
- esp-idf GitHub issues #14444 (dynamic port leak), #4983 / #3119 (heap leak after 100 TLS reqs)
- Random Nerd Tutorials SSE (Arduino), ESP32.com forum thread #37904 (SSE on IDF via `httpd_socket_send`)
- Hubble/Zbotic comparisons: ESP-IDF vs Arduino as shim
- GitHub search of `ESP32 reverse proxy / SSE / API gateway` — see §11

_All memory figures subject to SDK version; re-measure on target build and record in `docs/memory-budget.md`._


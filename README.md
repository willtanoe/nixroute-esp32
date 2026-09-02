# NixRoute ESP32 — ESP32-WROOM-32 AI API Router

> **NixRoute ESP32 — Standalone embedded AI API gateway on ESP32-WROOM-32 (520 KB SRAM, 4 MB flash, no PSRAM).**
> OpenAI-compatible `POST /v1/chat/completions` router — point your OpenAI SDK at `http://<esp32-ip>/v1`. Fork-inspired UI from `D:\nixroute` (NixRoute 0.5.59).

```
curl / Python OpenAI SDK / Claude Code / OpenCode
        │
        │ OpenAI-compatible HTTP (LAN plain :80)
        ▼
   ESP32-WROOM-32  —  esp_http_server + routing + esp_http_client + mbedTLS
        │                serialized TLS (1 upstream)
        ├─► deepseek  (https://api.deepseek.com/v1/chat/completions)
        └─► openrouter (https://openrouter.ai/api/v1/chat/completions)  [fallback]
```

**Project status:** ✅ Builds (pio `esp32dev` 68.6% flash / 11.8% RAM), Wi-Fi reconnect + HTTP gateway + proxy + fallback + SSE streaming implemented. Hardware flash pending (no CP210/CH340 detected on CI host — see `docs/deployment.md`).

---

## Features

- **Wi-Fi STA** with event-driven reconnect (exponential backoff 1s→30s), `esp_timer`, DHCP/DNS via lwIP, RSSI/IP in `/health`
- **HTTP gateway** (`esp_http_server`): `GET /health` (no auth), `GET /v1/models`, `POST /v1/chat/completions`, `GET /admin/{status,providers,stats}`, CORS, 404/OPTIONS
- **OpenAI-compatible** chat completions: forwards original JSON verbatim, extracts `model`+`stream` bounded (8 KB body cap → 413), validates minimal fields
- **Provider abstraction**: `provider_t` registry (DeepSeek first, OpenRouter fallback), `routing_lookup` prefix match (`deepseek-`→deepseek), generic `openai_compatible` adapter
- **Fallback**: retries on transport error / 429 / 500 / 502 / 503 / 504 before first byte streamed; 100 ms×2ⁿ backoff; non-retryable 400/401/404 pass through
- **Streaming**: `stream:true` → `text/event-stream` chunked forwarding via `httpd_resp_send_chunk` per `HTTP_EVENT_ON_DATA` (≈1 KB chunks), watchdog-fed; fallback disabled after first chunk (see §Streaming)
- **Security**: optional `LOCAL_API_TOKEN` Bearer (constant-time), provider keys only in NVS never returned, logs sanitized (`***`), plain-HTTP LAN only (do not port-forward without VPN)
- **Persistence**: `nvs_flash` (`gateway` namespace) for wifi/deps keys/routing override; `config_manager` sanitized logging; encrypted NVS documented as next step
- **Observability**: `/health` + `/admin/*` expose uptime, heap free/min/largest, wifi, disconnects, request/fallback counters, last latency
- **Memory**: mbedTLS dynamic buffers (`DYNAMIC_BUFFER` saves ~20 KB), Wi-Fi buffers trimmed, httpd 4 sockets, `partitions.csv` single-app 1.5 MB (`storage` spiffs 0x280000), cJSON-free lightweight JSON scan

---

## Repository Layout

```
firmware/               # PlatformIO ESP-IDF project (platform = espressif32@7.1.0)
  platformio.ini
  sdkconfig.defaults    # dynamic mbedTLS, trimmed Wi-Fi/HTTPD, 4MB flash
  partitions.csv        # nvs+phy+factory(0x170000)+spiffs(0x280000)
  src/  main.c, wifi_manager.c, http_gateway.c, proxy_handler.c, provider.c, routing.c, auth.c, json_util.c, config_manager.c, stats.c
  include/              # headers per module
  CMakeLists.txt + src/CMakeLists.txt # IDF component glue
config/example/gateway.example.json
tests/scripts/test_health.py, test_openai_compat.py
tools/flash.py
docs/research.md, architecture.md, memory-budget.md, protocol.md, testing.md, deployment.md
.env.example            # placeholders only — never commit real keys
```

---

## Quick Start

```bash
# toolchain
pip install platformio intelhex
pio --version  # 6.1.19

# 1) configure locally (never commit)
cp .env.example .env   # fill WIFI_SSID, WIFI_PASSWORD, *_API_KEY locally

# 2) provision NVS (two options)
# Option A — compile-time default via NVS stub (Phase 10: future POST /admin/config)
# For now, keys are loaded from NVS; if empty, HTTP will return 500 provider not configured.
# Provision via ESP-IDF NVS tool or serial (see docs/deployment.md)

# 3) build
pio run -e esp32dev
# RAM 11.8% 38588 / 327680  Flash 68.6% 1034485 / 1507328 (partitions singleapp)

# 4) flash (when CP210x/CH340 ESP32 attached)
pio device list
pio run -e esp32dev -t upload --upload-port COMx   # or /dev/ttyUSB0
pio device monitor -b 115200

# 5) test from LAN host (replace IP)
curl http://192.168.1.50/health | jq
curl http://192.168.1.50/v1/models | jq
python tests/scripts/test_openai_compat.py --host 192.168.1.50 --model deepseek-chat --token $LOCAL_API_TOKEN
# streaming
python tests/scripts/test_openai_compat.py --host 192.168.1.50 --model deepseek-chat --stream
# OpenAI SDK
OPENAI_BASE_URL=http://192.168.1.50/v1 OPENAI_API_KEY=$LOCAL_TOKEN python -c "from openai import OpenAI; c=OpenAI(); print(c.chat.completions.create(model='deepseek-chat', messages=[{'role':'user','content':'hi'}]).choices[0].message.content)"
```

---

## Configuration

All persisted in NVS namespace `gateway`:

| Key (nvs_get_str) | Env | Example |
|---|---|---|
| `wifi_ssid` | `WIFI_SSID` | `MyWiFi` |
| `wifi_pass` | `WIFI_PASSWORD` | `secret` |
| `local_token` | `LOCAL_API_TOKEN` | `sk-local-...` (empty = open) |
| `ds_key` | `DEEPSEEK_API_KEY` | `sk-...` |
| `or_key` | `OPENROUTER_API_KEY` | `sk-or-...` |
| `routing_json` | — | `[{"prefix":"deepseek-","providers":["deepseek","openrouter"]}]` |

`gateway_config_t` defaults: `http_port 80`, `upstream_timeout_ms 15000` (30s for streaming), `max_body_bytes 8192`.

Provisioning: Phase 10 stub stores via `nvs_set_str` (see `tools/flash.py` + `docs/deployment.md` for NVS CSV import). Future `POST /admin/config` with Bearer auth will allow runtime update.

---

## API

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/health` | none | `{status, uptime_s, wifi_connected, ip, rssi, free_heap, min_heap, requests_total, heap_largest}` |
| `GET` | `/v1/models` | Bearer if `LOCAL_API_TOKEN` set | OpenAI `list` shape, derived from provider registry |
| `POST` | `/v1/chat/completions` | Bearer if set | Proxy; `model`+`stream` extracted bounded; `413` if body > 8KB; `429` if concurrent; fallback before first byte |
| `GET` | `/admin/status` | Bearer | uptime/heap/wifi |
| `GET` | `/admin/providers` | Bearer | registry (no keys) |
| `GET` | `/admin/stats` | Bearer | counters + last latency |
| `OPTIONS` | `/*` | — | CORS preflight `204` |

All responses include `Access-Control-Allow-Origin:*`, `Cache-Control:no-store`. Errors are OpenAI-shaped `{"error":{"message","type","code","provider_status"}}`.

---

## Security Notes

- **LAN-only plain HTTP** — do not port-forward. Use VPN (WG/Tailscale) if remote.
- Provider keys stored only in ESP32 NVS, never echoed in any endpoint, header, or log. Logs use `***`.
- `LOCAL_API_TOKEN` when set → all `/v1/*` + `/admin/*` require `Authorization: Bearer <token>` (constant-time compare). Missing → `401`. `/health` intentionally unauthenticated for monitoring (no secrets).
- Physical flash read exposes NVS without `CONFIG_NVS_ENCRYPTION` + flash encryption (documented next step in `docs/architecture.md` D-06).
- No `DATABASE_URL`, no JS runtime.

---

## Streaming

- `stream:true` → gateway sets `Content-Type: text/event-stream`, forwards each `HTTP_EVENT_ON_DATA` chunk immediately via `httpd_resp_send_chunk` (no per-token `malloc`), feeds watchdog every ~2 KB, terminates with `send_chunk(NULL,0)`.
- After first chunk, fallback is disabled (cannot retract bytes). Pre-first-byte fallback still applies (e.g., DNS failure → second provider).
- Non-stream → fully buffered up to `RESP_MAX 16384`; larger truncated with warning; SSE detected via `data:` prefix sets `text/event-stream`.
- Heap: streaming holds +22 KB (dynamic mbedTLS) for duration, single buffer 1 KB stack/heap; no leak (see `docs/memory-budget.md` gates).

---

## Testing

```bash
# host-side (no hardware)
pio run -e esp32dev                  # gate: must succeed

# hardware-in-loop (ESP32 IP = 192.168.1.50)
python tests/scripts/test_health.py --host 192.168.1.50
python tests/scripts/test_openai_compat.py --host 192.168.1.50 --model deepseek-chat
python tests/scripts/test_openai_compat.py --host 192.168.1.50 --model deepseek-chat --stream
# malformed/unknown/timeout/concurrency/repeated heap checks documented in docs/testing.md
```

Hardware not present on CI — `pio device list` shows only `COM1` (ACPI), no CP210x/CH340. Flash validation **pending**; build + static analysis validated.

---

## Memory & Limitations

- **Measured build:** RAM 11.8% static (38 KB), Flash 68.6% (1.03 MB / 1.507 MB). Runtime heap idle ~200 KB expected; peak during TLS ~180 KB (see `docs/memory-budget.md` for instrumented points).
- **One upstream TLS at a time** (mutex), `429` on concurrent.
- **Body cap 8 KB** → `413`; `resp` cap 16 KB non-stream truncated.
- **No PSRAM** — PSRAM-dependent features (large context, large concurrency) deliberately excluded.
- **NRND** — WROOM-32 NRND (2025); WROOM-32E drop-in compatible.
- **Watchdog:** TWDT 5s; streaming loop yields `vTaskDelay(1)`.

See `docs/research.md` (TLS dynamic buffer saves 20 KB), `docs/architecture.md` (decisions D-01..D-09), `docs/memory-budget.md` (gates).

---

## Decisions (see `docs/architecture.md`)

| ID | Decision | Rationale |
|---|---|---|
| D-01 | ESP-IDF via PIO (not Arduino) | TLS/RAM/streaming control |
| D-02 | Plain HTTP inbound | Saves 40 KB+; LAN threat model |
| D-03 | Serialized upstream | PSRAM-less heap cannot concurrency |
| D-08 | IDF `esp_http_server` over PsychicHttp | Lower heap, native chunked |
| D-09 | Custom `partitions.csv` single-app | Fits 1.03 MB within 1.5 MB ceiling |

---

## Roadmap

- Encrypted NVS + flash encryption provisioning script
- `POST /admin/config` with auth for live reprovisioning
- PsychicHttp evaluation if async WS needed
- Prometheus stub (if heap allows)
- OTA dual 1.2 MB layout after slimming ~150 KB

---

## License

MIT — see `LICENSE`.

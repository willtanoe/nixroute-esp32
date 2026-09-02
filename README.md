# NixRoute — ESP32 API Gateway

> **A standalone OpenAI-compatible AI gateway that runs entirely on an ESP32.**
> Point any OpenAI SDK at `http://<esp32-ip>/v1` and route requests across multiple
> AI providers — with smart failover, round-robin load balancing, and live token
> usage tracking — all managed from a single self-hosted dashboard.

```
curl / Python OpenAI SDK / Claude Code / OpenCode
        │
        │  OpenAI-compatible HTTP (LAN, plain :80)
        ▼
   ESP32-WROOM-32  ──  Arduino WebServer + WiFiClientSecure (mbedTLS)
        │                 serialized TLS (1 upstream at a time)
        └─► N dynamic providers (OpenAI / OpenRouter / Groq / DeepSeek / Ollama / …)
              · smart failover on 429/5xx
              · round-robin across providers serving the same model
              · token usage + per-provider metrics
```

---

## Features

- **OpenAI-compatible** `POST /v1/chat/completions` — forwards JSON, extracts `model`
  and `stream` (8 KB body cap → `413 Payload Too Large`).
- **Multi-provider routing** — up to 16 upstream providers, each with a name, base URL,
  and API key; model lists are auto-fetched and cached in NVS.
- **Smart failover** — if a provider returns `429` (rate limit) or `5xx`/timeout, the
  request cascades to the next candidate before returning an error.
- **Round-robin** — when a model is served by multiple providers, requests rotate across
  them to spread quota and rate limits.
- **Namespaced models** — every model is exposed as `<provider>/<model>` (e.g.
  `openrouter/gpt-4o`), so routing is unambiguous.
- **Streaming** — `stream: true` → `text/event-stream` (SSE) passthrough with true
  chunked forwarding (no full-body buffering).
- **Token usage tracking** — prompt / completion / total tokens, per-model breakdown,
  and a rolling log of recent requests (model, tokens, latency, status).
- **Per-provider metrics** — total requests, success, failures, `429` count, and last
  latency per provider.
- **SPA dashboard** — dark, responsive, zero external dependencies (offline-ready on LAN).
- **Persistence** — all config (providers, keys, Wi-Fi, token, password) in NVS via
  `Preferences`.
- **No hardcoded secrets** — Wi-Fi and keys are configured from the dashboard, never in code.
- **Optional auth** — `LOCAL_API_TOKEN` Bearer (constant-time compare) when set.

---

## Requirements

| Item | Detail |
|---|---|
| Board | DOIT ESP32 DEVKIT V1 (ESP32-WROOM-32, 520 KB SRAM, 4 MB flash) |
| Core | ESP32 Arduino core **3.3.x** |
| Toolchain | Arduino CLI or Arduino IDE 2.x |
| Libraries | `ArduinoJson` (7.x), `Preferences`, `WebServer`, `WiFiClientSecure` (bundled) |

---

## Repository Layout

```
firmware_arduino/esp32_router/
  esp32_router.ino    # firmware: HTTP server, routing, failover, NVS, Wi-Fi
  dashboard_html.h    # dashboard SPA (HTML/CSS/JS, stored in flash via PROGMEM)
tests/scripts/        # host-side HTTP smoke tests (no hardware needed)
```

---

## Build & Flash

```bash
# board: DOIT ESP32 DEVKIT V1 → FQBN esp32:esp32:esp32
arduino-cli compile --fqbn esp32:esp32:esp32 firmware_arduino/esp32_router/esp32_router.ino
arduino-cli upload  --fqbn esp32:esp32:esp32 --port COM11 firmware_arduino/esp32_router/esp32_router.ino
arduino-cli monitor --port COM11 --config baudrate=115200
```

Typical build size (ESP32 core 3.3.11):

```
Sketch uses 1117960 bytes (85%) of program storage space.
Global variables use 53864 bytes (16%) of dynamic memory, leaving 273816 bytes.
```

### First boot / Wi-Fi

SSID/password live in NVS (`wifi_ssid` / `wifi_pass`), set from **Settings → Wi-Fi** —
never hardcoded. On first boot with no Wi-Fi configured, NixRoute starts an access
point **`NixRoute-Setup`** (password `12345678`, IP `192.168.4.1`) so you can reach the
dashboard and set your Wi-Fi.

---

## Dashboard

Open `http://<esp32-ip>/` and sign in (default password `123456`).

| View | Purpose |
|---|---|
| **Overview** | IP, Wi-Fi status, RSSI, uptime, heap, request counters, provider health |
| **Usage** | Token totals, per-model breakdown, recent requests log |
| **Providers** | Add / edit / remove providers, toggle active, sync models |
| **Settings** | Local API token, Wi-Fi, admin password |

### Providers

Add a provider with a **name**, **base URL**, and **API key**. On save the firmware
immediately calls the provider's `GET /models` and caches the result, exposing each model
as `<provider>/<model>`. Leave the key empty to skip fetching (routing then falls back to
other providers). Use the **active** toggle to disable a provider without deleting it.

Auth: `POST /admin/login` sets an `esp_auth=ok` cookie (24 h); `GET /admin/logout` clears it.

---

## API

### Public (OpenAI-compatible)

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/health` | none | device status + per-provider metrics + token totals |
| `GET` | `/v1/models` | Bearer if token set | OpenAI `list` shape, models namespaced as `<provider>/<model>` |
| `POST` | `/v1/chat/completions` | Bearer if token set | proxy with failover + round-robin; `413` if body > 8 KB |

### Admin (JSON, cookie-authenticated)

| Method | Path | Body | Description |
|---|---|---|---|
| `GET` | `/api/state` | — | full state: providers, metrics, usage, token, Wi-Fi, stats |
| `POST` | `/api/providers` | `{name,url,key,active}` | add/update provider (id slugified from name) + auto-fetch models |
| `POST` | `/api/providers/remove` | `{id}` | remove a provider (and its model cache) |
| `POST` | `/api/providers/toggle` | `{id,active}` | enable/disable a provider |
| `POST` | `/api/providers/fetch` | `{id}` | fetch + cache a provider's models |
| `POST` | `/api/token/generate` | — | rotate local token |
| `POST` | `/api/token/clear` | — | clear local token |
| `POST` | `/api/password` | `{password}` | change admin password |
| `POST` | `/api/wifi` | `{ssid,pass}` | set Wi-Fi + reboot |

Responses include `Access-Control-Allow-Origin: *` and `Cache-Control: no-store`. Errors are
OpenAI-shaped: `{"error":{"message":"..."}}`.

### Examples

```bash
curl http://<ip>/health

curl -H "Authorization: Bearer $TOKEN" http://<ip>/v1/models

curl -H "Authorization: Bearer $TOKEN" http://<ip>/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"openrouter/gpt-4o","messages":[{"role":"user","content":"hi"}]}'

# OpenAI SDK
OPENAI_BASE_URL=http://<ip>/v1 OPENAI_API_KEY=$TOKEN python -c "from openai import OpenAI; c=OpenAI(); print(c.chat.completions.create(model='openrouter/gpt-4o', messages=[{'role':'user','content':'hi'}]).choices[0].message.content)"
```

---

## Security Notes

- **LAN-only plain HTTP** — do not port-forward. Use a VPN (WireGuard/Tailscale) for remote access.
- Provider keys live only in ESP32 NVS; never echoed in any endpoint or log (masked `***`).
- `LOCAL_API_TOKEN` when set → all `/v1/*` require `Authorization: Bearer <token>` (constant-time compare).
- Physical flash read exposes NVS without flash encryption (documented as a future step).

---

## Testing

```bash
# host-side smoke tests (replace IP)
python tests/scripts/test_health.py --host 192.168.110.187
python tests/scripts/test_openai_compat.py --host 192.168.110.187 --model openrouter/gpt-4o
python tests/scripts/test_admin_api.py --host 192.168.110.187 --password 123456
```

---

## License

MIT — see [`LICENSE`](LICENSE).

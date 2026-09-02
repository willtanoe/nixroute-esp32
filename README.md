# NixRoute ESP32 — AI API Router (Arduino)

> **NixRoute ESP32 — Standalone OpenAI-compatible AI gateway on ESP32-WROOM-32 (520 KB SRAM, 4 MB flash).**
> Point any OpenAI SDK at `http://<esp32-ip>/v1`. Ships a section-based SPA dashboard with dark mode, client-side routing, and per-provider API-key management.

```
curl / Python OpenAI SDK / Claude Code / OpenCode
        │
        │ OpenAI-compatible HTTP (LAN plain :80)
        ▼
   ESP32-WROOM-32  —  Arduino WebServer + WiFiClientSecure (mbedTLS)
        │                serialized TLS (1 upstream at a time)
        ├─► deepseek   (https://api.deepseek.com/v1/chat/completions)
        ├─► openrouter (https://openrouter.ai/api/v1/chat/completions)
        └─► custom     (any OpenAI-compatible base URL)
```

---

## Features

- **OpenAI-compatible** `POST /v1/chat/completions` — forwards JSON verbatim, extracts `model` + `stream` (8 KB body cap → 413)
- **Provider routing** by model prefix:
  - `deepseek-*` → DeepSeek
  - `openrouter-*` / `claude-*` / `gemini-*` → OpenRouter
  - `custom-*` / `bandel-*` → Custom base URL
  - Fallback order: DeepSeek → OpenRouter → Custom
- **Streaming** `stream:true` → `text/event-stream` (SSE) passthrough
- **SPA dashboard** at `http://<ip>/` — login (default `123456`), rail navigation, dark mode, back/forward + direct-URL routing
- **Persistence** via `Preferences` (NVS namespace `gateway`): provider keys, local token, admin password
- **Optional auth** — `LOCAL_API_TOKEN` Bearer (constant-time compare) when set; empty = open
- **Observability** — `GET /health` (uptime, heap, RSSI, request counters)

---

## Repository Layout

```
firmware_arduino/esp32_router/
  esp32_router.ino    # single-file firmware (Arduino, ESP32 core 3.3.x)
  favicon.svg         # NixRoute mark (served at /favicon.svg)
  nixroute.svg        # alias for the same mark
tests/scripts/        # host-side HTTP smoke tests (no hardware needed)
```

---

## Build & Flash

Toolchain: Arduino CLI (or Arduino IDE 2.x) with ESP32 core **3.3.x**.

```bash
# board: DOIT ESP32 DEVKIT V1 → FQBN esp32:esp32:esp32
arduino-cli compile --fqbn esp32:esp32:esp32 firmware_arduino/esp32_router/esp32_router.ino
arduino-cli upload  --fqbn esp32:esp32:esp32 --port COM11 firmware_arduino/esp32_router/esp32_router.ino
arduino-cli monitor --port COM11 --config baudrate=115200
```

Measured: ~81% flash / ~14% RAM (ESP32 core 3.3.11).

### Wi-Fi

SSID/password are hardcoded at the top of `esp32_router.ino` (`WIFI_SSID`, `WIFI_PASS`). Edit before flashing.

---

## Dashboard

| Page | Route | Purpose |
|---|---|---|
| Routes | `/dashboard/endpoint` | Local endpoint, Wi-Fi status, uptime, request counters |
| Providers | `/dashboard/providers` | API keys (local token, DeepSeek, OpenRouter, Custom) |
| Policies | `/dashboard/policies` | Routing rules + fallback order |
| Observe | `/dashboard/usage` | Usage counters + heap |
| Tools | `/dashboard/tools` | curl / OpenAI SDK snippets |
| Settings | `/dashboard/settings` | Change admin password + About |

Auth: `POST /admin/login` sets `esp_auth=ok` cookie (24 h). `GET /admin/logout` clears it.

---

## API

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/health` | none | `{status, uptime_s, wifi_connected, ip, rssi, free_heap, requests_total, requests_ok, requests_fail, local_token_set}` |
| `GET` | `/v1/models` | Bearer if token set | OpenAI `list` shape (deepseek-chat, deepseek-reasoner, openrouter-auto, custom-model) |
| `POST` | `/v1/chat/completions` | Bearer if token set | Proxy; `model`+`stream` extracted; `413` if body > 8 KB |
| `GET` | `/admin/status` | — | alias for `/health` |
| `GET` | `/admin/login` / `POST` | — | dashboard login |
| `POST` | `/admin/keys` | cookie | save provider keys |
| `POST` | `/admin/token/generate` / `clear` | cookie | rotate local token |
| `POST` | `/admin/password` | cookie | change admin password |
| `OPTIONS` | `/*` | — | CORS preflight `204` |

Responses include `Access-Control-Allow-Origin:*` and `Cache-Control:no-store`. Errors are OpenAI-shaped `{"error":{"message":...}}`.

```bash
curl http://<ip>/health
curl -H "Authorization: Bearer $TOKEN" http://<ip>/v1/models
curl -H "Authorization: Bearer $TOKEN" http://<ip>/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}'

# OpenAI SDK
OPENAI_BASE_URL=http://<ip>/v1 OPENAI_API_KEY=$TOKEN python -c "from openai import OpenAI; c=OpenAI(); print(c.chat.completions.create(model='deepseek-chat', messages=[{'role':'user','content':'hi'}]).choices[0].message.content)"
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
python tests/scripts/test_openai_compat.py --host 192.168.110.187 --model deepseek-chat
```

---

## License

MIT — see `LICENSE`.

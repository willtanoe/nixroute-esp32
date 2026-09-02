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
        └─► N dynamic OpenAI-compatible providers (add/remove/fetch-models)
```

---

## Features

- **OpenAI-compatible** `POST /v1/chat/completions` — forwards JSON verbatim, extracts `model` + `stream` (8 KB body cap → 413)
- **Dynamic provider manager** (like 9router): add / remove / fetch-models / set providers; model → provider routing by prefix → fetched-model match → fallback
- **Streaming** `stream:true` → `text/event-stream` (SSE) passthrough
- **SPA dashboard** at `http://<ip>/` — login (default `123456`), rail navigation, dark mode, back/forward + direct-URL routing
- **Persistence** via `Preferences` (NVS namespace `gateway`): providers, cached models, Wi-Fi, local token, admin password
- **No hardcoded secrets** — Wi-Fi + keys live in NVS, configured from the dashboard; AP fallback (`NixRoute-Setup`) on first boot
- **Optional auth** — `LOCAL_API_TOKEN` Bearer (constant-time compare) when set; empty = open
- **Observability** — `GET /health` (uptime, heap, RSSI, request counters, provider count)

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

Measured: ~83% flash / ~15% RAM (ESP32 core 3.3.11).

### Wi-Fi

SSID/password are stored in NVS (`wifi_ssid` / `wifi_pass`), set from **Settings → Wi-Fi** in the dashboard — never hardcoded. On first boot with no Wi-Fi configured, the device starts an access point `NixRoute-Setup` (password `12345678`, IP `192.168.4.1`) so you can reach the dashboard and set your Wi-Fi.

---

## Dashboard

| Page | Route | Purpose |
|---|---|---|
| Routes | `/dashboard/endpoint` | Local endpoint, Wi-Fi status, uptime, request counters |
| Providers | `/dashboard/providers` | Add / remove / fetch-models / set providers + local token |
| Policies | `/dashboard/policies` | Model → provider routing rules |
| Observe | `/dashboard/usage` | Usage counters + heap |
| Tools | `/dashboard/tools` | curl / OpenAI SDK snippets |
| Settings | `/dashboard/settings` | Wi-Fi, admin password, About |

### Providers

Providers are OpenAI-compatible endpoints stored in NVS as a JSON array. From the dashboard you can **add**, **remove**, **fetch models** (calls each provider's `GET /models`, cached in NVS), and **set/update** (re-submitting an existing `id` updates it).

Model → provider routing (like 9router):
1. prefix match — `model` starting with `<id>-` or `<id>/`
2. exact match against the provider's fetched model list
3. fallback — first provider that has an API key

Auth: `POST /admin/login` sets `esp_auth=ok` cookie (24 h). `GET /admin/logout` clears it.

---

## API

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/health` | none | `{status, uptime_s, wifi_connected, ip, rssi, free_heap, requests_total, requests_ok, requests_fail, local_token_set, providers}` |
| `GET` | `/v1/models` | Bearer if token set | OpenAI `list` shape, aggregated from each provider's fetched models |
| `POST` | `/v1/chat/completions` | Bearer if token set | Proxy; `model`+`stream` extracted; `413` if body > 8 KB |
| `GET` | `/admin/status` | — | alias for `/health` |
| `GET` | `/admin/login` / `POST` | — | dashboard login |
| `POST` | `/admin/providers/add` | cookie | add or update (set) a provider |
| `POST` | `/admin/providers/remove` | cookie | remove a provider (and its model cache) |
| `POST` | `/admin/providers/fetch` | cookie | fetch + cache a provider's models |
| `POST` | `/admin/token/generate` / `clear` | cookie | rotate local token |
| `POST` | `/admin/password` | cookie | change admin password |
| `POST` | `/admin/wifi` | cookie | set Wi-Fi SSID/password + reboot |
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

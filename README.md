# ESP32 Router — AI API Gateway (Arduino)

> **Standalone OpenAI-compatible gateway on ESP32-WROOM-32 (520 KB SRAM, 4 MB flash).**
> Point any OpenAI SDK at `http://<esp32-ip>/v1`. Manage multiple AI providers from a
> single dashboard — each provider has a name, base URL, and API key, and the firmware
> fetches its model list automatically.

```
curl / Python OpenAI SDK / Claude Code / OpenCode
        │
        │ OpenAI-compatible HTTP (LAN plain :80)
        ▼
   ESP32-WROOM-32  —  Arduino WebServer + WiFiClientSecure (mbedTLS)
        │                serialized TLS (1 upstream at a time)
        └─► N dynamic OpenAI-compatible providers (add / auto-fetch / remove)
```

---

## Features

- **OpenAI-compatible** `POST /v1/chat/completions` — forwards JSON, extracts `model` + `stream` (8 KB body cap → 413)
- **Dynamic providers** — add a provider by **name + base URL + API key**; the firmware
  auto-fetches its `GET /models` and caches the list in NVS
- **Namespaced models** — every model is exposed as `<provider>/<model>` (e.g. `baroq/gpt-4o`),
  so routing is unambiguous and the model list clearly shows which provider owns each model
- **Model routing** — (1) `provider/model` prefix, (2) exact match against fetched models,
  (3) `provider-*` prefix, (4) fallback to the first provider with a key
- **Streaming** `stream:true` → `text/event-stream` (SSE) passthrough
- **SPA dashboard** at `http://<ip>/` — login (default `123456`), fetch-based UI (no page
  reloads), dark mode, JSON admin API
- **Persistence** via `Preferences` (NVS namespace `gateway`): providers, cached models,
  Wi-Fi, local token, admin password
- **No hardcoded secrets** — Wi-Fi + keys live in NVS, configured from the dashboard;
  AP fallback (`ESP32Router-Setup`) on first boot
- **Optional auth** — `LOCAL_API_TOKEN` Bearer (constant-time compare) when set; empty = open
- **Observability** — `GET /health` (uptime, heap, RSSI, request counters, provider count)

---

## Repository Layout

```
firmware_arduino/esp32_router/
  esp32_router.ino    # single-file firmware (Arduino, ESP32 core 3.3.x)
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

### Wi-Fi

SSID/password are stored in NVS (`wifi_ssid` / `wifi_pass`), set from **Settings → Wi-Fi** —
never hardcoded. On first boot with no Wi-Fi configured, the device starts an access point
`ESP32Router-Setup` (password `12345678`, IP `192.168.4.1`) so you can reach the dashboard
and set your Wi-Fi.

---

## Dashboard

| View | Purpose |
|---|---|
| Overview | IP, Wi-Fi status, uptime, heap, request counters, endpoint URL |
| Providers | Add (name + base URL + API key), auto-fetch, edit, remove |
| Models | Aggregated `<provider>/<model>` list across all providers |
| Settings | Local API key, Wi-Fi, admin password |

### Providers

Add a provider with a **name**, **base URL**, and **API key**. On save the firmware
immediately calls the provider's `GET /models` and caches the result. Each model is then
listed as `<provider>/<model>` — e.g. providers `baroq` and `grip` produce `baroq/model-a`
and `grip/model-b`. If the API key is left empty, fetching is skipped (and routing falls
back to other providers).

Auth: `POST /admin/login` sets `esp_auth=ok` cookie (24 h). `GET /admin/logout` clears it.

---

## API

### Public (OpenAI-compatible)

| Method | Path | Auth | Description |
|---|---|---|---|
| `GET` | `/health` | none | `{status, uptime_s, wifi_connected, ip, rssi, free_heap, requests_*, local_token_set, providers}` |
| `GET` | `/v1/models` | Bearer if token set | OpenAI `list` shape, models namespaced as `<provider>/<model>` |
| `POST` | `/v1/chat/completions` | Bearer if token set | Proxy; `model`+`stream` extracted; `413` if body > 8 KB |

### Admin (JSON, cookie-authenticated)

| Method | Path | Body | Description |
|---|---|---|---|
| `GET` | `/api/state` | — | full dashboard state (providers + models + token + wifi + stats) |
| `POST` | `/api/providers` | `{name,url,key}` | add/update provider (id slugified from name) + auto-fetch models |
| `POST` | `/api/providers/fetch` | `{id}` | fetch + cache a provider's models |
| `POST` | `/api/providers/remove` | `{id}` | remove a provider (and its model cache) |
| `POST` | `/api/token/generate` | — | rotate local token |
| `POST` | `/api/token/clear` | — | clear local token |
| `POST` | `/api/password` | `{password}` | change admin password |
| `POST` | `/api/wifi` | `{ssid,pass}` | set Wi-Fi + reboot |

Responses include `Access-Control-Allow-Origin:*` and `Cache-Control:no-store`. Errors are
OpenAI-shaped `{"error":{"message":...}}`.

```bash
curl http://<ip>/health
curl -H "Authorization: Bearer $TOKEN" http://<ip>/v1/models
curl -H "Authorization: Bearer $TOKEN" http://<ip>/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"baroq/gpt-4o","messages":[{"role":"user","content":"hi"}]}'

# OpenAI SDK
OPENAI_BASE_URL=http://<ip>/v1 OPENAI_API_KEY=$TOKEN python -c "from openai import OpenAI; c=OpenAI(); print(c.chat.completions.create(model='baroq/gpt-4o', messages=[{'role':'user','content':'hi'}]).choices[0].message.content)"
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
python tests/scripts/test_openai_compat.py --host 192.168.110.187 --model baroq/gpt-4o
python tests/scripts/test_admin_api.py --host 192.168.110.187 --password 123456
```

---

## License

MIT — see `LICENSE`.

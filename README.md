# ESP32-WROOM-32 AI API Router

Standalone embedded AI API gateway on **ESP32-WROOM-32** — OpenAI-compatible `POST /v1/chat/completions` router that forwards to upstream LLM providers.

```
curl / OpenAI SDK  ──►  ESP32 (http://esp32.local/v1)  ──►  DeepSeek / OpenRouter / ...
```

> **Status:** Phase 0 complete — research, architecture, and memory budget documented. Firmware scaffold in progress. No hardware flash yet (COM port pending).

## Quick Start (Phase 1+)

```bash
cp .env.example .env   # fill locally, never commit
pio run -e esp32dev        # build (firmware/)
pio run -e esp32dev -t upload --upload-port /dev/ttyUSB0
pio device monitor -b 115200
curl http://<esp32-ip>/health
```

## Docs

- `docs/research.md` — hardware constraints, TLS memory, streaming, prior art
- `docs/architecture.md` — decisions, routing, fallback, security
- `docs/memory-budget.md` — heap/flash planning (pre-measurement)
- `docs/deployment.md` — flashing & config (pending Phase 12)
- `docs/protocol.md` — API spec (pending)

## Hardware

- Target: **ESP32-WROOM-32** (520 KB SRAM, 4 MB flash, no PSRAM) — NRND but firmware compatible with WROOM-32E
- Framework: **ESP-IDF v5.4** via **PlatformIO**
- Inbound: plain HTTP LAN (`:80`); outbound: HTTPS (`:443`, mbedTLS dynamic buffers)

## Security Notes

- Do not port-forward without VPN — inbound is plain HTTP.
- Provider keys live only in ESP32 NVS; never returned in API/logs.
- Set `LOCAL_API_TOKEN` if you need Bearer auth on your LAN.

## License

MIT (see `LICENSE`)


# Testing Plan

## Host-side (no hardware)

- `pio run` must succeed (CI gate)
- python lint for tools/tests

## Hardware-in-loop (requires ESP32 on /dev/ttyUSB0 or COMx)

1. Wi-Fi connect → /health
2. GET /v1/models
3. POST /v1/chat/completions (non-stream)
4. malformed / unknown model → 4xx
5. timeout / fallback
6. streaming SSE
7. 100x repeated + heap check (no leak)
8. client disconnect, upstream disconnect

Scripts in `tests/scripts/`; run `python tests/scripts/test_health.py --host <ip>`

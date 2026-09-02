# Deployment

## Prerequisites

- Python 3.10+, `pip install platformio intelhex`
- `pio --version` ≥ 6.1
- ESP32-WROOM-32 dev board (CP210x/CH340/CH9102 USB bridge), 4 MB flash, **no PSRAM required**

Check tooling:
```
pio --version
python -m pip show intelhex
```

## Build

```bash
cp .env.example .env   # fill locally — never commit .env
# .env is not used directly by firmware; values must be provisioned to NVS
pio run -e esp32dev
# expected: RAM 11.8% 38588, Flash 68.6% 1034485/1507328 — must succeed
pio run -e esp32dev -t size
```

Partition: `firmware/partitions.csv` (single-app 0x170000 + spiffs 0x280000). 4 MB flash, 4 MB `board_build.fuses.flash_freq 40m`, `dio`. If you need OTA, switch to `partitions_two_ota.csv` and slim ~150 KB (see memory-budget).

SDK config: `firmware/sdkconfig.defaults` (dynamic mbedTLS, trimmed Wi-Fi/HTTPD, CMN bundle). Do not edit generated `firmware/sdkconfig*`.

## Provisioning (NVS)

Provider keys + Wi-Fi are persisted in NVS namespace `gateway`. Two paths:

### Path A — Compile-time stub (current)

Firmware boots with empty NVS → `wifi_manager` logs `SSID empty — skipping Wi-Fi` and HTTP still serves `/health` (no Wi-Fi). Provision via serial NVS tool (future `POST /admin/config`):

```bash
# Example via ESP-IDF nvs_partition_gen.py (optional)
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate config.csv nvs.bin 0x6000
esptool.py --chip esp32 --port COMx write_flash 0x9000 nvs.bin
# config.csv form:
# key,type,encoding,value
# gateway,namespace,,
# wifi_ssid,data,string,MySSID
# wifi_pass,data,string,MyPASS
# ds_key,data,string,sk-...
# or_key,data,string,sk-or-...
# local_token,data,string,sk-local-...
```

### Path B — Runtime admin (planned Phase 10)

`POST /admin/config` with `Authorization: Bearer <LOCAL_TOKEN>` → `nvs_set_str` + `nvs_commit`. Not yet in this milestone; document as next.

Sanitized logging: `config_manager_log_sanitized` prints `***` for keys. No `Authorization` header is logged.

## Flash

```bash
pio device list
# Expect something like:
# /dev/ttyUSB0 — CP2102
# COM5 — CH340
# Only COM1 (ACPI) means no board attached — flash pending.

pio run -e esp32dev -t upload --upload-port /dev/ttyUSB0
# or Windows: --upload-port COM5
pio device monitor -b 115200
# Look for:
# I (xxx) gateway: heap free=... min=...
# I (xxx) wifi: initializing Wi-Fi SSID='...'
# I (xxx) http: Starting HTTP server on port 80
# I (xxx) http: heap after httpd: free=...
```

If `SSID empty`, HTTP still serves `/health` locally via AP? Currently AP not enabled — connect board via USB serial to provision or set defaults in code.

## Verify

From a LAN host on same Wi-Fi:

```bash
ESP_IP=192.168.1.50   # from monitor log "GOT IP"
curl http://$ESP_IP/health | jq
# {"status":"ok","uptime_s":...,"wifi_connected":true,...}

curl http://$ESP_IP/v1/models | jq

# With LOCAL_TOKEN set:
curl -H "Authorization: Bearer $LOCAL_TOKEN" http://$ESP_IP/admin/status | jq
curl -H "Authorization: Bearer $LOCAL_TOKEN" http://$ESP_IP/admin/providers | jq

# Proxy (requires provider key provisioned)
curl -H "Authorization: Bearer $LOCAL_TOKEN" -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}' \
  http://$ESP_IP/v1/chat/completions | jq

# Streaming
curl -N -H "Authorization: Bearer $LOCAL_TOKEN" -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}],"stream":true}' \
  http://$ESP_IP/v1/chat/completions
# expect: data: {"choices":[...]}
```

OpenAI SDK:
```
OPENAI_BASE_URL=http://$ESP_IP/v1 OPENAI_API_KEY=$LOCAL_TOKEN \
python tests/scripts/test_openai_compat.py --host $ESP_IP --model deepseek-chat
```

## Security & Operations

- Do **not** port-forward port 80. LAN-only. For remote, use WireGuard/Tailscale to reach LAN.
- Rotate `LOCAL_API_TOKEN` via NVS, restart.
- Monitor `free_heap`/`min_heap`/`heap_largest` via `/health` — `min` monotonic decline >2% over 100 requests signals leak (see memory-budget gates).
- Watchdog 5 s: streaming loop yields; if TWDT trips, check for blocking handler without `vTaskDelay`.
- Fallback count in `/admin/stats` — >5% indicates upstream flakiness.

## CI

GitHub remote = `git@github.com:willtanoe/esp32-router.git` (ssh). CI gate: `pio run` must succeed; push on each milestone (already 5 pushes). No secrets in history (verify `git log -p | grep -i api_key` empty).

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `SSID empty — skipping Wi-Fi` | NVS empty | Provision via NVS CSV path above |
| `429 too many concurrent` | Serialized upstream | Design: one TLS at a time; retry after 1 s |
| `413 payload too large` | Body > 8192 | Reduce `messages` length or raise `max_body_bytes` in NVS/Kconfig |
| `502 Bad Gateway` | Upstream DNS/TLS/5xx + no fallback | Check provider key, `admin/providers`, latency in `/admin/stats` |
| `provider API key not configured` | NVS missing `ds_key`/`or_key` | Provision key per table |
| Flash 98% with default partition | Need singleapp large | Already using `partitions.csv` 1.5 MB; verify `pio run` Flash % ~68% |
| Partition overflow 4.0MB | `partitions.csv` sum > flash | Keep factory 0x170000 + storage 0x280000 (fits 0x400000 - 0x9000) |
| Only COM1 on `pio device list` | No USB bridge | Attach ESP32 dev board; install CP210x/CH340 driver |

Hardware validation pending on CI host (no USB bridge). Build & static analysis validated; flight test requires board.

# Testing

## Goals

Validate an embedded network appliance correctness, memory stability, and failure handling on a 520 KB SRAM device with one serialized TLS.

## Host-Side (No Hardware) — CI Gate

```bash
pio run -e esp32dev               # must SUCCESS — RAM 11.8%, Flash 68.6%
pio run -e esp32dev -t size
# python -m py_compile tests/scripts/*.py tools/*.py
```

No secrets in history:
```
git log --all --patch | grep -i "sk-"
# expect no hits (only .env.example placeholders)
```

## Hardware-In-Loop (Requires ESP32 on /dev/ttyUSB0 or COMx, Wi-Fi, NVS keys)

Set `LOCAL_API_TOKEN` if enabled. Replace `ESP_IP` with `wifi_manager` log `GOT IP`.

### 1. Wi-Fi

```
# monitor
pio device monitor -b 115200 | grep wifi
# expect: WIFI_EVENT_STA_START, GOT IP, heap after CONNECT
# disconnect Wi-Fi AP → expect DISCONNECTED reason, reconnect timer 1s→30s, reconnect_count++
```

### 2. Health
```
python tests/scripts/test_health.py --host $ESP_IP
curl http://$ESP_IP/health | jq
# expect status ok, wifi_connected true, heap free > 180k implied via free_heap, min_heap monotonic?
```

### 3. Models
```
curl -H "Authorization: Bearer $TOKEN" http://$ESP_IP/v1/models | jq
curl http://$ESP_IP/v1/models | jq   # without token should 401 if LOCAL_TOKEN set
```

### 4. Simple completion (non-stream)
```
python tests/scripts/test_openai_compat.py --host $ESP_IP --model deepseek-chat
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"ping"}]}' \
  http://$ESP_IP/v1/chat/completions | jq
# expect 200, choices[0].message.content, no key in response
```

### 5. Malformed
```
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"model":""}' http://$ESP_IP/v1/chat/completions -i   # empty body handling
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"messages":[]}' http://$ESP_IP/v1/chat/completions -i # routing fallback to deepseek default
```

### 6. Unknown model
```
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"model":"unknown-xyz","messages":[{"role":"user","content":"hi"}]}' \
  http://$ESP_IP/v1/chat/completions -i   # routing → default provider, may 404 from upstream
```

### 7. Timeout
Configure `upstream_timeout_ms` small (e.g., 1000) via NVS then request:
```
# provider timeout → 504 or fallback depending on retryable
```

### 8. Rate-limit simulation
Set invalid key to trigger provider 429 mock, or set second provider via routing to verify fallback count increments.

### 9. Provider fallback (controlled)
```
# 1) set ds_key invalid, or blank, with routing deepseek->openrouter
# 2) request deepseek-chat
# expect: log "provider ds_key missing" or upstream 401 non-retryable? 401 is non-retryable, so no fallback. Use 429/500 trigger.
# For transport/DNS fail fallback: use unreachable provider URL patched via build, verify second provider success.
```

### 10. Streaming
```
curl -N -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"count to 5"}],"stream":true}' \
  http://$ESP_IP/v1/chat/completions
# expect Content-Type text/event-stream, data: ... lines, chunked framing, no buffering of whole

python tests/scripts/test_openai_compat.py --host $ESP_IP --model deepseek-chat --stream
# OpenAI SDK streaming:
OPENAI_BASE_URL=http://$ESP_IP/v1 OPENAI_API_KEY=$TOKEN python - <<'PY'
from openai import OpenAI
c=OpenAI(base_url="http://"+__import__("os").environ["ESP_IP"]+"/v1", api_key=__import__("os").environ["TOKEN"])
for ch in c.chat.completions.create(model="deepseek-chat", messages=[{"role":"user","content":"hi"}], stream=True):
    print(ch)
PY
```

### 11. Client disconnect
Start streaming then Ctrl-C curl mid-stream → gateway should `send_chunk failed`, abort `esp_http_client`, no leak (heap returns).

### 12. Upstream disconnect
Mock by killing provider mid-stream (or using timeout 1s) → gateway terminates chunked with `NULL,0`, logs.

### 13. Large request
```
python -c "print('{\"model\":\"deepseek-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"' + 'a'*9000 + '\"}]}')" | curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d @- http://$ESP_IP/v1/chat/completions -i
# expect 413 payload too large
```

### 14. Repeated / stability
```
for i in $(seq 1 100); do curl -s -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}' http://$ESP_IP/v1/chat/completions | head -1; echo $i; done
# after: curl http://$ESP_IP/health | jq .min_heap
# gate: min_heap decline <2%, largest_block not collapsed
python - <<'PY'
import subprocess, json, urllib.request
# fetch /admin/stats before/after 100x and assert requests_total==100, no OOM
PY
```

### 15. Concurrent
```
# two parallel requests—only one should succeed, second 429
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"long..."}],"stream":true}' http://$ESP_IP/v1/chat/completions &
curl -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}' http://$ESP_IP/v1/chat/completions -i &
wait
```

### 16. Wi-Fi reconnect long-run
```
# run for >1h with periodic health polling, optionally AP power cycle
watch -n 10 curl -s http://$ESP_IP/health | jq
# after reconnect, /health ip correct, disconnects counter increments, proxy still works
```

### 17. Auth negative
```
curl http://$ESP_IP/v1/chat/completions -i   # missing token when enabled → 401, no body leak
curl -H "Authorization: Bearer wrong" http://$ESP_IP/v1/chat/completions -i  # 401
curl http://$ESP_IP/health -i   # always 200 even with token set (no secrets)
```

### 18. Memory / watchdog
```
# enable heap logging heartbeat every 10s in monitor
# run streaming 50 token * 10 min → verify no TWDT, heap heartbeat stable
```

## Pass Criteria

- Host `pio run` SUCCESS.
- Hardware gate: `/health` 200, `/v1/models` 200, `chat/completions` 200 (with valid key), streaming SSE valid, fallback count increments when forced, 401 on bad auth, 413 on large, 429 on concurrent, no key in any response, min_heap stable after 100x.

## Pending

Hardware not attached to CI (only `COM1`). `pio device list` shows no CP210x/CH340. All hardware-in-loop tests are **pending** but scripts are ready (`tests/scripts/*`). Build + config + code review validated.

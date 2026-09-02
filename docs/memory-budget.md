# Memory Budget — ESP32-WROOM-32 AI API Router

_Date: 2026-09-02 (updated 2026-09-02 Phase 9 build)_
_Status: Phase 9 — build verified, runtime heap pending HW (see §10)_
_Target: ESP32-WROOM-32 (520 KB SRAM, no PSRAM, 4 MB flash)_

---

## 1. Ground Truth — What We Know

| Source | Figure | Notes |
|---|---|---|
| IDF datasheet | 520 KB SRAM total | Includes 8 KB RTC FAST/SLOW; usable DRAM ~320 KB after ROM/IRAM static |
| Boot + IDF baseline | ~260–280 KB free pre-Wi-Fi | `esp_get_free_heap_size()` after `app_main` entry, no Wi-Fi/TLS |
| Wi-Fi initialized | **~210–250 KB free** | Static + dynamic buffers; depends on `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM`, `DYNAMIC_RX/TX`, `LWIP` tuning |
| After `httpd_start` | ~200–230 KB | httpd task stack + socket buffers |
| TLS per session (from IDF docs, `https_request`, with bundle) | **22 KB (dynamic) vs 42 KB (default)** | Savings via `MBEDTLS_DYNAMIC_BUFFER` + `FREE_CA_CERT` + `FREE_CONFIG_DATA` |
| `esp_http_client` handle | ~2–4 KB + buffers | `buffer_size=1024` TX/RX |

> All figures must be re-measured on actual WROOM-32; this document will be updated after Phase 1 build flash.

---

## 2. Kconfig Choices (memory-relevant)

Derived from `docs/research.md` §4.2.

```
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y          # modest
# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n          # ~4 KB saving; disable after debugging
CONFIG_MBEDTLS_HARDWARE_AES=y
CONFIG_MBEDTLS_HARDWARE_SHA=y
CONFIG_MBEDTLS_HARDWARE_MPI=y
CONFIG_MBEDTLS_HARDWARE_ECC=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
# CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y    # ~100 KB flash
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6               # default 10 → save ~6 KB
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=6              # default 32 → cap during proxy
CONFIG_ESP_WIFI_TX_BUFFER_TYPE=1                     # dynamic
CONFIG_LWIP_MAX_SOCKETS=6                            # default 10 → save per-socket
CONFIG_LWIP_SO_REUSE=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=512
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_HTTPD_MAX_OPEN_SOCKETS=4                      # default 7 → save task slots
```

Each reduction saves DRAM at cost of concurrency — appropriate since gateway serializes.

---

## 3. Proposed Heap Map (steady-state, Wi-Fi connected, idle)

Assuming `sdkconfig.defaults` above, one-shot measurement plan after Phase 1 boot:

| Region / Allocation | Estimated (bytes) | Location | Notes |
|---|---|---|---|
| IDF baseline free | ~230,000 | DRAM | After `wifi_manager_init` + `httpd_start`, no TLS |
| httpd task stack (one task) | 6,144 | DRAM | `CONFIG_HTTPD_CTRL_STACK_SIZE` |
| httpd per-socket (×4 max) | ~2,000 × 4 = 8,000 | DRAM | lwIP + httpd context |
| NVS + config cache | ~2,000 | DRAM+flash | Small key-value cache |
| Stats / routing table | ~1,000 | DRAM | |
| **Free at idle (target)** | **~205,000–215,000** | | Must re-measure |

If below 180 KB at idle, investigate config bloat (cert bundle unexpectedly RAM-resident, Wi-Fi buffers too high).

---

## 4. Per-Request Heap Walk

### 4.1 Non-streaming `POST /v1/chat/completions` (~2 KB request, ~2 KB response)

| Phase | Stack / Heap Δ | Notes |
|---|---|---|
| On `httpd_req_recv` | +8,192 body buffer (clamped) | Allocated once per request on heap or handler stack |
| cJSON parse `model`+`stream` | +~1,000 temp | Bounded parse of first ~4 KB |
| `esp_http_client_init` | +~3,000 handle | |
| TLS handshake (`esp-tls`) | **+~22,000** (dynamic) | Peak; freed after close |
| Upstream tx (headers+body) | +~1,024 | |
| Response accumulation (non-stream) | +~2,048–16,384 | Capped 16 KB; if > cap, fallback to streaming path |
| `esp_http_client_cleanup` | **−~22,000** | Verify heap returns |
| `httpd_resp_send` + free buffers | −~12,000 | |
| **Net leak expected** | **~0 (within 100 bytes)** | Monotonic decline = bug |

**Estimated peak during TLS (with body 8 KB):** idle ~210 KB → ~175 KB free (usable with margin). Still leaves >40 KB headroom above OOM guard (20 KB).

**Without dynamic buffers:** peak → ~155 KB free (acceptable but tighter; fragmentation worse). Hence DYN enabled.

### 4.2 Streaming (`stream:true`, ~5 s, ~50 chunks)

Same as above plus:

| Phase | Δ |
|---|---|
| `stream_buf[1024]` (stack or single alloc) | 1,024 |
| Per-chunk: zero extra (forward via `send_chunk`) | 0 |
| Duration heap stays elevated (+22 KB) for stream length | — |
| After close: full reclaim | −22 KB |

Heap must not allocate per-token; any `malloc` in loop is defect.

### 4.3 Worst case counted

- One TLS session only (serialized) — peak +22 KB.
- Two concurrent clients would double to +44 KB + second body 8 KB → free would drop to ~150 KB; feasible but **not allowed** per D-03. Second request returns `429` before TLS.
- Large body (16 KB max) → +8 KB extra at peak; still inside 20 KB guard.

---

## 5. Flash Budget (4 MB) — Measured Phase 9

Current `firmware/partitions.csv` (single-app large, no OTA) — chosen because proxy + cert bundle + http client exceeded 1 MB default:

| Partition | Offset | Size | Contents |
|---|---|---|---|
| nvs | 0x9000 | 0x6000 (24 KB) | Config + Wi-Fi cal |
| phy_init | — | 0x1000 (4 KB) | |
| factory app | — | 0x170000 (1474560 B / 1.44 MB) | Firmware + cert bundle |
| storage (spiffs) | — | 0x280000 (2621440 B) | Remainder (2.5 MB) |

Build (2026-09-02, `pio run -e esp32dev`, `idf 5.x`, `partitions.csv`):
- `RAM 11.8% 38588/327680`, `Flash 68.6% 1034485/1507328`, text+data 823k+225k, bss 20593.
- With default OTA 1 MB ceiling we hit 98.5% (1033065/1048576) — overflow imminent. Single-app large moves ceiling to 1507328, leaving 31% headroom for future +300 KB.
- Cert bundle `CMN` vs `FULL` did not significantly change Flash after clean (still ~1.03 MB) — dominant cost is `esp_http_client` + `mbedtls` (tf-psa-crypto) not bundle alone.
- If OTA required later, either slim ~150 KB (disable `CONFIG_MBEDTLS_HARDWARE_ECC`, reduce log, or switch to minimal bundle) or use `partitions_two_ota` with 2×0xE0000 (~900 KB each) which is too small — so OTA at 1.2 MB needs further slimming documented in `docs/architecture.md` D-09.

---

## 6. How to Measure (to be executed in Phase 1–3)

In `app_main` and handler edges:

```c
ESP_LOGI(TAG, "heap free=%d min=%d largest_block=%d",
    esp_get_free_heap_size(),
    esp_get_minimum_free_heap_size(),
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
```

Instrument points:

```
[boot] before wifi
[boot] after wifi + httpd
[per-request] on entry (httpd handler)
[per-request] after body recv
[per-request] after esp_http_client_init
[per-request] after TLS handshake (client connected callback)
[per-request] after first upstream chunk
[per-request] after upstream close
[per-request] after httpd_resp_send / last chunk
[per-request] 100 ms after cleanup (let IDLE task reclaim)
```

Plot over repeated runs (`tests/scripts/repeated.sh` × 100) to catch leak/fragmentation.

Also measure `heap_caps_get_minimum_free_heap_size()` global — never below 20,000 should remain.

### From IDF report (preliminary)

If an https_request example measures ~22 KB with dynamic buffers, our gateway peak should be predictable.

---

## 7. Budget Gates

Before declaring any phase complete, verify:

- [ ] Idle free ≥ 180 KB
- [ ] Peak during TLS ≥ 30 KB remaining (i.e., never below 20 KB)
- [ ] After 100 sequential requests, `min_free_heap` decline < 2% (no leak)
- [ ] Streaming run (50 token × 10 min) shows no OOM
- [ ] Binary fits in ~1,285 KB OTA partition (or else re-partition doc'd)

Failure to meet any gate → re-tune Kconfig (§2) or reduce max body.

---

## 8. Known Unknowns (runtime pending HW)

- Runtime idle free on our `sdkconfig.defaults` (measure at `[boot] before wifi`, `[boot] after wifi + httpd`, per-request phases). Build static RAM 11.8% corresponds to .bss+data, not heap; heap derived vs 320 KB DRAM after static.
- Fragmentation after 100+ TLS cold-starts with dynamic buffers (IDF issue #14444 fixed in v5.2; verify our v5.4 not regressed).
- lwIP RX leak if client aborts mid-stream (must test via `curl -N ...` Ctrl-C while streaming).
- Largest alloc block decay (`heap_caps_get_largest_free_block`).

Until flashed on WROOM-32 (only COM1 on CI), figures remain planning + static build. Runtime gate instrumentation added in `wifi_manager` + `proxy_handler` logs.

## 10. Measured Build (Phase 9) — Static

- `sdkconfig.defaults` as of Phase 9 includes `MBEDTLS_DYNAMIC_BUFFER=y`, `CMN` bundle, `HTTPD_MAX_OPEN_SOCKETS=4`, Wi-Fi 6/6/16.
- `pio run` (2026-09-02): `Linking firmware.elf -> RAM 38588, Flash 1034485/1507328 (68.6%)`, `text 823k + data 225k + bss 20593`.
- Trend: Phase 1 (no Wi-Fi/http) 16.2% flash 169 KB → Phase 3 (http+Wi-Fi) 81.3% 852 KB → Phase 9 (proxy+streaming) 68.6% with larger partition but absolute 1.03 MB. No further growth headroom 0.47 MB.
- Runtime heap gates remain unchecked (COM1 only, no CP210x/CH340). Phases 11/12 will fill after HW.

Previous planning (§3–4) retained as budget hypothesis — now annotated.

---

## 9. Future Mitigations if Tight

1. Reduce `CONFIG_ESP_WIFI_*` further (static rx 4).
2. Cap `max_body_bytes` to 4 KB (sufficient for simple chat).
3. Use global CA store with least-common bundle (`CMN`) — flash 40 KB vs 100 KB, slightly more RAM free.
4. Disable `CONFIG_ESP_TASK_WDT` debug strings to save ~2 KB.
5. Move routing table to `const` flash (PROGMEM style) vs DRAM.

Prefer simplicity now; tune only when gate fails.


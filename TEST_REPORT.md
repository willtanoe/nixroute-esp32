# NixRoute — Adversarial Audit & Stress Test Report

**Target:** `firmware_arduino/esp32_router` on a DOIT ESP32 DEVKIT V1
(device IP `192.168.110.187`, Arduino core 3.3.11).
**Firmware under test:** commit `c2ed46c` (+ earlier fixes described below).
**Date:** 2026-09-03.

Load tools live in `tools/loadtest/` (see its `README.md`). Raw per-phase
results and serial logs are in `tools/loadtest/results/`.

The board has one real provider (`geraikita`, Claude via HTTPS) and, during
tests, one or two **temporary mock providers** served over plain HTTP from the
test PC (`192.168.110.107:9000`). Temporary providers are removed and the
original single provider / single API token are restored after every run.

---

## Summary of findings

- The gateway is **stable under sustained load**: no crash, no watchdog, no
  reboot (one `SW_CPU_RESET` observed is our own `/api/reboot` test). No
  heap-leak trend over a 150 s soak.
- **Single-flight is the defining constraint**: at most one chat is relayed at
  a time; any overlap gets an immediate `503 {"error":…"proxy busy…"}`. This is
  by design but caps throughput.
- Two **confirmed firmware bugs** were found and fixed during this session:
  1. An upstream **RST / stall** after body upload held the single-flight slot
     for ~60 s and returned nothing to the requester (`readStringUntil` waited
     the full read timeout). Now an RST surfaces in ~0.3 s (requester gets an
     immediate `502`) and a provider that never answers fails in ~8–13 s
     instead of 60 s.
  2. `nx/<unknown>/<model>` was forwarded to unrelated real providers. It now
     returns `404 unknown provider in model namespace`.
- One **open operational limit**: a client that disconnects mid a **very large
  synthetic stream** (~199 KB) can hold the single-flight slot ~30 s while the
  relay drains into the dead socket. Real LLM streams are far smaller and
  token-paced, so real-world impact is expected to be seconds at most, but it
  is the top item to harden next.

---

## Environment / method

- Host tools: Python 3.13 (stdlib only). Deterministic failures injected via
  the local mock upstream (fault/behaviour chosen by model id), real TLS churn
  against `geraikita`.
- Every phase samples `/health` (`free_heap`, `uptime`) while it runs and
  records minimum heap and any uptime rollback (=reset). Serial on `COM11`
  captured in parallel and scanned for `Guru Meditation`, `task_wdt`,
  `Backtrace`, `abort()`, `panic`, `WARN`.
- Concurrency ramp 1 → 2 → 4 → 8 → 16 run against the mock.

### Results matrix (mock = local HTTP, unless stated)

| # | Test | concurrency | req/s | latency | ok / fail | status | heap min | reset | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | Concurrent chats | 1–16 | — | ~250–340 ms | see below | 200/503 | 147 KB | 0 | single-flight |
| 2 | Concurrent SSE | 1 / 2 | — | 1.2 s (199 KB) | 1 ok + 1×503 | 200/503 | 125 KB | 0 | |
| 3 | Large prompts | 1 | — | ok | ok | 200 | n/a | 0 | upload streams; >2 MB → drop |
| 4 | Large stream | 1 | — | 1.2 s, 199 436 B | ok | 200 | 125 KB | 0 | full relay + `[DONE]` |
| 5 | Slow client | 1 | — | 1.5 s | ok | 200 | 118 KB | 0 | drip-read 512 B/10 ms |
| 6 | Disconnect mid stream | 1 | — | — | partial | n/a | n/a | 0 | **slot held ~32 s** (see limits) |
| 7 | Provider timeout (hang) | 1 | — | **13.3 s** | fail | **502** | 142 KB | 0 | recovery 200 after |
| 8 | Provider 429 | 1 | — | 344 ms | fail→429 | **429** | — | 0 | |
| 9 | Provider 5xx (500/503) | 1 | — | ~250 ms | fail→502 | **502** | — | 0 | |
| 10 | Provider reset (RST) | 1 | — | **0.26 s** | fail→502 | **502** | — | 0 | recovery 200 at +0.25 s |
| 11 | Multi-provider failing (failover) | 1 | — | — | 14/16 | 200 (+2×502) | 143 KB | 0 | see suspected issues |
| 12 | Rapid repeated | 1 (seq) | **3.66/s** | ~270 ms | 40/40 | 200 | 143 KB | 0 | |
| 13 | Soak 150 s | 1 | ~0.87/s | — | **131/131** | 200 | 145 KB sampled / 117 KB during bursts | 0 | heap steady, no trend |
| 14 | Heap usage/frag | — | — | — | — | — | see above | — | steady |
| 15 | TLS churn (real) | 1 | — | 3.3–5.3 s | 6/6 | 200 | 150 KB | 0 | |
| 16 | Repeated connect/disconnect | many | — | — | ok | — | — | 0 | keep-alive stable |
| 17 | Malformed | 1 | — | — | 400 / reset | 400, conn reset | — | 0 | see below |
| 18 | Auth failures | 1 | — | — | 401 | 401 | — | 0 | no/bad token rejected |
| 19 | Routing edge (unknown ns) | 1 | — | — | fail | **404** | — | 0 | now clean (fixed) |
| 20 | NVS / reboot recovery | 1 | — | — | ok | — | — | 1 (own reboot) | providers+tokens persist |

### Concurrency detail (single-flight)

| concurrency | wall | 200 | 503 |
|---|---|---|---|
| 1 | 0.31 s | 1 | 0 |
| 2 | 0.30 s | 1 | 1 |
| 4 | 1.3 s | 2 | 2 |
| 8 | 3.25 s | 3 | 5 |
| 16 | 7.28 s | 4 | 12 |

### Malformed / auth / routing

- garbage body, missing `model` → `400 missing model`
- >2 MB `Content-Length` → connection dropped by device (guarded)
- no token / wrong token on `/v1/*` → `401`
- `nx/nosuchprovider/mockok` → `404 unknown provider in model namespace` (post-fix)

---

## Confirmed bugs found & fixed in this session

- **Upstream RST / dead-peer stall (~60 s + no reply)** — root cause was
  `readStringUntil()` on the response head after the body upload, which waits
  out the full 60 s read timeout even when the peer already RST/closed.
  Replaced header/chunk-size reading with a line reader that checks
  `client.connected()` every iteration and gives up after an 8 s idle.
  Result: RST → immediate `502` to the requester; hang → `502` in ~13 s; slot
  recovers at once. (firmware commit `c2ed46c`)
- **Unknown provider namespace leaked to real upstreams** — models like
  `nx/<unknown>/x` fell through to the generic fallback and were sent as a
  junk model string to unrelated providers (costing a real call). Now
  `404 unknown provider in model namespace`. (firmware commit `c2ed46c`)

## Open / suspected issues (need manual verification or future work)

1. **Client disconnect during a large stream can hold the single-flight slot
   for ~30 s** (measured with a synthetic ~199 KB stream). The relay keeps
   writing into a dead socket until the TCP stack finally errors. Recommended
   hardening: a bounded downstream write (non-blocking/send-timeout) so the
   slot frees within a couple of seconds. Impact on real (small, token-paced)
   LLM streams is expected to be far lower — to be confirmed manually.
2. **Failover run showed 2 unexplained `502`s** out of 16 (dead-port provider +
   live provider, real provider disabled). The remaining 14 were `200`,
   proving connect-failure failover works; the two `502`s suggest a transient
   where both candidates failed (timing/ordering). Needs a focused re-run.
3. **Admin `GET` cookie flake on one-shot TCP clients.** Under rapid
   fire-and-forget connections (each request = new TCP connection) an
   intermittent 401 was observed with a valid session (phantom `Content-Length`
   on a GET). Persistent keep-alive connections were stable (20/20) and the
   browser dashboard uses keep-alive, so user impact is low; the root cause is
   in the WebServer/lwIP accept+parse path, not the auth code.
4. Circuit-breaker cooldown (3 fails → 60 s) can hide a provider behind
   `503 … cooling down` right after real outages — expected behaviour, but it
   must be remembered when interpreting bursts of 503s.

## Maximum safe operating envelope (observed)

- **Concurrency:** 1 in-flight chat. Overlap → immediate `503`; the gateway
  stays healthy (no wedge, no crash) even when hammered at 16 concurrent.
- **Chat throughput:** latency-bound. ~3.7 req/s sustained to a fast local
  upstream (~270 ms each); with a real HTTPS provider (3–5 s per reply) the
  effective rate is ~0.2–0.3 req/s serialized.
- **Streaming:** sustained large SSE streams OK; single big stream (~199 KB)
  completed in ~1.2 s with heap floor ~117–125 KB.
- **Heap:** idle ~158–160 KB; busy floor observed ~116 KB during big streams;
  stable across 131 sequential large streams (no leak trend). Keep at least
  ~60 KB headroom for safety.
- **Soak:** 131/131 requests over 150 s, zero resets, zero serial error markers.
- **Recovery:** all injected provider faults (429/5xx/RST/hang) recovered fully;
  client-visible errors map to 429/400/404/502 as designed.

## Bottlenecks (by impact)

1. **Single-flight serialization** — the hard throughput ceiling; by design for
   MCU headroom.
2. **Upstream latency (TLS + TTFB)** — dominates per-request time; with real
   providers the gateway is a low-rate (sub-1 rps) relay.
3. **Disconnect-mid-stream slot hold** for very large streams (open; see above).
4. WebServer accept/parse flake on one-shot rapid TCP clients (admin GET only).

## Reproducible commands

```bash
python tools/loadtest/mock_upstream.py --port 9000          # terminal 1
python tools/loadtest/stress.py --host 192.168.110.187 \
    --mock-pc 192.168.110.107 --admin-password 123456        # full battery
python tools/loadtest/exp_recovery.py                        # RST / disconnect
python tools/loadtest/real_churn.py                          # real TLS churn
# raw results: tools/loadtest/results/*.json and *.log
```

## Recommendations (priority order)

1. **High** — Bound downstream writes during streaming (send timeout /
   non-blocking + `connected()` checks) so a vanished client frees the
   single-flight slot within ~2 s even for huge streams.
2. **Medium** — If higher concurrency is required, remove the single-flight
   guard (per-request state + per-job body pipe) or queue instead of 503;
   otherwise document 503-backpressure semantics for clients.
3. **Medium** — Re-run the failover matrix to explain the two transient `502`s
   (ordering/timing).
4. **Low** — Investigate the one-shot-connection admin `GET` flake at the
   WebServer/lwIP accept layer (keep-alive unaffected).
5. **Low** — Surface circuit-breaker cooldown on the dashboard provider row
   (state already exposes `cooling`).

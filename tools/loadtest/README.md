# NixRoute stress / load-testing tools

Adversarial load-testing harness for the NixRoute ESP32 AI gateway.
**It does not modify production firmware.** It only drives the gateway over
HTTP and manages temporary mock providers through the admin API, then restores
the original configuration.

## Layout

```
tools/loadtest/
  mock_upstream.py     deterministic fake OpenAI upstream (429/5xx/reset/hang/
                       huge/… selected by model id)
  stress.py            scenario runner + concurrency/soak/reporting
  exp_recovery.py      focused recovery experiments (upstream RST, mid-stream
                       disconnect)
  real_churn.py        real-provider TLS churn + config-restore check
  results/             raw JSON + serial logs per phase
```

## Quick start

```bash
# terminal 1: deterministic mock upstream (reachable by the ESP32 on the LAN)
python tools/loadtest/mock_upstream.py --port 9000

# terminal 2: run everything against the board (adjust IPs)
python tools/loadtest/stress.py --host 192.168.110.187 \
    --mock-pc 192.168.110.107 --mock-port 9000 --admin-password 123456

# run only some phases (default runs all, including a 150 s soak)
python tools/loadtest/stress.py --host 192.168.110.187 --only baseline,concurrency
python tools/loadtest/stress.py --host 192.168.110.187 --only soak --soak-sec 300
```

All results are written to `tools/loadtest/results/phase_<name>.json` and a
`summary.json`.

## Behaviour injection (model id → upstream behaviour)

| model | behaviour |
|---|---|
| `mockok` / `mockecho` | quick 200 (stream or JSON), echoes your user text |
| `mock429/500/503/400/401` | fixed error status |
| `mockhang` | accepts body, never replies (tests idle/hang handling) |
| `mockreset` | accepts body, then RST |
| `mockmid` | streams a little SSE then dies mid-stream |
| `mockbig` | ~180 KB non-streaming JSON |
| `mockhuge` | ~199 KB streaming SSE (many chunks) |
| `mockchunks` | one upstream chunk >> 1 KB (framing test) |
| `mockblob` | echoes request body size + hash |

Chat with them as `model = nx/<mockok|mockdead>/<behaviour>` (the runner
registers two temporary providers: `mockok` on the live port and `mockdead`
pointing at a closed port).

## What it records

Every phase: status distribution, wall time, per-request latency, free heap
minimum (sampled via `/health`), device resets (uptime rollback), and – in the
serial log – watchdog/crash markers. See `TEST_REPORT.md` at the repository
root for the results and operating envelope.

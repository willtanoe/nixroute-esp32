#!/usr/bin/env python3
"""Aggressive stress/soak harness for the NixRoute ESP32 gateway.

Usage:
  python mock_upstream.py --port 9000            # terminal 1
  python stress.py --host 192.168.110.187 --mock-pc 192.168.110.107 --mock-port 9000

Reads/writes admin state on the device (adds/removes temp mock providers and
temporarily toggles the real provider) then restores the original config.
Writes per-phase JSON + a human summary into tools/loadtest/results/.
"""
import argparse, json, time, threading, os, sys, socket, hashlib
import http.client

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
os.makedirs(RES, exist_ok=True)

ADMIN_COOKIE = [None]
_admin = [None]  # persistent http.client conn


def admin_conn(host):
    return http.client.HTTPConnection(host, 80, timeout=30)


def admin_login(host, password):
    c = admin_conn(host)
    c.request("POST", "/admin/login", "password=%s" % password,
              {"Content-Type": "application/x-www-form-urlencoded"})
    r = c.getresponse(); r.read()
    for k, v in r.getheaders():
        if k.lower() == "set-cookie":
            ADMIN_COOKIE[0] = v.split(";")[0]
    _admin[0] = c
    return ADMIN_COOKIE[0]


def a_req(host, method, path, body=None, t=60):
    # keep-alive admin connection; reconnect if stale
    if _admin[0] is None:
        _admin[0] = admin_conn(host)
    c = _admin[0]
    try:
        c.request(method, path, body=json.dumps(body).encode() if body is not None else None,
                  headers={"Cookie": ADMIN_COOKIE[0], "X-NixRoute": "1",
                           "Content-Type": "application/json"})
        r = c.getresponse(); data = r.read()
        try:
            j = json.loads(data.decode() or "null")
        except Exception:
            j = None
        return r.status, j
    except Exception:
        try:
            _admin[0].close()
        except Exception:
            pass
        _admin[0] = admin_conn(host)
        try:
            _admin[0].request(method, path, body=json.dumps(body).encode() if body is not None else None,
                              headers={"Cookie": ADMIN_COOKIE[0], "X-NixRoute": "1",
                                       "Content-Type": "application/json"})
            r = _admin[0].getresponse(); data = r.read()
            return r.status, json.loads(data.decode() or "null")
        except Exception as e:
            return -1, {"error": str(e)}


def chat_once(host, token, model, payload_extra=None, t=90, headers=None):
    h = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
    h.update(headers or {})
    body = {"model": model,
            "messages": [{"role": "user", "content": "hello from stress"}]}
    if payload_extra:
        body.update(payload_extra)
    c = http.client.HTTPConnection(host, 80, timeout=t)
    t0 = time.time()
    try:
        c.request("POST", "/v1/chat/completions", body=json.dumps(body).encode(), headers=h)
        r = c.getresponse()
        raw = r.read()
        dur = time.time() - t0
        return r.status, raw, dur
    except Exception as e:
        return -1, str(e).encode(), time.time() - t0
    finally:
        c.close()


def health(host, t=8):
    c = http.client.HTTPConnection(host, 80, timeout=t)
    try:
        c.request("GET", "/health")
        r = c.getresponse()
        return json.loads(r.read().decode())
    finally:
        c.close()


class HeapMon(threading.Thread):
    """Samples /health. Records min heap and any uptime rollback (reboot)."""
    def __init__(self, host, name):
        super().__init__(daemon=True)
        self.host = host
        self.name = name
        self.min_heap = None
        self.samples = []
        self.resets = []
        self.last_uptime = None
        self.stop = False

    def run(self):
        while not self.stop:
            try:
                j = health(self.host)
                hp = j.get("free_heap")
                if hp is not None:
                    self.samples.append(hp)
                    self.min_heap = hp if self.min_heap is None else min(self.min_heap, hp)
                up = j.get("uptime_s")
                if up is not None:
                    if self.last_uptime is not None and up < self.last_uptime - 1:
                        self.resets.append(time.time())
                    self.last_uptime = up
            except Exception:
                pass
            time.sleep(0.35)

    def stopmon(self):
        self.stop = True
        self.join(timeout=2)


def add_provider(host, name, url, key="sk-mock"):
    st, j = a_req(host, "POST", "/api/providers", {"name": name, "url": url, "key": key, "active": True})
    return st, (j or {}).get("id"), j


def del_provider(host, pid):
    return a_req(host, "POST", "/api/providers/remove", {"id": pid})


def toggle_provider(host, pid, active):
    return a_req(host, "POST", "/api/providers/toggle", {"id": pid, "active": active})


# ---------------------------------------------------------------- scenarios
def phase_state(args, token):
    st, j = a_req(args.host, "GET", "/api/state")
    return st, j


def scenario_baseline(args, token):
    out = {"name": "baseline", "results": []}
    mon = HeapMon(args.host, "base"); mon.start()
    for i in range(10):
        st, raw, dur = chat_once(args.host, token, "nx/mockok/mockok", {"stream": False})
        out["results"].append({"i": i, "status": st, "ms": round(dur * 1000, 1)})
    mon.stopmon()
    out["min_heap"] = mon.min_heap
    return out


def run_concurrency(args, token):
    """1..16 concurrent chat requests against mock (single-flight gateway)."""
    out = {"name": "concurrency", "levels": []}
    for c in [1, 2, 4, 8, 16]:
        res = []
        mon = HeapMon(args.host, "conc%d" % c); mon.start()
        lock = threading.Lock()
        def worker(i):
            st, raw, dur = chat_once(args.host, token, "nx/mockok/mockok", {"stream": False})
            with lock:
                res.append({"i": i, "status": st, "ms": round(dur * 1000, 1)})
        ts = [threading.Thread(target=worker, args=(i,)) for i in range(c)]
        t0 = time.time()
        for t in ts: t.start()
        for t in ts: t.join()
        wall = time.time() - t0
        mon.stopmon()
        codes = {}
        for r in res:
            codes[r["status"]] = codes.get(r["status"], 0) + 1
        out["levels"].append({"concurrency": c, "wall_s": round(wall, 2),
                              "ok": codes.get(200, 0), "busy_503": codes.get(503, 0),
                              "other": {k: v for k, v in codes.items() if k not in (200, 503)},
                              "min_heap": mon.min_heap, "resets": len(mon.resets)})
    return out


def scenario_streams(args, token):
    out = {"name": "sse-streams", "results": []}
    # one big streaming response, watch heap while it flows
    mon = HeapMon(args.host, "huge"); mon.start()
    c = http.client.HTTPConnection(args.host, 80, timeout=120)
    body = json.dumps({"model": "nx/mockok/mockhuge", "stream": True,
                       "messages": [{"role": "user", "content": "hi"}]})
    t0 = time.time(); got = 0
    try:
        c.request("POST", "/v1/chat/completions", body=body.encode(),
                  headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        r = c.getresponse()
        while True:
            chunk = r.read(4096)
            if not chunk:
                break
            got += len(chunk)
    except Exception as e:
        out["results"].append({"error": str(e)})
    finally:
        c.close()
    dur = time.time() - t0
    mon.stopmon()
    out["results"].append({"bytes": got, "sec": round(dur, 2), "min_heap": mon.min_heap,
                           "resets": len(mon.resets)})
    # overlapping: two streams -> expect one 503
    outs = [None, None]
    def one(i):
        st, raw, d = chat_once(args.host, token, "nx/mockok/mockhuge", {"stream": True}, t=120)
        outs[i] = (st, len(raw))
    ts = [threading.Thread(target=one, args=(i,)) for i in range(2)]
    for t in ts: t.start()
    for t in ts: t.join()
    out["results"].append({"overlap_statuses": [o[0] for o in outs]})
    return out


def scenario_slow_client(args, token):
    out = {"name": "slow-client", "result": {}}
    c = http.client.HTTPConnection(args.host, 80, timeout=120)
    body = json.dumps({"model": "nx/mockok/mockhuge", "stream": True,
                       "messages": [{"role": "user", "content": "hi"}]})
    mon = HeapMon(args.host, "slow"); mon.start()
    t0 = time.time()
    try:
        c.request("POST", "/v1/chat/completions", body=body.encode(),
                  headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        r = c.getresponse()
        total = 0
        while total < 60000:
            chunk = r.read(512)
            if not chunk:
                break
            total += len(chunk)
            time.sleep(0.01)   # drip-feed the gateway
    except Exception as e:
        out["result"]["error"] = str(e)
    finally:
        c.close()
    dur = time.time() - t0
    mon.stopmon()
    out["result"].update({"bytes_read": total, "sec": round(dur, 2),
                          "min_heap": mon.min_heap, "resets": len(mon.resets)})
    return out


def scenario_disconnect(args, token):
    out = {"name": "client-disconnect", "result": {}}
    c = http.client.HTTPConnection(args.host, 80, timeout=90)
    body = json.dumps({"model": "nx/mockok/mockhuge", "stream": True,
                       "messages": [{"role": "user", "content": "hi"}]})
    try:
        c.request("POST", "/v1/chat/completions", body=body.encode(),
                  headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        r = c.getresponse()
        first = r.read(2048)  # read a little then vanish
        out["result"]["first_bytes"] = len(first)
        c.close()  # abort mid-stream
        time.sleep(1.5)
    except Exception as e:
        out["result"]["error"] = str(e)
    # recovery probe
    st, raw, dur = chat_once(args.host, token, "nx/mockok/mockok", {"stream": False})
    out["result"]["recovery_status"] = st
    return out


def scenario_provider_faults(args, token):
    out = {"name": "provider-faults", "results": []}
    for model, expect in [("mock429", 429), ("mock500", 500), ("mock503", 503),
                          ("mock400", 400), ("mockreset", 502)]:
        st, raw, dur = chat_once(args.host, token, "nx/mockok/" + model, {"stream": False})
        body = raw.decode("utf-8", "replace")[:120]
        out["results"].append({"model": model, "status": st, "expected": expect,
                               "ms": round(dur * 1000, 1), "body": body})
    # mid-stream abort
    st, raw, dur = chat_once(args.host, token, "nx/mockok/mockmid", {"stream": True}, t=60)
    out["results"].append({"model": "mockmid-stream", "status": st,
                           "bytes": len(raw), "ms": round(dur * 1000, 1)})
    # recovery
    st, raw, dur = chat_once(args.host, token, "nx/mockok/mockok", {"stream": False})
    out["results"].append({"recovery": st})
    return out


def scenario_malformed(args, token):
    out = {"name": "malformed", "results": []}
    # garbage body
    c = http.client.HTTPConnection(args.host, 80, timeout=20)
    c.request("POST", "/v1/chat/completions", b"not json at all {{{",
              headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    r = c.getresponse(); rb = r.read()
    out["results"].append({"case": "garbage-json", "status": r.status, "body": rb[:80].decode("utf-8", "replace")})
    c.close()
    # missing model
    c = http.client.HTTPConnection(args.host, 80, timeout=20)
    c.request("POST", "/v1/chat/completions", b'{"messages":[{"role":"user","content":"x"}]}',
              headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
    r = c.getresponse(); rb = r.read()
    out["results"].append({"case": "missing-model", "status": r.status, "body": rb[:80].decode("utf-8", "replace")})
    c.close()
    # huge content-length (drop path, 2 MB+)
    c = http.client.HTTPConnection(args.host, 80, timeout=30)
    big = b'{"model":"x","messages":[]}' + b" " * (2 * 1024 * 1024 + 100)
    try:
        c.request("POST", "/v1/chat/completions", big,
                  headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        r = c.getresponse(); rb = r.read()
        out["results"].append({"case": "huge-body-2MB", "status": r.status})
    except Exception as e:
        out["results"].append({"case": "huge-body-2MB", "status": "exc:" + str(e)[:60]})
    finally:
        c.close()
    # unknown provider namespace
    st, raw, dur = chat_once(args.host, token, "nx/nosuchprovider/mockok")
    out["results"].append({"case": "unknown-provider-ns", "status": st,
                           "body": raw.decode("utf-8", "replace")[:80]})
    return out


def scenario_auth(args, token):
    out = {"name": "auth", "results": []}
    c = http.client.HTTPConnection(args.host, 80, timeout=20)
    c.request("GET", "/v1/models")
    r = c.getresponse(); rb = r.read()
    out["results"].append({"case": "models-no-token", "status": r.status})
    c.close()
    c = http.client.HTTPConnection(args.host, 80, timeout=20)
    c.request("GET", "/v1/models", headers={"Authorization": "Bearer wrongtoken"})
    r = c.getresponse(); rb = r.read()
    out["results"].append({"case": "models-bad-token", "status": r.status})
    c.close()
    st, raw, dur = chat_once(args.host, token, "nx/mockok/mockok")
    out["results"].append({"case": "chat-good-token", "status": st})
    return out


def scenario_rapid_sequential(args, token):
    out = {"name": "rapid-sequential", "results": {}}
    N = 40
    ok = 0; fails = []
    mon = HeapMon(args.host, "rapid"); mon.start()
    c = http.client.HTTPConnection(args.host, 80, timeout=60)
    t0 = time.time()
    body = json.dumps({"model": "nx/mockok/mockok", "stream": False,
                       "messages": [{"role": "user", "content": "ping"}]})
    for i in range(N):
        try:
            c.request("POST", "/v1/chat/completions", body=body.encode(),
                      headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            r = c.getresponse(); r.read()
            if r.status == 200:
                ok += 1
            else:
                fails.append((i, r.status))
        except Exception as e:
            fails.append((i, str(e)[:50]))
    wall = time.time() - t0
    c.close()
    mon.stopmon()
    out["results"].update({"requests": N, "ok": ok, "fails": fails[:10],
                           "req_per_s": round(N / wall, 2), "wall_s": round(wall, 2),
                           "min_heap": mon.min_heap, "resets": len(mon.resets)})
    return out


def scenario_provider_timeout(args, token):
    out = {"name": "provider-timeout", "result": {}}
    mon = HeapMon(args.host, "hang"); mon.start()
    st, raw, dur = chat_once(args.host, token, "nx/mockok/mockhang", {"stream": False}, t=80)
    mon.stopmon()
    out["result"] = {"status": st, "ms": round(dur * 1000, 1),
                     "body": raw.decode("utf-8", "replace")[:80],
                     "min_heap": mon.min_heap, "resets": len(mon.resets)}
    # recovery
    st2, raw2, dur2 = chat_once(args.host, token, "nx/mockok/mockok", {"stream": False})
    out["result"]["recovery"] = st2
    return out


def scenario_failover(args, token):
    out = {"name": "failover", "results": {}}
    # only dead + ok mocks active; geraikita disabled for isolation
    toggle_provider(args.host, "geraikita", False)
    time.sleep(0.3)
    res = []
    mon = HeapMon(args.host, "fo"); mon.start()
    for i in range(16):
        st, raw, dur = chat_once(args.host, token, "zzz-shared-fallback-model", {"stream": False}, t=30)
        res.append(st)
    mon.stopmon()
    toggle_provider(args.host, "geraikita", True)
    out["results"] = {"statuses": res, "ok": res.count(200),
                      "min_heap": mon.min_heap, "resets": len(mon.resets)}
    return out


def scenario_reboot_recovery(args, token):
    out = {"name": "reboot-recovery", "results": {}}
    st, j = a_req(args.host, "GET", "/api/state")
    provs = [p["id"] for p in (j or {}).get("providers", [])]
    toks = len((j or {}).get("token", {}).get("list", []))
    out["results"]["before"] = {"providers": provs, "tokens": toks}
    st, _ = a_req(args.host, "POST", "/api/reboot", {})
    time.sleep(10)
    # wait for boot
    for _ in range(40):
        try:
            health(args.host)
            break
        except Exception:
            time.sleep(1)
    time.sleep(2)
    cookie = admin_login(args.host, args.admin_password)
    time.sleep(1)
    st, j = a_req(args.host, "GET", "/api/state")
    out["results"]["after"] = {"providers": [p["id"] for p in (j or {}).get("providers", [])],
                               "tokens": len((j or {}).get("token", {}).get("list", []))} if j else None
    return out


def scenario_soak(args, token, seconds):
    out = {"name": "soak", "results": {}}
    N = 0; ok = 0
    heap_series = []
    mon = HeapMon(args.host, "soak"); mon.start()
    t_end = time.time() + seconds
    c = http.client.HTTPConnection(args.host, 80, timeout=60)
    body = json.dumps({"model": "nx/mockok/mockhuge", "stream": True,
                       "messages": [{"role": "user", "content": "soak"}]})
    while time.time() < t_end:
        try:
            c.request("POST", "/v1/chat/completions", body=body.encode(),
                      headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
            r = c.getresponse()
            n = 0
            while True:
                ch = r.read(4096)
                if not ch:
                    break
                n += len(ch)
            if r.status == 200:
                ok += 1
            N += 1
        except Exception as e:
            N += 1
        if len(heap_series) < 2000:
            try:
                heap_series.append(health(args.host)["free_heap"])
            except Exception:
                pass
    c.close()
    mon.stopmon()
    out["results"] = {"requests": N, "ok": ok, "heap_samples": heap_series,
                      "min_heap": min(heap_series) if heap_series else None,
                      "min_mon_heap": mon.min_heap, "resets": len(mon.resets)}
    return out


def save(name, data):
    fn = os.path.join(RES, name + ".json")
    with open(fn, "w") as f:
        json.dump(data, f, indent=1)
    print("saved", fn)
    return fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.110.187")
    ap.add_argument("--mock-pc", default="192.168.110.107")
    ap.add_argument("--mock-port", type=int, default=9000)
    ap.add_argument("--admin-password", default="123456")
    ap.add_argument("--only", default=None, help="comma list e.g. baseline,soak")
    ap.add_argument("--soak-sec", type=int, default=150)
    a = ap.parse_args()

    mock_url = "http://%s:%d/v1" % (a.mock_pc, a.mock_port)

    # admin session + token
    admin_login(a.host, a.admin_password)
    st, j = a_req(a.host, "GET", "/api/state")
    if st != 200 or not j:
        print("cannot reach admin api:", st, j); return 1
    token = j["token"]["list"][0]
    before = [p["id"] for p in j["providers"]]
    print("host=%s token=%s providers_before=%s" % (a.host, token[:10], before))

    # ensure temp mock providers exist
    add_provider(a.host, "mockok", mock_url)
    time.sleep(0.3)
    add_provider(a.host, "mockdead", "http://%s:1/v1" % a.mock_pc)  # refused
    time.sleep(0.3)

    phases = {}
    def run(name, fn, *args):
        print("\n=== %s ===" % name, flush=True)
        try:
            r = fn(*args)
            phases[name] = r
            save("phase_%s" % name, r)
        except Exception as e:
            import traceback; traceback.print_exc()
            phases[name] = {"error": str(e)}
        time.sleep(1.0)

    only = [s.strip() for s in (a.only or "").split(",") if s.strip()]
    want = lambda n: (not only) or (n in only)

    if want("baseline"):
        run("baseline", scenario_baseline, a, token)
    if want("concurrency"):
        run("concurrency", run_concurrency, a, token)
    if want("streams"):
        run("streams", scenario_streams, a, token)
    if want("slow"):
        run("slow", scenario_slow_client, a, token)
    if want("disconnect"):
        run("disconnect", scenario_disconnect, a, token)
    if want("faults"):
        run("faults", scenario_provider_faults, a, token)
    if want("timeout"):
        run("timeout", scenario_provider_timeout, a, token)
    if want("malformed"):
        run("malformed", scenario_malformed, a, token)
    if want("auth"):
        run("auth", scenario_auth, a, token)
    if want("rapid"):
        run("rapid", scenario_rapid_sequential, a, token)
    if want("failover"):
        run("failover", scenario_failover, a, token)
    if want("reboot"):
        run("reboot", scenario_reboot_recovery, a, token)
    if want("soak"):
        run("soak", scenario_soak, a, token, a.soak_sec)

    # cleanup: remove temp mocks, verify original restored
    print("\ncleanup...", flush=True)
    del_provider(a.host, "mockok")
    del_provider(a.host, "mockdead")
    time.sleep(0.5)
    st, j = a_req(a.host, "GET", "/api/state")
    after = [p["id"] for p in (j or {}).get("providers", [])] if j else None
    print("providers_after:", after, "(restored=%s)" % (after == before))
    save("summary", {"before": before, "after": after, "phases": {k: _mini(v) for k, v in phases.items()}})
    print("\nDONE")


def _mini(v):
    if isinstance(v, dict):
        return {k: (_mini(x) if isinstance(x, (dict, list)) else x) for k, x in list(v.items())[:6]}
    return v


if __name__ == "__main__":
    main()

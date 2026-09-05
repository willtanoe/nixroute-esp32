#!/usr/bin/env python3
"""Targeted experiments for upstream-reset and mid-stream disconnect recovery.

Adds a temporary mock provider, measures how long the single-flight gateway
stays busy after (a) an upstream RST and (b) a client disconnecting mid-stream,
then removes the mock provider.
"""
import argparse, http.client, json, time, threading, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress import (admin_login, a_req, add_provider, del_provider, chat_once,
                    health, ADMIN_COOKIE)

HOST = "192.168.110.187"
ADMIN = "123456"
PC = "192.168.110.107"


def parse_args():
    p = argparse.ArgumentParser(description="Upstream-reset / mid-stream disconnect recovery experiment")
    p.add_argument("--host", default=HOST)
    p.add_argument("--admin-password", default=ADMIN)
    p.add_argument("--mock-pc", default=PC)
    p.add_argument("--mock-port", type=int, default=9000)
    return p.parse_args()


def normal_chat(token):
    st, raw, dur = chat_once(HOST, token, "nx/mockok/mockok", {"stream": False}, t=20)
    return st, round(dur * 1000, 1)


def poll_until_good(token, secs=90):
    """Poll normal chat every ~1s; return time-to-first-200 after t0."""
    t0 = time.time()
    rec = []
    while time.time() - t0 < secs:
        st, ms = normal_chat(token)
        rec.append((round(time.time() - t0, 2), st))
        if st == 200:
            return rec
        time.sleep(1.0)
    return rec


def main():
    global HOST, ADMIN, PC
    a = parse_args()
    HOST, ADMIN, PC = a.host, a.admin_password, a.mock_pc

    admin_login(HOST, ADMIN)
    st, j = a_req(HOST, "GET", "/api/state")
    toks = ((j or {}).get("token") or {}).get("list") or []
    if not toks:
        raise SystemExit("no API tokens configured on the device; create one first")
    token = toks[0]
    try:
        run_experiments(token, a.mock_port)
    finally:
        # never leave the mock provider configured, whatever blew up above
        del_provider(HOST, "mockok")
        time.sleep(0.3)
        st, j = a_req(HOST, "GET", "/api/state")
        print("\nproviders after:", [p["id"] for p in (j or {}).get("providers", [])], flush=True)


def run_experiments(token, mock_port):
    add_provider(HOST, "mockok", "http://%s:%d/v1" % (PC, mock_port))
    time.sleep(0.5)
    print("healthy check:", normal_chat(token), flush=True)

    # ---- A) upstream RST after body --------------------------------------
    print("\n[A] upstream RST (client timeout 6 s)", flush=True)
    t0 = time.time()
    c = http.client.HTTPConnection(HOST, 80, timeout=6)
    try:
        body = json.dumps({"model": "nx/mockok/mockreset", "stream": False,
                           "messages": [{"role": "user", "content": "x"}]})
        c.request("POST", "/v1/chat/completions", body.encode(),
                  headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        r = c.getresponse(); raw = r.read()
        print("  client got status", r.status, "after %.1fs" % (time.time() - t0),
              raw[:80], flush=True)
    except Exception as e:
        print("  client error after %.1fs: %s" % (time.time() - t0, str(e)[:80]), flush=True)
    finally:
        c.close()
    print("  recovery polling:", flush=True)
    rec = poll_until_good(token, 75)
    good = [(t, st) for (t, st) in rec if st == 200]
    print("  samples:", rec[:8], flush=True)
    print("  first-200 at: %.2fs" % good[0][0] if good else "  NO RECOVERY IN 75s", flush=True)

    # ---- B) client disconnect mid large stream -----------------------------
    print("\n[B] client disconnect mid huge stream", flush=True)
    c = http.client.HTTPConnection(HOST, 80, timeout=30)
    body = json.dumps({"model": "nx/mockok/mockhuge", "stream": True,
                       "messages": [{"role": "user", "content": "hi"}]})
    t0 = time.time()
    try:
        c.request("POST", "/v1/chat/completions", body.encode(),
                  headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"})
        r = c.getresponse(); got = len(r.read(8192))
        print("  read %d bytes then disconnecting at +%.2fs" % (got, time.time() - t0), flush=True)
        c.close()
    except Exception as e:
        print("  exc:", str(e)[:80], flush=True)
        c.close()
    print("  recovery polling:", flush=True)
    rec = poll_until_good(token, 40)
    good = [(t, st) for (t, st) in rec if st == 200]
    print("  samples:", rec[:8], flush=True)
    print("  first-200 at: %.2fs" % good[0][0] if good else "  NO RECOVERY IN 40s", flush=True)


if __name__ == "__main__":
    main()

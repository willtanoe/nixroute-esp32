#!/usr/bin/env python3
"""
Admin API smoke test for NixRoute.

Usage:
  python test_admin_api.py --host 192.168.1.50 --password 123456
  python test_admin_api.py --host 192.168.1.50 --password 123456 \
      --add-name baroq --add-url https://api.example.com --add-key sk-xxx
"""
import argparse
import json
import urllib.request
import http.cookiejar

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="192.168.1.50")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--password", default="123456")
    p.add_argument("--add-name")
    p.add_argument("--add-url")
    p.add_argument("--add-key", default="")
    return p.parse_args()

def login(base, password):
    url = f"{base}/admin/login"
    data = f"password={urllib.parse.quote(password)}".encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/x-www-form-urlencoded"})
    print(f"POST {url}")
    r = urllib.request.urlopen(req, timeout=5)
    print(f"status {r.status} (expect 303)")
    return r.headers.get("Set-Cookie", "")

def state(base, cookie):
    req = urllib.request.Request(f"{base}/api/state", headers={"Cookie": cookie})
    with urllib.request.urlopen(req, timeout=5) as r:
        data = json.loads(r.read().decode())
        print(f"GET /api/state -> version={data.get('version')} providers={len(data.get('providers', []))}")
        assert "providers" in data, "missing providers in state"
        for p in data["providers"]:
            print(f"  provider: {p['id']} ({p['name']}) models={len(p.get('models', []))}")
            for m in p.get("models", []):
                assert "/" in m, f"model {m} not namespaced as provider/model"
        print("state OK")

def add_provider(base, cookie, name, url, key):
    payload = {"name": name, "url": url, "key": key}
    req = urllib.request.Request(
        f"{base}/api/providers",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "Cookie": cookie},
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        data = json.loads(r.read().decode())
        print(f"POST /api/providers -> id={data.get('id')} fetched={data.get('fetched_models')}")
        return data

if __name__ == "__main__":
    import urllib.parse
    a = parse_args()
    base = f"http://{a.host}:{a.port}"
    cookie = login(base, a.password)
    state(base, cookie)
    if a.add_name and a.add_url:
        add_provider(base, cookie, a.add_name, a.add_url, a.add_key)
        state(base, cookie)

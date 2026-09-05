import argparse, sys, os, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress import admin_login, a_req

DEFAULT_HOST = "192.168.110.187"


def parse_args():
    p = argparse.ArgumentParser(description="Re-sync every provider's model cache")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--admin-password", default="123456")
    return p.parse_args()


def main():
    a = parse_args()
    host = a.host
    admin_login(host, a.admin_password)
    st, j = a_req(host, "GET", "/api/state")
    for _ in range(5):
        if st == 200 and j and "providers" in j:
            break
        time.sleep(1)
        st, j = a_req(host, "GET", "/api/state")
    if not j or "providers" not in j:
        raise SystemExit("state unavailable")
    print("state", st)
    for p in j["providers"]:
        print("before:", p["id"], "active", p["active"], "models", len(p.get("models", [])))
        st2, r = a_req(host, "POST", "/api/providers/fetch", {"id": p["id"]})
        print("  sync ->", st2, (r or {}).get("ok"), (r or {}).get("count"))
        time.sleep(0.4)
    time.sleep(0.5)
    st, j = a_req(host, "GET", "/api/state")
    for p in j["providers"]:
        print("after:", p["id"], "models", len(p.get("models", [])), p.get("models", [])[:3])


if __name__ == "__main__":
    main()

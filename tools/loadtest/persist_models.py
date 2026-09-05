import argparse, sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress import admin_login, a_req, health

DEFAULT_HOST = "192.168.110.187"


def parse_args():
    p = argparse.ArgumentParser(description="Verify the model cache survives a reboot (NVS persistence)")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--admin-password", default="123456")
    return p.parse_args()


def get_state(host):
    for _ in range(8):
        st, j = a_req(host, "GET", "/api/state")
        if st == 200 and j and "providers" in j:
            return j
        time.sleep(0.7)
    raise SystemExit("state unavailable")


def main():
    a = parse_args()
    host = a.host
    admin_login(host, a.admin_password)
    j = get_state(host)
    if not j["providers"]:
        raise SystemExit("no providers configured; add one first")
    print("after flash boot models:", len(j["providers"][0].get("models", [])))

    pid = j["providers"][0]["id"]
    st, r = a_req(host, "POST", "/api/providers/fetch", {"id": pid})
    print("sync ->", st, (r or {}).get("count"))

    time.sleep(0.5)
    j = get_state(host)
    print("before reboot models:", len(j["providers"][0].get("models", [])))

    st, _ = a_req(host, "POST", "/api/reboot", {})
    time.sleep(10)
    for _ in range(40):
        try:
            health(host)
            break
        except Exception:
            time.sleep(1)
    time.sleep(2)
    admin_login(host, a.admin_password)
    j = get_state(host)
    print("after reboot models:", len(j["providers"][0].get("models", [])))
    print("first:", j["providers"][0].get("models", [])[:2])


if __name__ == "__main__":
    main()

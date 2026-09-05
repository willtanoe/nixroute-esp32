import sys, os, time, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress import admin_login, a_req, health

H = "192.168.110.187"

def get_state():
    for _ in range(8):
        st, j = a_req(H, "GET", "/api/state")
        if st == 200 and j and "providers" in j:
            return j
        time.sleep(0.7)
    raise SystemExit("state unavailable")

admin_login(H, "123456")
j = get_state()
print("after flash boot models:", len(j["providers"][0].get("models", [])))

pid = j["providers"][0]["id"]
st, r = a_req(H, "POST", "/api/providers/fetch", {"id": pid})
print("sync ->", st, (r or {}).get("count"))

time.sleep(0.5)
j = get_state()
print("before reboot models:", len(j["providers"][0].get("models", [])))

st, _ = a_req(H, "POST", "/api/reboot", {})
time.sleep(10)
for _ in range(40):
    try:
        health(H)
        break
    except Exception:
        time.sleep(1)
time.sleep(2)
admin_login(H, "123456")
j = get_state()
print("after reboot models:", len(j["providers"][0].get("models", [])))
print("first:", j["providers"][0].get("models", [])[:2])

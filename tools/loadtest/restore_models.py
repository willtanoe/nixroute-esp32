import sys, time
sys.path.insert(0, ".")
from stress import admin_login, a_req

H = "192.168.110.187"
admin_login(H, "123456")
for attempt in range(6):
    st, j = a_req(H, "GET", "/api/state")
    if st == 200 and j and "providers" in j:
        break
    time.sleep(1)
print("state", st)
for p in j["providers"]:
    print("before:", p["id"], "active", p["active"], "models", len(p.get("models", [])))
    st2, r = a_req(H, "POST", "/api/providers/fetch", {"id": p["id"]})
    print("  sync ->", st2, (r or {}).get("ok"), (r or {}).get("count"))
    time.sleep(0.4)
time.sleep(0.5)
st, j = a_req(H, "GET", "/api/state")
for p in j["providers"]:
    print("after:", p["id"], "models", len(p.get("models", [])), p.get("models", [])[:3])

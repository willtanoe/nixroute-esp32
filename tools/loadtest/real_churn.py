import http.client, json, time, sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from stress import admin_login, a_req, health
H = "192.168.110.187"
admin_login(H, "123456")
st, j = a_req(H, "GET", "/api/state")
print("state", st, (list(j.keys()) if j else None))
if not j or "token" not in j:
    for _ in range(5):
        time.sleep(1)
        st, j = a_req(H, "GET", "/api/state")
        if j and "token" in j:
            break
toks = j["token"]["list"]
prov = j["providers"][0]["id"]
keep = toks[0]
for t in toks[1:]:
    a_req(H, "POST", "/api/token/delete", {"token": t})
st, j = a_req(H, "GET", "/api/state")
print("tokens now:", j["token"]["list"], "providers:", [p["id"] for p in j["providers"]])
tok = keep
lat = []
okc = 0
for i in range(6):
    body = {"model": "nx/" + prov + "/claude-opus-5",
            "messages": [{"role": "user", "content": "Reply with the single word: OK"}],
            "stream": False, "max_tokens": 10}
    t0 = time.time()
    st2, rb = a_req(H, "POST", "/v1/chat/completions", body, t=0) if False else (None, None)
    # use chat_once-like fresh call via http.client with bearer
    c = http.client.HTTPConnection(H, 80, timeout=120)
    c.request("POST", "/v1/chat/completions", body=json.dumps(body).encode(),
              headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json", "Connection": "close"})
    r = c.getresponse(); r.read(); c.close()
    lat.append(round((time.time() - t0) * 1000))
    okc += 1 if r.status == 200 else 0
    time.sleep(0.5)
print("real churn %d/6 ok lat(ms) %s" % (okc, lat))
h = health(H)
print("heap", h["free_heap"], "uptime", h["uptime_s"], "req_ok", h["requests_ok"], "req_fail", h["requests_fail"])

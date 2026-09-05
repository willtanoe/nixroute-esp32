import argparse, http.client, json, time, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stress import admin_login, a_req, health

H = "192.168.110.187"


def parse_args():
    p = argparse.ArgumentParser(description="Real-upstream sequential churn probe")
    p.add_argument("--host", default=H)
    p.add_argument("--admin-password", default="123456")
    p.add_argument("--model-suffix", default="claude-opus-5",
                   help="model id appended to nx/<provider>/ (must exist in the provider's cache)")
    return p.parse_args()


def main():
    a = parse_args()
    host = a.host
    admin_login(host, a.admin_password)
    st, j = a_req(host, "GET", "/api/state")
    for _ in range(5):
        if st == 200 and j and "token" in j and j.get("providers"):
            break
        time.sleep(1)
        st, j = a_req(host, "GET", "/api/state")
    if not j or "token" not in j or not j.get("providers"):
        raise SystemExit("state unavailable or no provider/token configured")
    toks = j["token"]["list"]
    prov = j["providers"][0]["id"]
    keep = toks[0]
    for t in toks[1:]:
        a_req(host, "POST", "/api/token/delete", {"token": t})
    st, j = a_req(host, "GET", "/api/state")
    print("tokens now:", j["token"]["list"], "providers:", [p["id"] for p in j["providers"]])
    tok = keep
    lat = []
    okc = 0
    for i in range(6):
        body = {"model": "nx/" + prov + "/" + a.model_suffix,
                "messages": [{"role": "user", "content": "Reply with the single word: OK"}],
                "stream": False, "max_tokens": 10}
        t0 = time.time()
        c = http.client.HTTPConnection(host, 80, timeout=120)
        try:
            c.request("POST", "/v1/chat/completions", body=json.dumps(body).encode(),
                      headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json",
                               "Connection": "close"})
            r = c.getresponse()
            r.read()
            status = r.status  # capture before close; r.status is dead afterwards
        finally:
            c.close()
        lat.append(round((time.time() - t0) * 1000))
        okc += 1 if status == 200 else 0
        time.sleep(0.5)
    print("real churn %d/6 ok lat(ms) %s" % (okc, lat))
    h = health(host)
    print("heap", h["free_heap"], "uptime", h["uptime_s"], "req_ok", h["requests_ok"], "req_fail", h["requests_fail"])


if __name__ == "__main__":
    main()

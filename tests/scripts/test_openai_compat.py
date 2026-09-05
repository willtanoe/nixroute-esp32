#!/usr/bin/env python3
"""
OpenAI-compatible client test — Phase 5+
Tests POST /v1/chat/completions via curl-like + OpenAI SDK path.

Usage:
  python test_openai_compat.py --host 192.168.1.50 --model deepseek-chat --token LOCAL
"""
import argparse, json, urllib.request, urllib.error

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="192.168.1.50")
    p.add_argument("--port", type=int, default=80)
    p.add_argument("--model", default="deepseek-chat")
    p.add_argument("--token", default="")
    p.add_argument("--stream", action="store_true")
    return p.parse_args()

def test_chat(args):
    url = f"http://{args.host}:{args.port}/v1/chat/completions"
    payload = {
        "model": args.model,
        "messages": [{"role": "user", "content": "Hello via NixRoute"}],
        "stream": args.stream
    }
    headers = {"Content-Type": "application/json"}
    if args.token:
        headers["Authorization"] = f"Bearer {args.token}"
    data = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=data, headers=headers)
    print(f"POST {url} model={args.model} stream={args.stream}")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            print(f"status {r.status}")
            if args.stream:
                got_content = False
                for line in r:
                    text = line.decode(errors='ignore').strip()
                    print(text)
                    if text.startswith("data:") and '"content"' in text:
                        got_content = True
                assert got_content, "no streamed content deltas received"
            else:
                body = r.read().decode()
                print(body[:2000])
                resp = json.loads(body)
                assert resp.get("choices"), "response missing choices"
                assert resp["choices"][0].get("message", {}).get("content"), "empty message content"
        print("openai-compat OK")
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode(errors='ignore')[:500]}")
        raise SystemExit(1)

if __name__ == "__main__":
    test_chat(parse_args())

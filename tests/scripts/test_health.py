#!/usr/bin/env python3
"""
Test script for ESP32 AI Router — Phase 3+
Usage: python test_health.py --host 192.168.1.50 --port 80
"""
import argparse
import json
import urllib.request

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="192.168.1.50")
    p.add_argument("--port", type=int, default=80)
    return p.parse_args()

def test_health(host, port):
    url = f"http://{host}:{port}/health"
    print(f"GET {url}")
    with urllib.request.urlopen(url, timeout=5) as r:
        print(f"status: {r.status}")
        body = r.read().decode()
        print(body)
        data = json.loads(body)
        assert "status" in data or "heap" in data, "unexpected health shape"
        print("health OK")

if __name__ == "__main__":
    a = parse_args()
    test_health(a.host, a.port)

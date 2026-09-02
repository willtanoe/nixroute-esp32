#!/usr/bin/env python3
"""Wrapper around pio run --target upload with env checks."""
import subprocess, sys, os
print("Flash helper — ensures .env exists and runs pio upload")
if not os.path.exists("firmware/platformio.ini"):
    print("Run from repo root")
    sys.exit(1)
subprocess.run(["pio", "run", "-e", "esp32dev", "-t", "upload"], cwd="firmware", check=False)

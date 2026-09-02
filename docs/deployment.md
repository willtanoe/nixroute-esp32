# Deployment — Phase 1 stub

## Build

```
cp .env.example .env   # fill locally
pio run -e esp32dev            # build
pio run -e esp32dev -t size    # check flash ~15% baseline
```

## Flash (when hardware attached)

Check port: `pio device list`
```
pio run -e esp32dev -t upload --upload-port COMx
pio device monitor -b 115200
```

Hardware not present on CI — mark flash validation as **pending**.

## NVS Config

Via `idf.py` or serial provisioning (Phase 10). Current skeleton uses NVS defaults; set:
- WIFI_SSID, WIFI_PASSWORD, deepseek_key in `config_manager`.

## Security

- Inbound plain HTTP LAN only; do not port-forward without VPN.
- Provider keys in NVS never returned.
- Set LOCAL_API_TOKEN via NVS for Bearer auth.

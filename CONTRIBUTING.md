# Contributing to NixRoute

Thanks for taking the time to contribute — whether it's a bug report, a feature
idea, a documentation fix, or a code change, it's all appreciated.

This document lays out the conventions and workflow so contributions are easy to
review and merge. Not sure where to start? Opening an issue is always a great
first step.

---

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Reporting Bugs](#reporting-bugs)
- [Suggesting Features](#suggesting-features)
- [Development Setup](#development-setup)
- [Making Changes](#making-changes)
- [Commit Message Convention](#commit-message-convention)
- [Testing](#testing)
- [Pull Request Checklist](#pull-request-checklist)

---

## Code of Conduct

Be kind, be constructive, and assume good intent. Harassment, spam, and
off-topic noise are not welcome. Treat maintainers and other contributors with
respect.

---

## Reporting Bugs

Found a bug? Please open an issue using the **Bug Report** template. A good bug
report saves everyone time — try to include:

1. **Description** — a clear, concise summary of what went wrong.
2. **Steps to reproduce** — the exact commands or clicks that trigger the bug.
3. **Expected vs. actual** — what you expected to happen vs. what happened.
4. **Environment** — board, ESP32 core version, firmware version, and how you
   flashed it.
5. **Logs** — serial output at `115200` baud (the `=== NixRoute vX ===` boot
   banner and any `heartbeat`/error lines are especially useful).
6. **Screenshots** — if the issue is visual (dashboard, login page, etc.).

> **Security-sensitive issues?** Do **not** post API keys, Wi-Fi passwords, or
> tokens in an issue. Contact the maintainer privately instead (see
> [Security Notes](README.md#security-notes)).

### Before opening a new issue

- Search existing issues (open **and** closed) to avoid duplicates.
- For hardware/connectivity problems, verify with `GET /health` first and share
  the JSON response (it contains heap, RSSI, and provider metrics).

---

## Suggesting Features

Have an idea? Open an issue using the **Feature Request** template and describe:

- The **problem** it solves (not just the feature itself).
- A **proposed solution** and how it fits the existing architecture.
- Any **alternatives** you considered.

Keep in mind the target hardware is an ESP32 (520 KB SRAM, 4 MB flash), so
proposals that are memory-conscious and offline-friendly are preferred.

---

## Development Setup

```bash
# 1. Add the ESP32 board package
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

# 2. Install ArduinoJson
arduino-cli lib install ArduinoJson

# 3. Compile the firmware
arduino-cli compile --fqbn esp32:esp32:esp32 firmware_arduino/esp32_router/esp32_router.ino
```

The firmware is split into two files:

| File | Purpose |
|---|---|
| `firmware_arduino/esp32_router/esp32_router.ino` | HTTP server, routing, failover, NVS, Wi-Fi |
| `firmware_arduino/esp32_router/dashboard_html.h` | Dashboard SPA (HTML/CSS/JS in PROGMEM) |

---

## Making Changes

1. **Fork** the repository and clone your fork.
2. Create a branch from `master` with a short, descriptive name:

   ```bash
   git checkout -b fix/provider-failover
   ```

3. Make focused, minimal changes. One logical change per commit.
4. Follow the existing code style (see below).
5. Compile before pushing — uncommitted code that doesn't build slows review.
6. Push and open a **Pull Request** against `master`.

### Code style

- **C++/Arduino** — match the surrounding code: 2-space indent, `String` for
  dynamic text, `Preferences` for persistence, and `JsonDocument` (ArduinoJson 7)
  for JSON. Keep heap usage in mind (no full-body buffering, 8 KB request cap).
- **Dashboard** — plain HTML/CSS/JS, zero external dependencies, dark theme using
  the brand palette (navy `#0c1a30`, cyan `#00a8b5`). Keep the SPA offline-ready.
- **Docs** — write in English, keep tables tidy, and update the README when you
  change public behavior or API endpoints.

---

## Commit Message Convention

This repo uses [Conventional Commits](https://www.conventionalcommits.org/).
Prefix each commit with a type, optionally scoped:

| Type | Use for |
|---|---|
| `feat` | a new feature |
| `fix` | a bug fix |
| `docs` | documentation-only changes |
| `style` | formatting/whitespace (no behavior change) |
| `refactor` | code change that neither fixes a bug nor adds a feature |
| `test` | adding or fixing tests |
| `chore` | build/tooling/maintenance |

Examples:

```
feat(core): add round-robin routing across providers
fix(core): cast tolower to char — prevent ASCII-code provider ids
docs: make build instructions port-agnostic
```

---

## Testing

Host-side smoke tests live in `tests/scripts/` and require no hardware beyond a
reachable device:

```bash
python tests/scripts/test_health.py --host <esp32-ip>
python tests/scripts/test_openai_compat.py --host <esp32-ip> --model <provider>/<model>
python tests/scripts/test_admin_api.py --host <esp32-ip> --password 123456
```

If you add or change an endpoint, add or update a smoke test to match.

---

## Pull Request Checklist

Before opening a PR, confirm:

- [ ] The firmware compiles cleanly (`arduino-cli compile --fqbn esp32:esp32:esp32 …`).
- [ ] Changes follow the code style and memory-safety guidelines above.
- [ ] Public behavior/API changes are reflected in `README.md`.
- [ ] Relevant tests were added or updated.
- [ ] Commit messages follow the convention above.
- [ ] No secrets (API keys, Wi-Fi passwords, tokens) are committed.

---

## License

By contributing, you agree that your contributions will be licensed under the
project's [MIT License](LICENSE).

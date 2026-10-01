# cyd-scanner

Counter-surveillance firmware for detecting **Flock Safety cameras** and similar
surveillance hardware (ALPR cameras, covert wireless cameras, tracking beacons) by
their RF signatures — built on the **ESP32-2432S028R "CYD"** touchscreen board and
an **ESP32-C5 DevKit** radio co-processor.

> **Purpose & ethics.** This is a *passive, defensive, privacy-research* tool. It
> only listens to broadcast RF that any receiver can already hear, and classifies
> devices from their public identifiers and signal metadata. It does **not** jam,
> deauthenticate, inject, or otherwise interfere with any device. Use it lawfully
> and responsibly.

> **On privacy.** This tool exists to expose *surveillance devices* — machines that
> collect data on everyone without consent. Those devices have no privacy interest this
> project is obligated to respect; revealing their uniqueness is the point, and no lawful
> device-fingerprinting capability here is limited for a machine's sake. The project's
> privacy concern is **human**, and one-directional: captured data can incidentally include
> identifiers of bystanders' devices (phone, wearable, etc.) alongside GPS and timestamps.
> **Such data must never be committed to this public repository.** Keep real captures on your
> own storage; the repo ships only code, device signatures, and synthetic examples.

## How it works

The scanner passively monitors the RF environment and matches observed devices
against signatures of known surveillance equipment:

- **Wi-Fi** — APs across 2.4 GHz and **5 GHz** (ESP32-C5), matched on MAC OUI ranges
  and SSID patterns.
- **Bluetooth LE** — advertising devices, matched on name patterns (service-UUID /
  company-ID matching planned).
- **802.15.4** — Zigbee / Thread devices (ESP32-C5, planned).

Each detection is scored against a **signature database** on the SD card (weighted OUI +
name rules → **suspect / likely / confirmed** tiers). Results are shown on the CYD's
portrait touchscreen — a status bar (time, GPS, connection), per-band counts, and the
strongest devices sorted threat-first — and logged to microSD with time and GPS.

A **phone web app** (Web Bluetooth) pairs over BLE to sync the phone's time and GPS into
the device, view live counts/threats, reload the signature DB, and download the log.

## Hardware

| Board | Role | Radios | Notes |
|-------|------|--------|-------|
| **ESP32-2432S028R (CYD)** | Host, touchscreen UI, storage | Wi-Fi 2.4 GHz b/g/n, BT Classic + BLE 4.2 | 2.8" 240×320 ILI9341/ST7789 TFT, XPT2046 resistive touch, microSD, RGB LED (speaker header unused → GPIO26 free) |
| **ESP32-C5 DevKit** (DOIT ESPC5-32) | Radio co-processor | Wi-Fi 6 dual-band 2.4 **+ 5 GHz**, BLE 5.0, IEEE 802.15.4 (Zigbee/Thread) | 32-bit RISC-V @240 MHz, 29 GPIO, USB-Serial/JTAG |

The two boards are linked over a serial (UART) connection: the CYD drives the UI
and 2.4 GHz scanning, while the C5 extends coverage into 5 GHz and 802.15.4 — bands
the CYD's original ESP32 cannot see.

### Reference material

All datasheets, pinout diagrams, and board images are kept under
[`hardware/`](hardware/):

```
hardware/
├── PINOUT.md                         # ← quick-access pin tables (start here)
└── boards/
    ├── cyd_esp32-2432S028r/
    │   └── pinout/                    # CYD pinout diagram
    └── esp32-c5-devkit/
        ├── datasheets/               # ESP32-C5 datasheet (PDF)
        └── pinout/                   # ESP32-C5 pinout diagram
```

**[hardware/PINOUT.md](hardware/PINOUT.md)** is the consolidated pin reference for
both boards (display, touch, SD, free GPIO, and the inter-board link).

## Building

This is a [PlatformIO](https://platformio.org/) project with **two environments** —
one per board — sharing a UART protocol library. Build and flash each board on its own:

```bash
pio run -e cyd -t upload -t monitor   # ESP32-2432S028R (CYD)
pio run -e c5  -t upload -t monitor   # ESP32-C5 DevKit
pio run                               # build both
```

Sources are split per board and filtered so each env compiles only its own code:

```
src/cyd/            -> [env:cyd]  board jczn_2432s028r
src/c5/             -> [env:c5]   board esp32-c5-devkitc-1
lib/link_protocol/  -> shared UART message protocol (compiled into both)
```

Both use the pioarduino espressif32 platform + Arduino framework (see
[platformio.ini](platformio.ini)).

## Status

Working end to end on hardware:

- ✅ Both boards bring-up: CYD display + calibrated touch + SD + RGB LED; C5 dual-band
  Wi-Fi + BLE.
- ✅ Inter-board UART link (framed protocol, synchronous request/response).
- ✅ C5 continuous async scanning (Wi-Fi 2.4 + 5 GHz, BLE) → detection table streamed to CYD.
- ✅ **Signature matching (Phase 1):** SD-based signature DB + weighted confidence scoring.
- ✅ Portrait UI with status bar; SD logging (time/GPS/score/tier).
- ✅ Phone web app over BLE: time/GPS sync, live status, BLE log download, DB reload;
  optional Wi-Fi SoftAP bulk download.

Next up: signature-DB tuning, promiscuous-mode Wi-Fi capture (probe/IE), BLE
service-UUID matching, 802.15.4, and the supervised capture/merge/correlation workflow.
See [docs/signature-matching.md](docs/signature-matching.md) and
[HANDOFF.md](HANDOFF.md).

## Repository layout

```
cyd-scanner/
├── CLAUDE.md          # architecture, gotchas, guidance for AI-assisted dev
├── HANDOFF.md         # current state + next steps (start here for a new session)
├── README.md          # this file
├── platformio.ini     # PlatformIO config (env:cyd, env:c5)
├── src/cyd/           # CYD host firmware (main, pins, phone, webshare, sigdb, touch)
├── src/c5/            # ESP32-C5 scanner firmware
├── lib/link_protocol/ # shared UART message protocol
├── webapp/            # phone control web app (Web Bluetooth, hosted on GitHub Pages)
├── docs/              # signature-matching research + design
├── hardware/          # datasheets, pinouts, PINOUT.md
└── .claude/           # agents + hooks for AI-assisted development
```

## Legal

Provided for lawful, defensive, educational, and privacy-research use only. You are
responsible for complying with all applicable laws and regulations governing radio
reception and monitoring in your jurisdiction.

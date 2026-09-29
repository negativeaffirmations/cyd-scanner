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

## How it works

The scanner passively monitors the RF environment and matches observed devices
against signatures of known surveillance equipment:

- **Wi-Fi** — APs and clients across 2.4 GHz (CYD) and **5 GHz** (ESP32-C5),
  matched on MAC OUI ranges, SSID patterns, and probe/beacon behavior.
- **Bluetooth LE** — advertising devices and beacons.
- **802.15.4** — Zigbee / Thread devices (ESP32-C5).

Detections are shown on the CYD's touchscreen, with signal strength for rough
proximity and (optionally) logging to microSD.

## Hardware

| Board | Role | Radios | Notes |
|-------|------|--------|-------|
| **ESP32-2432S028R (CYD)** | Host, touchscreen UI, storage | Wi-Fi 2.4 GHz b/g/n, BT Classic + BLE 4.2 | 2.8" 240×320 ILI9341/ST7789 TFT, XPT2046 resistive touch, microSD, RGB LED, speaker |
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

Early development. Hardware selected and documented; firmware in progress.

## Repository layout

```
cyd-scanner/
├── CLAUDE.md          # guidance for AI-assisted development
├── README.md          # this file
├── platformio.ini     # PlatformIO configuration
├── src/               # firmware sources
├── include/           # headers
├── lib/               # project libraries
├── test/              # tests
└── hardware/          # datasheets, pinouts, images, PINOUT.md
```

## Legal

Provided for lawful, defensive, educational, and privacy-research use only. You are
responsible for complying with all applicable laws and regulations governing radio
reception and monitoring in your jurisdiction.

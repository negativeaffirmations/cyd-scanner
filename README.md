# cyd-scanner

Counter-surveillance firmware for detecting **Flock Safety cameras** and similar
surveillance hardware (ALPR cameras, covert wireless cameras, tracking beacons) by
their RF signatures — built on the **ESP32-2432S028R "CYD"** touchscreen board and
an **ESP32-C5 DevKit** radio co-processor.

> **Purpose & ethics.** This is a *defensive, privacy-research* tool. It observes
> broadcast RF and classifies devices from their public identifiers and signal
> metadata, using ordinary scanning (including active BLE/Wi-Fi scans that solicit a
> device's own advertised name and service UUIDs — the same thing any phone does). It
> does **not** jam, deauthenticate, inject, spoof, flood, or otherwise interfere with
> or attack any device. Use it lawfully and responsibly.

## How it works

The scanner monitors the RF environment and matches observed devices
against signatures of known surveillance equipment:

- **Wi-Fi** — APs across 2.4 GHz and **5 GHz** (ESP32-C5), matched on MAC OUI ranges and
  SSID patterns, plus **promiscuous-mode capture** of client probe requests and 802.11 IE
  fingerprints.
- **Bluetooth LE** — active scan of advertising devices, matched on name patterns, **service
  UUIDs** (including extra advertised 16-bit UUIDs), and **manufacturer company IDs**.
- **802.15.4** — Zigbee / Thread presence (ESP32-C5, opt-in / default-off for BLE coexistence).
- **Behavioral detectors** — iBeacon, Find My / AirTag, Pwnagotchi, evil-twin APs, deauth-flood
  observation, and OpenDroneID / drone Remote-ID presence.

Each detection is scored against a **signature database** on the SD card (weighted OUI + name +
IE + BLE-UUID + company-ID rules → **suspect / likely / confirmed** tiers) and grouped into threat
**categories** (Flock, Axon, ALPR, Cam, Ring, Raven, Glass, Tracker, Drone, Deauth, …). Results
show on the CYD's portrait touchscreen — a status bar (time, GPS, connection), per-band counts, a
tappable category readout, and the strongest devices sorted threat-first — and are logged to
microSD (NDJSON, with time/GPS/score/tier) with a 512 MB auto-rotating cap.

A **phone web app** (Web Bluetooth) pairs over BLE to sync time + GPS, view a live tier-colored
detection list and category breakdown, start/stop scans, manage a whitelist, see a "following me"
follower view, reload the signature DB, and download / time-filter / **map** logs (Leaflet/OSM);
an optional Wi-Fi SoftAP handles bulk download.

## Hardware

| Board | Role | Radios | Notes |
|-------|------|--------|-------|
| **ESP32-2432S028R (CYD)** | Host, touchscreen UI, storage | Wi-Fi 2.4 GHz b/g/n, BT Classic + BLE 4.2 | 2.8" 240×320 ILI9341/ST7789 TFT, XPT2046 resistive touch, microSD, RGB LED (speaker header unused → GPIO26 free) |
| **ESP32-C5 DevKit** (DOIT ESPC5-32) | Radio co-processor | Wi-Fi 6 dual-band 2.4 **+ 5 GHz**, BLE 5.0, IEEE 802.15.4 (Zigbee/Thread) | 32-bit RISC-V @240 MHz, 29 GPIO, USB-Serial/JTAG |

The two boards are linked over a serial (UART) connection: the CYD drives the UI, SD logging,
and the phone link, while the **C5 does all the RF scanning** (Wi-Fi 2.4 + 5 GHz, BLE, and
802.15.4) — including the 5 GHz and 802.15.4 bands the CYD's original ESP32 cannot see.

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

- ✅ Both boards bring-up; inter-board UART link (framed protocol, synchronous request/response).
- ✅ C5 continuous scanning (Wi-Fi 2.4 + 5 GHz AP scan, promiscuous probe/IE capture, active BLE,
  opt-in 802.15.4) → shared detection table streamed to the CYD.
- ✅ **Signature matching (Phases 1–3):** SD signature DB + weighted scoring across OUI, device
  name, 802.11 IE fingerprint, BLE service UUID (multiple), and company ID → suspect/likely/confirmed.
- ✅ **Behavioral detectors:** iBeacon / Find My / Pwnagotchi / evil-twin / deauth-flood / drone
  Remote-ID presence, raising threat tiers + live alerts.
- ✅ **Threat categories** with a tappable on-device readout + drill-down, mirrored in the web app.
- ✅ **"Following me" (Phase 5)** spatial/temporal correlation + user whitelist.
- ✅ Portrait UI (BOOT + touch); NDJSON SD logging with time/GPS and 512 MB auto-rotation.
- ✅ Phone web app over BLE: time/GPS sync, live list + categories, scan control, whitelist,
  follower view, time-filtered log download, wardriving map; optional Wi-Fi SoftAP bulk download.

Next up: full ASTM F3411 drone Remote-ID **decode** (operator/drone location, not just presence),
and the supervised capture → merge → offline-correlation workflow. See
[docs/signature-matching.md](docs/signature-matching.md) and [HANDOFF.md](HANDOFF.md).

## Repository layout

```
cyd-scanner/
├── CLAUDE.md          # architecture, gotchas, guidance for AI-assisted dev
├── HANDOFF.md         # current state + next steps (start here for a new session)
├── README.md          # this file
├── platformio.ini     # PlatformIO config (env:cyd, env:c5)
├── src/cyd/           # CYD host firmware (main, pins, phone, webshare, sigdb, whitelist, logfilter, touch)
├── src/c5/            # ESP32-C5 scanner firmware (main, promisc, ieee154)
├── lib/link_protocol/ # shared UART message protocol
├── webapp/            # phone control web app (Web Bluetooth, GitHub Pages); styles in webapp/scss/ (SCSS → style.css)
├── docs/              # signature-matching research + design
├── hardware/          # datasheets, pinouts, PINOUT.md
└── .claude/           # agents + hooks for AI-assisted development
```

## Legal

Provided for lawful, defensive, educational, and privacy-research use only. You are
responsible for complying with all applicable laws and regulations governing radio
reception and monitoring in your jurisdiction.

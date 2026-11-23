# COINTEL-FLO

Counter-surveillance scanners that detect **Flock Safety cameras** and similar surveillance hardware (ALPR cameras,
covert wireless cameras, tracking beacons, drones) by their RF signatures.

There are two hardware builds that run the same scanning firmware and phone app:

- **CYD build**: an **ESP32-2432S028R "CYD"** touchscreen host + an **ESP32-C5 DevKit** radio co-processor.
- **Mini build**: a screenless pocket unit, a **Seeed XIAO ESP32-S3 Sense** host + a **Seeed XIAO ESP32-C5**
  scanner, with an RGB status LED and a real-time clock. It's driven entirely from the phone app.

> **Purpose & ethics.** This is a *defensive, privacy-research* tool. It observes
> broadcast RF and classifies devices from their public identifiers and signal
> metadata, using ordinary scanning (including active BLE/Wi-Fi scans that solicit a
> device's own advertised name and service UUIDs — the same thing any phone does). It
> does **not** jam, deauthenticate, inject, spoof, flood, or otherwise interfere with
> or attack any device. Use it lawfully and responsibly.
>
> **Captured data stays local.** Scan logs can pair bystanders' device identifiers with GPS +
> timestamps — and with the **onboard GPS** fitted the device self-geolocates even with no
> phone connected, so background-scan logs may carry real positions. Keep real captures on
> your own storage; **never commit them to this public repo** (it ships only code, device
> signatures, and synthetic examples).

## How it works

The scanner monitors the RF environment and matches observed devices
against signatures of known surveillance equipment:

- **Wi-Fi** — APs across 2.4 GHz and **5 GHz** (ESP32-C5), matched on MAC OUI ranges and
  SSID patterns, plus **promiscuous-mode capture** of client probe requests, 802.11 IE
  fingerprints and clients already joined to an AP.
- **Bluetooth LE** — active scan of advertising devices, matched on name patterns, **service
  UUIDs** (including extra advertised 16-bit UUIDs), and **manufacturer company IDs**.
- **802.15.4** — Zigbee / Thread presence (ESP32-C5, opt-in / default-off for BLE coexistence).
- **Behavioral detectors** — iBeacon, Find My / AirTag, Pwnagotchi, evil-twin APs, deauth-flood
  observation (it names the sender, and ignores a router simply dropping one of its own clients),
  and drone **Remote-ID** (full decode: drone + operator position, height, speed).

Each detection is scored against a **signature database** on the SD card (weighted OUI + name +
IE + BLE-UUID + company-ID rules → **suspect / likely / confirmed** tiers) and grouped into threat
**categories** (Flock, Axon, ALPR, Cam, Ring, Raven, Glass, Tracker, Drone, Deauth, Flipper, Skim).
Results are logged to microSD (NDJSON, with time/GPS/score/tier) with a 512 MB auto-rotating cap.
On the CYD build they also show on the portrait touchscreen: a status bar (time, GPS, battery,
connection), per-band counts, a tappable category readout, and the strongest devices sorted
threat-first. The mini build shows its state on an RGB LED.

A **phone app** pairs over BLE (authenticated + encrypted) to sync time + GPS, view a live tier-colored
detection list and category breakdown, start/stop scans, manage a whitelist, see a "following me"
follower view, raise phone notifications, reload the signature DB, browse the SD card, and download /
time-filter / **map** logs (Leaflet/OSM). It runs as a web app (Web Bluetooth, Chrome on Android) or as
the Android app below. An optional Wi-Fi SoftAP handles bulk download.

Several scanners can work as a **fleet** over ESP-NOW: one leader splits the bands between the units,
merges what they all see, and collects their logs. Logs can also be uploaded, or streamed live, to a
self-hosted analysis server that the phone app pairs with by QR code.

## Hardware

Both builds pair a host board (UI / storage / phone link) with an **ESP32-C5** that does all the RF
scanning over a UART link. The C5 adds **5 GHz Wi-Fi 6** and **802.15.4**, which the hosts' radios
can't see. Both carry a **GY-NEO6MV2 GPS** on the C5 (position + time when no phone is connected)
and a **battery-sense divider** on the host.

**Full pin reference for both builds: [hardware/PINOUT.md](hardware/PINOUT.md)** (CYD §1–5, mini §6).
The tables below are a quick summary of what this project wires; PINOUT.md is authoritative.

### CYD build

| Board | Role | Radios | Notes |
|-------|------|--------|-------|
| **ESP32-2432S028R (CYD)** | Host, touchscreen UI, storage | Wi-Fi 2.4 GHz b/g/n, BT Classic + BLE 4.2 | 2.8" 240×320 ILI9341/ST7789 TFT, XPT2046 resistive touch, microSD, RGB LED |
| **ESP32-C5 DevKit** (DOIT ESPC5-32) | Radio co-processor | Wi-Fi 6 dual-band 2.4 **+ 5 GHz**, BLE 5.0, IEEE 802.15.4 | 32-bit RISC-V @240 MHz, USB-Serial/JTAG |

**Pinout (CYD):**

| Function | CYD GPIO |
|----------|----------|
| Display (VSPI) | MOSI 13 · MISO 12 · SCLK 14 · CS 15 · DC 2 · backlight 21 |
| Touch (XPT2046, own bus) | CLK 25 · CS 33 · MOSI 32 · MISO 39 · IRQ 36 |
| microSD | CS 5 · MOSI 23 · SCK 18 · MISO 19 |
| RGB status LED (onboard, active low) | R 4 · G 16 · B 17 |
| Link to the C5 | TX **22** → C5 GPIO4 · RX **27** ← C5 GPIO5 · GND |
| Battery sense | **35** (input-only ADC1_CH7), 100 kΩ/100 kΩ divider off the switched battery |
| BOOT button | 0 |
| Free | 26 (no speaker fitted), serial header 1/3 |

**Pinout (ESP32-C5 DevKit):**

| Function | C5 GPIO |
|----------|---------|
| Link to the CYD | RX **4** ← CYD 22 · TX **5** → CYD 27 · GND |
| GPS (NEO-6M TXD, 9600 baud, receive-only) | RX **15** (UART0); GPIO23 is the unused TX |
| USB console / flashing | native USB port (USB-Serial/JTAG, 13/14) |

**Power:** 5000 mAh 3.7 V LiPo → TP4056 Type-C charger → master switch **SW1** → 1.5 A boost
(3.7→5 V). The 5 V output feeds the **CYD VIN** and the **C5 5V**.

> ⚠️ **Feed 5 V only, one source at a time.** Each board keeps its own 3.3 V regulator — never
> tie the two 3.3 V rails together. The two boards share one 5 V rail, so a USB cable plugged into
> either board also powers the other: turn **SW1** off before plugging in any programming USB, and
> plug in only one board's USB at a time.
>
> **Optional, handy for development: a second switch (SW2) between the CYD's 5 V and the C5's 5V pin.**
> Opening it disconnects the two boards' 5 V, so you can plug USB into **both** boards and flash or
> monitor them at the same time. Close it again for battery use. It isn't needed for normal use and
> isn't drawn in the wiring diagram.
> [hardware/PINOUT.md §4](hardware/PINOUT.md#4-power-subsystem--on-board-battery-current-rig) has its wiring.

**Full pinout + diagrams:**
- Pin tables: [hardware/PINOUT.md §1–5](hardware/PINOUT.md#1-esp32-2432s028r-cyd-cheap-yellow-display)
  (power wiring: [§4](hardware/PINOUT.md#4-power-subsystem--on-board-battery-current-rig))
- Wiring diagram: [`cyd-scanner_wiring_diagram_v2.png`](hardware/diagrams/cyd-scanner_wiring_diagram_v2.png)
  (labeled source [`cyd-scanner_diagram_v2_labeled.svg`](hardware/diagrams/cyd-scanner_diagram_v2_labeled.svg))
- Board pinouts: [CYD](hardware/boards/cyd_esp32-2432S028r/pinout/cyd_esp32-2432S028r_pinout-1.png) ·
  [ESP32-C5 DevKit](hardware/boards/esp32-c5-devkit/pinout/esp32-c5-devkit_pinout.jpg)
  ([datasheet](hardware/boards/esp32-c5-devkit/datasheets/esp32-c5_datasheet_en.pdf))

### Mini build

| Board | Role | Radios | Notes |
|-------|------|--------|-------|
| **Seeed XIAO ESP32-S3 Sense** | Host, storage, phone link (no screen) | Wi-Fi 2.4 GHz, BLE 5.0 | 8 MB PSRAM, onboard microSD (Sense board), native USB-C |
| **Seeed XIAO ESP32-C5** | Radio co-processor | Wi-Fi 6 dual-band 2.4 **+ 5 GHz**, BLE 5, IEEE 802.15.4 | needs a **dual-band 2.4/5 GHz antenna** (the stock one is 2.4 GHz only) |
| **DS3231 RTC** | Keeps the clock with no phone or GPS | — | I2C 0x68; fit a LIR2032, or remove the module's charge resistor for a CR2032 |
| **5 mm RGB LED** (common cathode) | Status light | — | green = link up, red = link down, blinking red = alert, magenta = follower, threat colours, blue = fleet |

**Pinout (XIAO ESP32-S3 Sense, host):**

| Pad | GPIO | Use |
|-----|------|-----|
| D0 | 1 | RGB LED **red** (470 Ω) |
| D1 | 2 | Battery sense (100 kΩ/100 kΩ divider off the switched battery) |
| D2 | 3 | RGB LED **green** (100 Ω) |
| D3 | 4 | RGB LED **blue** (100 Ω) |
| D4 | 5 | DS3231 SDA |
| D5 | 6 | DS3231 SCL |
| D6 | 43 | Link TX → C5 D5 |
| D7 | 44 | Link RX ← C5 D4 |
| D8 / D9 / D10, — | 7 / 8 / 9, 21 | microSD SCK / MISO / MOSI, CS (onboard, no wiring) |
| 3V3 / GND | — | DS3231 + GPS power; RGB LED common cathode → GND |

**Pinout (XIAO ESP32-C5, scanner):**

| Pad | GPIO | Use |
|-----|------|-----|
| D4 | 23 | Link TX → S3 D7 |
| D5 | 24 | Link RX ← S3 D6 |
| D0 | 1 | GPS RX ← NEO-6M TXD (receive-only) |

> ⚠️ **The link crosses:** S3 D6 → C5 **D5**, S3 D7 ← C5 **D4**. Wiring it straight by pad position
> (D6→D4) leaves the link dead.

**Power:** one LiPo cell → TP4056 (charge via its own USB-C) → switch → **both XIAOs' BAT+ pads**, each
board on its own regulator; the DS3231 and GPS run from the S3's 3V3. No boost converter. **Don't feed
the C5 from the S3's 3V3** (it won't boot that way), **never plug in both XIAOs' USB at once**, and keep
the switch on whenever any USB is plugged in (the C5 has no other supply).

**Full pinout + diagrams:**
- Pin tables: [hardware/PINOUT.md §6](hardware/PINOUT.md#6-mini-build--screenless-xiao-sibling-xiao-s3-host--xiao-c5-scanner--ds3231)
  · net-by-net wiring table: [docs/mini-build.md §2.6](docs/mini-build.md#26-complete-device-wiring-pinout)
  · staged bring-up test: [docs/mini-build-bringup.md](docs/mini-build-bringup.md)
- Wiring diagram: [`mini-build-rgb_wiring_diagram_labeled.png`](hardware/diagrams/mini-build-rgb_wiring_diagram_labeled.png)
  (source [`mini-build-rgb_wiring_diagram_labeled.svg`](hardware/diagrams/mini-build-rgb_wiring_diagram_labeled.svg))
- Board pinouts: [XIAO ESP32-S3 Sense](hardware/boards/xiao_esp32-s3_sense/pinout/xiao_esp32-s3_sense_pinout.jpg) ·
  [XIAO ESP32-C5](hardware/boards/xiao_esp32-c5/pinout/xiao_esp32-c5_pinout.png)

### Reference material

All datasheets, pinout diagrams, wiring diagrams and board images are kept under [`hardware/`](hardware/):

```
hardware/
├── PINOUT.md                  # ← pin tables for both builds (start here)
├── diagrams/                  # wiring diagrams (CYD v2, mini RGB)
└── boards/
    ├── cyd_esp32-2432S028r/   # CYD pinout + datasheets
    ├── esp32-c5-devkit/       # ESP32-C5 DevKit pinout, datasheet, images
    ├── xiao_esp32-s3_sense/   # XIAO ESP32-S3 Sense pinout
    └── xiao_esp32-c5/         # XIAO ESP32-C5 pinout
```

## Building

This is a [PlatformIO](https://platformio.org/) project with **one environment per board**, all sharing
a UART link protocol library and (for the two hosts) one host core. Each board is flashed on its own,
and both boards of a build must run the same link protocol version (flash them together after a
protocol change):

```bash
pio run -e cyd         -t upload   # CYD build: host (ESP32-2432S028R)
pio run -e c5          -t upload   # CYD build: scanner (ESP32-C5 DevKit)
pio run -e xiao_s3_rgb -t upload   # mini build: host (XIAO ESP32-S3 Sense, RGB LED)
pio run -e xiao_c5     -t upload   # mini build: scanner (XIAO ESP32-C5)
```

```
src/cyd/            -> [env:cyd]          board jczn_2432s028r (screen, touch, BOOT button)
src/xiao_s3/        -> [env:xiao_s3_rgb]  board seeed_xiao_esp32s3 (headless, RGB LED)
src/host_common/    -> shared host core (scanning pipeline, logging, phone link, fleet), in both hosts
src/c5/             -> [env:c5] / [env:xiao_c5]  the same scanner firmware for both C5 boards
lib/link_protocol/  -> shared UART message protocol (compiled into every env)
```

All envs use the pioarduino espressif32 platform + Arduino framework (see [platformio.ini](platformio.ini)).
Flashing notes: the C5 DevKit flashes from its **native USB** port, not the "UART" port; the XIAO S3 and
C5 share a USB ID (`303A:1001`), so identify them with `esptool chip-id` before uploading. Details in
[CLAUDE.md](CLAUDE.md) and [hardware/PINOUT.md §6f](hardware/PINOUT.md#6f-flashing-the-xiao-boards).

## Android app (COINTEL-FLO)

The phone web app is also built as an Android app: the same web app in a native shell ([Capacitor](https://capacitorjs.com/)),
with native Bluetooth instead of Web Bluetooth, so it needs no browser flags, opens without signal, and keeps
alerts running with the screen off. Every push that changes the app builds a signed APK and publishes it as a
[GitHub release](../../releases) tagged `app-0.1.<n>`.

**Install and update with [Obtainium](https://obtainium.imranr.dev/)** (in F-Droid):

1. In Obtainium, tap **Add App** and enter this repository's URL.
2. Set **Filter release titles by regular expression** to `^COINTEL-FLO build` and **Filter APKs by regular
   expression** to `cointel-flo-.*\.apk`.
3. Tap **Add**, then **Install**, and allow Obtainium to install apps when Android asks.

Later builds show up as updates in Obtainium. Each scanner's phone key has to be entered in the app once (CYD:
Settings → Phone key, then **Scan QR** in the app; mini: its pairing QR), because the browser stores keys in a form
that can't be exported. Details, design and progress: [docs/android-app.md](docs/android-app.md). The code is in
[android-app/](android-app/).

## Status

Working end to end on hardware, on both builds:

- ✅ Inter-board UART link; C5 continuous scanning (Wi-Fi 2.4 + 5 GHz AP scan, promiscuous capture,
  active BLE, opt-in 802.15.4) streamed to the host.
- ✅ **Signature matching:** SD signature DB + weighted scoring across OUI, device name, 802.11 IE
  fingerprint, BLE service UUIDs and company ID → suspect/likely/confirmed, grouped into threat categories.
- ✅ **Behavioral detectors** (iBeacon / Find My / Pwnagotchi / evil twin / deauth with sender detail) and
  full drone **Remote-ID** decode with drone + operator positions on the map.
- ✅ **"Following me"** spatial/temporal correlation + user whitelist.
- ✅ Onboard GPS (position + time), RTC on the mini, battery level; NDJSON SD logging with 512 MB rotation.
- ✅ Phone app (web + Android): authenticated, encrypted BLE link; live list + categories, scan control,
  notifications (also in the background on Android), SD file explorer, log download with a phone-side cache,
  `.cflo` scan archives, wardriving map.
- ✅ **Fleet mode:** scanners share the work over ESP-NOW, merge detections and forward their logs to the leader.
- ✅ Upload and live streaming to a self-hosted analysis server, and reading its device history back.

Current state and next steps: [HANDOFF.md](HANDOFF.md). Signature design: [docs/signature-matching.md](docs/signature-matching.md).

## Tools

- [`tools/label-printer/`](tools/label-printer/) — prints a scanner's label (pairing QR + name + scanner id) on a
  Supvan / Katasymbol T50M Pro over USB.
- [`tools/fleet-sniff/`](tools/fleet-sniff/) — checks what fleet mode puts on the air.
- [`tools/odid-tx/`](tools/odid-tx/) — a bench Remote-ID transmitter for testing the drone decoder.

## Repository layout

```
cyd-scanner/
├── CLAUDE.md          # architecture, gotchas, guidance for AI-assisted dev
├── HANDOFF.md         # current state + next steps (start here for a new session)
├── README.md          # this file
├── platformio.ini     # PlatformIO envs (cyd, c5, xiao_s3_rgb, xiao_c5, + debug/bench envs)
├── src/cyd/           # CYD host: screen UI, touch, board support
├── src/xiao_s3/       # mini host: headless board support, RGB LED, RTC
├── src/host_common/   # shared host core: scan pipeline, sigdb, logging, phone link, fleet
├── src/c5/            # ESP32-C5 scanner firmware (both C5 boards)
├── lib/link_protocol/ # shared UART message protocol
├── android-app/       # the web app as an Android app (Capacitor shell + native Bluetooth stand-in), see docs/android-app.md
├── webapp/            # phone web app (Web Bluetooth, GitHub Pages); JS in webapp/js/ (ES modules, no build), styles in webapp/scss/ (SCSS → style.css)
├── tools/             # label printer, fleet sniffer, Remote-ID bench transmitter
├── docs/              # design docs (signature matching, mini build, fleet mode, BLE auth, Android app, …)
├── hardware/          # datasheets, pinouts, wiring diagrams, PINOUT.md
└── .claude/           # agents + hooks for AI-assisted development
```

## Legal

Provided for lawful, defensive, educational, and privacy-research use only. You are
responsible for complying with all applicable laws and regulations governing radio
reception and monitoring in your jurisdiction.

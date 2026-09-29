# CLAUDE.md — cyd-scanner

Guidance for Claude Code when working in this repository.

## What this project is

**cyd-scanner** is counter-surveillance firmware for detecting Flock Safety
cameras and similar surveillance hardware (ALPR cameras, covert wireless cameras,
tracking beacons) by their RF signatures. It runs on a two-board system:

- **ESP32-2432S028R "CYD"** — the host: touchscreen UI, 2.4 GHz Wi-Fi/BLE scanning,
  storage, and display of detections.
- **ESP32-C5 DevKit** — a radio co-processor adding **5 GHz Wi-Fi 6** and
  **802.15.4 (Zigbee/Thread)** coverage the CYD's ESP32 cannot provide.

The goal is a portable, self-contained scanner that passively observes nearby RF
(Wi-Fi APs/clients, BLE advertisements, 802.15.4 devices), matches them against
signatures of known surveillance gear (e.g. MAC OUI ranges, SSID patterns, probe
behavior), and surfaces detections on the CYD's touchscreen.

### Scope / ethics

This is a **defensive, passive, privacy-research** tool. It observes broadcast RF
that any receiver can hear. Keep it that way:
- **Passive detection only** — no jamming, deauth, injection, or any active
  interference with other devices' operation.
- No capture or storage of third-party payload content beyond what's needed to
  classify a device (identifiers, signal metadata).
- When in doubt about whether a feature crosses from "detect" into "attack/DoS,"
  stop and ask.

## Hardware

Two boards. Full pin tables and quick reference:
**[hardware/PINOUT.md](hardware/PINOUT.md)** — read this before writing any pin-level code.

| Board                 | Role            | Radios                                              |
|-----------------------|-----------------|-----------------------------------------------------|
| ESP32-2432S028R (CYD) | Host + UI       | Wi-Fi 2.4 GHz b/g/n, BT Classic + BLE 4.2           |
| ESP32-C5 DevKit       | Radio co-proc   | Wi-Fi 6 2.4 **+ 5 GHz**, BLE 5.0, 802.15.4          |

Datasheets, pinout diagrams, and board images live under
[hardware/boards/](hardware/boards/). When new reference material is added there,
update [hardware/PINOUT.md](hardware/PINOUT.md) accordingly.

### Key pin facts to remember
- **CYD display** (ILI9341/ST7789 SPI): MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, backlight 21.
- **CYD touch** (XPT2046, separate bus): CLK 25, CS 33, MOSI 32, MISO 39, IRQ 36.
- **CYD SD**: CS 5, MOSI 23, SCK 18, MISO 19.
- **CYD free GPIO** for the C5 link / sensors: 22, 26, 27, 35 (input-only), serial header 1/3.
  GPIO26 is free because **no speaker is fitted** (the on-board audio amp header is unused).
- **ESP32-C5** console UART0 on 11/12; USB-Serial/JTAG on 13/14; strapping pins 8/9.
- ⚠️ **ESP32-C5 pins 25 & 26 are UNVERIFIED / possibly swapped** vs. online docs.
  Follow the numbers printed on the physical board (which the repo's edited pinout
  JPEG and PINOUT.md match), but **test before using** either pin. Prefer other GPIO
  for critical signals until confirmed. See the caveat in [hardware/PINOUT.md](hardware/PINOUT.md).

## Architecture: two boards, one project

Each board is a separate MCU with its own firmware binary, so each gets its **own
PlatformIO environment** — but both live in **one project/repo** because they share
a UART link protocol and are developed together.

- **CYD is the master.** It drives the UI, polls the C5, scores detections against
  the signature DB, logs to SD, and hosts the phone link.
- **C5 is the scanning co-processor.** It runs Wi-Fi (2.4 + 5 GHz) / BLE scanning
  (802.15.4 planned) continuously and streams detections back to the CYD on request.
- **Phone link (implemented):** the CYD is a BLE GATT peripheral for a phone web app
  (time/GPS sync, log download, DB reload) and can raise an on-demand Wi-Fi SoftAP for
  bulk log download. See "Phone link + web app" below.

Planned layout:

```
src/
  cyd/            # CYD host firmware      → [env:cyd]
  c5/             # ESP32-C5 firmware      → [env:c5]
lib/
  link_protocol/  # shared UART message structs — compiled into BOTH envs
```

`build_src_filter` per env keeps each board from compiling the other's sources.
Anything under `lib/` is auto-included, so the shared protocol stays in sync by
construction.

## Build & tooling

This is a **PlatformIO** project (see [platformio.ini](platformio.ini)).

- `[env:cyd]` — board `jczn_2432s028r`, `build_src_filter = +<cyd/>`.
- `[env:c5]` — board `esp32-c5-devkitc-1` (verified against the installed pioarduino
  platform), `build_src_filter = +<c5/>`, `board_build.partitions = huge_app.csv`
  (Wi-Fi + BLE overflow the default 1.3MB app partition).
- Both use the pioarduino espressif32 platform + Arduino framework.

### Flashing the ESP32-C5 (important gotcha)

The C5 DevKit has **two** USB-C ports:
- **Native "USB" port** (enumerates as VID `303A:1001`, USB-Serial/JTAG) — **use this
  to flash.** Auto-download works with no button presses. Serial console also comes
  out here because the env sets `-D ARDUINO_USB_MODE=1 -D ARDUINO_USB_CDC_ON_BOOT=1`.
- **"UART" port** (external bridge, VID `2E3C:5740`) — **does NOT flash reliably**:
  its auto-reset is only half-wired, so esptool fails with "No serial data received"
  even after manual BOOT+RST. Only use it as a plain serial console (and then only
  with `CDC_ON_BOOT=0`).

The CYD flashes normally over its single CH340 micro-USB/USB-C port (auto-reset works;
the first upload attempt occasionally glitches — just retry). COM port numbers are
not stable across replugs; identify boards by USB VID:PID (CYD = CH340 `1A86:7523`,
C5 native = `303A:1001`).

Build/flash each board independently:

```bash
pio run -e cyd -t upload -t monitor   # build + flash + monitor the CYD
pio run -e c5  -t upload -t monitor   # build + flash + monitor the C5
pio run                               # build all envs
pio device monitor -e cyd             # serial monitor only
```

### Inter-board link (working — hard-won notes)

The CYD⇄C5 UART link (CYD GPIO22/27 ⇄ C5 GPIO4/5, GND) is validated end to end:
the CYD commands the C5, which scans dual-band Wi-Fi and streams `Detection` frames
back (5 GHz included). Things that matter, learned the hard way:

- **Wiring must be mechanically solid.** Loose dupont/JST contacts pass small frames
  (Ping) but drop the larger detection burst. The `LINK_MONITOR` build mode in both
  `main.cpp` (heartbeat + live loss counter on the CYD screen/LED) is the tool for
  hunting bad contacts — flip it on, reseat wires, watch loss go to 0%.
- **The protocol is strictly synchronous request/response.** The CYD sends ONE
  command, then waits for the C5's closing `Status(scanning=0)` before sending
  anything else. Do NOT interleave pings or a second command mid-scan — the C5
  blocks during `WiFi.scanNetworks()` and can't read, so extra commands pile up in
  its RX buffer and the two boards desync. (The C5 also flushes stale RX after a
  scan as a safety net.)
- **Scan timeout must exceed a full dual-band scan.** A 2.4+5 GHz scan can take
  ~10 s; the CYD's scan timeout is 20 s. (A future improvement is async/per-band
  scanning so the UI isn't blocked ~10 s per refresh.)
- **Bigger RX buffers**: both sides call `setRxBufferSize()` before `begin()` so a
  ~1.5 KB detection burst fits without overflow.
- **C5 link UART uses the XTAL clock** (`setClockSource(UART_CLK_SRC_XTAL)` before
  `begin()`) so Wi-Fi PLL activity can't shift its baud. Good practice on any ESP32
  UART that runs alongside Wi-Fi. (For the original-ESP32 CYD, the equivalent is
  `UART_CLK_SRC_REF_TICK` — relevant only once the CYD does its own Wi-Fi scanning.)
- Link runs at 115200 (see `LINK_BAUD` in `link_protocol.h`).

### Scanner architecture (current)

- **C5 = continuous async scanner.** It runs Wi-Fi dual-band scans asynchronously
  (`WiFi.scanNetworks(true)` polled from `loop()`, ~3 s gap between scans) and BLE
  continuously (NimBLE callback), merging both into a shared **detection table**
  (dedup by source+MAC, 30 s TTL, mutex-guarded because the BLE callback runs in a
  separate task). Scanning never blocks the link.
- **CYD = poller + UI + logger.** Every ~2 s it sends `StartScan`, the C5 instantly
  dumps its current table (fast, no blocking scan), and the CYD shows per-source
  counts (2.4 GHz / 5 GHz / BLE) + the strongest devices, sorted by RSSI.
- **SD logging (CYD).** First-seen devices (dedup by source+MAC for the session) are
  appended to `/scanlog.csv`:
  `epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,name,score,tier,signature`.
  `epoch` is LOCAL time once the phone has synced (it sends UTC + tz offset; 0 before);
  `lat`/`lon` fill once GPS is sent; `score`/`tier`/`signature` come from the signature DB.
  A one-time wipe (bump `LOG_GEN` in `main.cpp`) forces a fresh log after a schema change.
- SD is on its own HSPI bus (display=VSPI, link=UART1), so no bus contention. Touch is
  NOT used in the scanner build — it shares HSPI with the SD card, so an on-screen touch
  UI would need a software-SPI touch driver first. The physical BOOT button is used
  instead (press to show the web-app QR).

### Signature matching (Phase 1 — implemented)

- **`src/cyd/sigdb.*`** loads `/signatures.csv` from SD into bounded RAM (~4 KB; caps
  64 OUI + 48 name rules) and scores each detection. Weights sum across layers
  (OUI + device-name), best-per-layer, mapped to **suspect / likely / confirmed** tiers
  (thresholds in the CSV). Shared vendor OUIs (Espressif/Qualcomm) carry LOW weight so
  they only escalate when combined with an SSID/BLE-name hit — this is what suppresses
  false positives.
- The DB **self-seeds** to the card on first boot and is editable on the card or
  reloadable from the phone (no reflash). Missing/corrupt → compiled-in fallback (`db=fb`).
- Rules: `oui,<PREFIX>,<weight>,<srcmask>,<label>` and `exact|prefix|contains,<pattern>,…`;
  `thresholds,<suspect>,<likely>,<confirmed>`. `srcmask`: W/B/4/A. No on-device regex.
- Design + roadmap (Phases 2–6): [docs/signature-matching.md](docs/signature-matching.md).

### Display / UI (portrait)

- Rotation `0` (portrait 240×320). Top **status bar**: local time (left), abbreviated
  GPS (middle), connection icon (right: Bluetooth / Wi-Fi / circle-slash). Below it:
  link/SD/db line, per-band counts, a threats line, then detection rows sorted
  threat-tier-first then RSSI (tier colors: suspect=yellow, likely=orange, confirmed=red).
- **BOOT button** toggles a full-screen QR that links to the hosted web app.

### Phone link + web app (Web Bluetooth) — working

- **CYD BLE GATT peripheral** (`src/cyd/phone.*`, NimBLE) advertises as `CYD-Scanner`,
  service `9a1e0000-…` with characteristics:
  - `…0001` TIME (write) — `"utcEpoch;tzOffsetMinutes"` (bare epoch also accepted)
  - `…0002` GPS (write) — `"lat,lon"`
  - `…0003` CMD (write) — `"1"/"0"` Wi-Fi download on/off · `"R"` reload DB · `"L"` BLE log download
  - `…0004` STATUS (read/notify) — `key=val;…` incl. `link,w24,w5,ble,uniq,time,gps,dl,susp,lk,conf,db`
  - `…0005` LOGDATA (notify) — BLE log stream (`SIZE=<n>` header then raw chunks)
  - **Discovery gotcha (fixed):** NimBLE 2.x has scan response OFF by default, and the
    128-bit service UUID fills the adv packet, so the name overflows. We call
    `enableScanResponse(true)` + set the name in the scan response, and the web app
    filters by **service UUID** (not name). Without this the phone finds nothing.
- **Web app** `webapp/index.html` — hosted at
  **https://negativeaffirmations.github.io/cyd-scanner/webapp/** (GitHub Pages). MUST be
  HTTPS (Web Bluetooth + geolocation need a secure context); **Chrome on Android only**
  (no iOS Safari). Connects over BLE, syncs time+GPS, shows live counts/threat tiers,
  downloads the log, reloads the DB.
- **Log download — BLE (default):** `L` → the CYD streams `/scanlog.csv` over the LOGDATA
  characteristic; the app reassembles and saves the file. One button, stays in-app.
- **Log download — Wi-Fi (optional, for bulk):** `src/cyd/webshare.*` raises a SoftAP +
  HTTP server; the CYD screen shows a QR that is the Wi-Fi-join code until the phone joins,
  then switches to `http://192.168.4.1` so scanning opens the download page. Pauses scanning.
- **Browser limits worth remembering:** a web page cannot auto-join Wi-Fi, cannot fetch()
  `http://192.168.4.1` from the HTTPS app (mixed content), and loses the BLE connection if
  it navigates there — which is why bulk transfer uses a separate tab / the QR, and BLE is
  the default.
- Needs `board_build.partitions = huge_app.csv` (BLE + Wi-Fi + web server + TFT).

## Subagents (`.claude/agents/`)

A specialized agent suite is available (adapted from the owner's other firmware project):
- **orchestrator** — entry point for multi-step tasks; delegates to the others.
- **architect** — design/planning, board-split + flash/RAM trade-offs (read-only).
- **fixer** — implements C++ for CYD/C5 (read-write).
- **tester** — runs `pio run -e cyd`/`-e c5`, reports build + flash/RAM.
- **reviewer** — code quality, memory/concurrency, link-protocol correctness (read-only).
- **security** — enforces passive-only scope, captured-data privacy, attack surface (read-only).
- **hardware-docs** — datasheets/pinout images → pin tables & docs; keeps `pins.h` ↔ `PINOUT.md`.

## Reading datasheets (PDF)

The Read tool renders PDF pages via poppler (`pdftoppm`, installed). If Read reports
`pdftoppm is not installed` (stale PATH after a fresh install — fixed by restarting the
session), convert pages manually and Read the PNGs:
```bash
pdftoppm -png -r 150 -f <first> -l <last> "hardware/.../file.pdf" "<tmp>/pg"
pdftotext -layout "hardware/.../file.pdf" -   # text-only extraction
```
Don't commit rendered pages. The `hardware-docs` agent handles this end to end.

## Conventions

- Keep pin definitions centralized (a `pins.h` / config header per board), sourced
  from [hardware/PINOUT.md](hardware/PINOUT.md) — do not scatter magic GPIO numbers.
- Match the surrounding code's style; this is embedded C++/Arduino.
- Prefer non-blocking scanning loops; the UI must stay responsive.
- Log to the USB console UART, keep the inter-board link on a separate UART.

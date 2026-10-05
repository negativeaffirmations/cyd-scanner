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

The goal is a portable, self-contained scanner that observes nearby RF
(Wi-Fi APs/clients, BLE advertisements, 802.15.4 devices — via active scanning plus passive
promiscuous capture), matches them against
signatures of known surveillance gear (e.g. MAC OUI ranges, SSID patterns, probe
behavior), and surfaces detections on the CYD's touchscreen.

### Scope / ethics / privacy

This is a **defensive, privacy-research** tool. It observes and fingerprints nearby surveillance
RF, and to do that it **actively scans** — standard BLE/Wi-Fi scanning that solicits a response
(e.g. a BLE `SCAN_REQ` to pull a device's scan-response **name + service UUIDs**, active Wi-Fi
scanning) is expected and **required**: the device/name/UUID fingerprinting this project depends on
does not work without it. The line that stays off-limits is **interfering with or attacking** other
devices, not transmitting as such:
- **Don't attack or disrupt anything it observes** — no jamming, deauth, injection, spoofing,
  beacon/probe floods, or any other active interference with another device's operation. Ordinary
  scanning that a device answers *by design* is fine; anything that degrades, hijacks, spoofs, or
  denies service to a third-party device is not.
- **The tool's own radios transmit freely for its own function** — the CYD's BLE GATT phone link,
  the on-demand Wi-Fi SoftAP (log download), active scan requests, and (contemplated) a link so
  **multiple of the user's own scanner devices talk to each other** (share detections / correlate
  sightings across a small fleet).
- When in doubt whether a feature crosses from "scan / fingerprint / coordinate our own devices"
  into "interfere with / attack / DoS someone else's device," stop and ask.

This tool exists to expose *surveillance devices* — machines that
collect data on everyone without consent. Those devices have no privacy interest this
project is obligated to respect; revealing their uniqueness is the point, and no lawful
device-fingerprinting capability here is limited for a machine's sake. The project's
privacy concern is **human**, and one-directional: captured data can incidentally include
identifiers of bystanders' devices (phone, wearable, etc.) alongside GPS and timestamps.
**Such data must never be committed to this public repository.** Keep real captures on your
own storage; the repo ships only code, device signatures, and synthetic examples.

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
- **C5 is the scanning co-processor.** It runs Wi-Fi (2.4 + 5 GHz) AP scan, promiscuous
  probe/IE capture, active BLE scanning, and opt-in 802.15.4 continuously, streaming
  detections back to the CYD on request.
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

- **C5 = continuous async scanner.** It **time-slices the Wi-Fi radio** between two
  phases (see `src/c5/promisc.*`): an async dual-band AP scan (`WiFi.scanNetworks(true)`,
  2.4 + 5 GHz beacons) and a **passive promiscuous-mode capture window** (Phase 2) that
  hops the 2.4 GHz probe hotspots 1/6/11 (~200 ms dwell, ~3 s window) to catch Wi-Fi
  **client probe requests** + **beacon/probe-resp 802.11 IE fingerprints** — the client
  behavior `scanNetworks()` can't see. BLE runs continuously (NimBLE callback) alongside.
  All sources merge into a shared **detection table** (dedup by source+MAC, 30 s TTL,
  mutex-guarded because the BLE + promiscuous callbacks run in separate tasks; a captured
  IE fingerprint is preserved across scan refreshes). Scanning never blocks the link.
  Promiscuous capture itself is **receive-only** — it just monitors 802.11 frames (BLE scanning is
  active; see the Scope section).
- **C5 behavioral detectors (added in protocol v6; link now v7).** Alongside fingerprint capture the C5 decodes
  broadcast adverts/beacons and sets per-detection `DetFlags` bits (in `link_protocol.h`), all
  **receive-only**: `FLAG_BLE_IBEACON`, `FLAG_BLE_FINDMY` (Apple 0x004C mfr-data types 0x02 / 0x12),
  `FLAG_WIFI_PWNAGOTCHI` (beacon DE:AD:BE:EF IE / "pwnd"), `FLAG_WIFI_EVILTWIN` (same SSID from ≥2
  BSSIDs at **different encryption** — a small C5 SSID table, encryption-mismatch verdict to avoid
  roaming false-positives), and `FLAG_BLE_ODID` (OpenDroneID / Remote-ID **presence** — BLE 0xFFFA
  service data or the Wi-Fi vendor IE; full ASTM F3411 operator-location decode is a planned phase 2).
  It also **counts deauth/disassoc frames it OBSERVES** (never sends any) into a sliding window with
  a ~10 s sticky peak, and reports aggregate counts in `Status`: `deauth_recent`, `evil_count`,
  `rid_count`. `Detection.flags` is 16-bit as of v6. The active BLE scan also captures up to
  `SVC16_MAX` extra 16-bit service UUIDs per device in `Detection.svc16[]` (protocol v7), so the
  CYD can match a signature UUID even when it isn't the first advertised. (Signature matching still
  runs on the CYD.)
- **CYD = poller + UI + logger.** Every ~2 s it sends `StartScan`, the C5 instantly
  dumps its current table (fast, no blocking scan), and the CYD shows per-source
  counts (2.4 GHz / 5 GHz / BLE) + the strongest devices, sorted by RSSI. It drops its
  **own** BLE advertisement (`phone::ownMac()`, from `NimBLEDevice::getAddress()`) and the
  **connected phone's** BLE address (`phone::peerMac()`, from the GATT link) from the
  results — both captured at runtime, so device-agnostic. The phone filter is best-effort:
  phones use rotating random BLE addresses, so their scanned advertisements may not match
  the connection address.
- **SD logging (CYD) — per-session NDJSON files, two parallel streams.** The log is **NDJSON**
  (one JSON object per line, `logNewDetections` in `main.cpp`). There are **two independent streams**
  (each a `LogStream` with its own dedup set): a **foreground** stream for on-screen scan sessions
  (`/logs/sess-NNNNN.jsonl`, renamed to `/logs/YYYYMMDD-HHMMSS.jsonl` on time sync) and a dedicated
  **background** stream for the always-on background scan (`/logs/bg-NNNNN.jsonl` → `/logs/bg-…`).
  The headless background cycle (any screen but `SCR_SCAN`) logs to the bg stream; the on-screen scan
  logs to the fg stream. First-seen devices (dedup by source+MAC per session) are appended, one object
  each; optional keys omitted when blank:
  `{"epoch":…,"ms":…,"lat":…,"lon":…,"src":"…","mac":"…","rssi":…,"ch":…,"ie":"…","cid":"…","uuid":"…","pan":"…","name":"…","score":…,"tier":"…","sig":"…","wl":0|1,"flags":"…"}`.
  `src` is `2.4`/`5G`/`BLE`/`PRB`/`154`; `ie` the 8-hex IE fingerprint; `cid`/`uuid`/`pan` the BLE
  company ID / service UUID / 802.15.4 PAN; `name` JSON-escaped; `wl` 1 when whitelisted; **`flags`**
  is a `+`-joined list of behavioral-detector hits (`ibeacon`/`findmy`/`pwnagotchi`/`eviltwin`/`droneid`,
  omitted when none). Behavioral **EVENTS** are their own line shape with an `"evt"` discriminator,
  rate-limited to ~once/30 s per type: `{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"deauth","count":N}`
  and `{…,"evt":"eviltwin","count":N}`. `epoch` is LOCAL time once synced (0 before); `lat`/`lon`
  once GPS is sent; `score`/`tier`/`sig` from the signature DB (tier also floored by any flag hit).
  **Why NDJSON:** append-only (a torn write loses only the trailing line), self-describing (schema
  grows without breaking old parsers), JSON escaping kills the comma/formula-injection bug class.
  Old `.csv` sessions still list/read (readers branch by extension).
- **Log retention — 512 MB cap with auto-rotation.** `enforceLogCap()` keeps total `/logs/` size
  under `LOG_CAP_BYTES` (512 MB) by deleting the **oldest** files (by FS timestamp), never the live
  fg/bg/Explore-held/currently-streaming file. Runs at boot, on each session rotation, and on a
  ~10 min loop timer (suppressed during downloads).
- **Log download + time-filtered export.** Download (BLE + Wi-Fi) serves the **current** stream
  (`webshare::setLogPath`, kept in sync with the active screen). A **time-filtered export** streams
  only matching lines, filtered **on the CYD** (two-pass, never the whole 512 MB file): BLE CMD
  `T:<mode>[:min][:path]` and HTTP `/dlf?f=&mode=&arg=` — mode `0` = since boot (whole file), `1` =
  past 24 h, `2` = custom minutes. Before the phone syncs time it falls back to `ms`-since-boot.
  Filtering lives in `src/cyd/logfilter.h`.
- SD is on its own HSPI bus (display=VSPI, link=UART1), so no bus contention. Touch is
  NOT used in the scanner build — it shares HSPI with the SD card, so an on-screen touch
  UI would need a software-SPI touch driver first. The physical BOOT button drives a
  simple on-device **main menu** instead (tap = next item, hold = select).

### Signature matching (Phase 1 — implemented)

- **`src/cyd/sigdb.*`** loads `/signatures.csv` from SD into bounded RAM (caps **96 OUI** + 64 name
  rules) and scores each detection. Weights sum across layers (OUI + device-name), best-per-layer,
  mapped to **suspect / likely / confirmed** tiers (thresholds in the CSV). Shared vendor OUIs
  (Espressif/Qualcomm) carry LOW weight so they only escalate when combined with an SSID/BLE-name hit
  — this is what suppresses false positives. The roster ships a large device taxonomy ported (as
  data, GPL-3.0, attributed in [docs/references.md](docs/references.md)) from SquachWatch-CYD: Axon,
  ALPR, cameras (Ring/Verkada/Axis/Hikvision/Wyze/…), Tile/Samsung/Google trackers, Meta/Snap camera
  glasses, Flipper/Pineapple, OpenDroneID, etc.
- **Behavioral-flag tiers.** The C5's `DetFlags` (iBeacon/FindMy/Pwnagotchi/EvilTwin/ODID) raise a
  detection's **tier floor** on the CYD (`computeScores`): Find-My/Drone-RID → suspect,
  Pwnagotchi/Evil-twin → likely, iBeacon → label only. Deauth-flood + evil-twin also surface as
  live **alerts** (red banner + LED) and `evt` log lines.
- The DB **self-seeds** to the card on first boot and is editable on the card or
  reloadable from the phone (no reflash). Missing/corrupt → compiled-in fallback (`db=fb`).
- Rules: `oui,<PREFIX>,<weight>,<srcmask>,<label>`, `exact|prefix|contains,<pattern>,…`,
  `ie,<8-hex-fingerprint>,…` (Phase 2 — matches the C5's 802.11 IE hash),
  `bleuuid,<uuid>,…` (Phase 3 — matches a BLE service UUID, 16-bit or full 128-bit; matched
  against the primary `svc[]` AND the extra 16-bit UUIDs the active scan captures in
  `Detection.svc16[]`, protocol v7, so a signature UUID is caught even when not advertised first;
  the Flock GATT UUID is seeded), and `blecid,<hex>,…` (Phase 3 — matches a BLE
  manufacturer company ID); `thresholds,<suspect>,<likely>,<confirmed>`. `srcmask`:
  W/B/4/A (a probe-request detection also matches W/A rules). Weights sum across the
  OUI + name + IE + BLE-UUID + company-ID layers. No on-device regex.
- Design + roadmap (Phases 2–6): [docs/signature-matching.md](docs/signature-matching.md).

### Display / UI (portrait)

- Rotation `0` (portrait 240×320). Top **status bar**: local time (left), abbreviated
  GPS (middle), phone connection icon (Bluetooth / Wi-Fi / circle-slash) with the
  **CYD↔C5 link dot** to its right (solid green = link up, solid red = down). Below it:
  SD/db line, per-band counts (2.4 / 5G / BLE / PRB / unique), a threats line, then
  detection rows sorted threat-tier-first then RSSI (tier colors: suspect=yellow,
  likely=orange, confirmed=red).
- **Input: BOOT button + a touch overlay.** A short **tap** moves the highlight / selects a
  touched item; a long **hold** selects / returns toward the menu. The HOME screen has **one list
  button, Scan**, plus two **title-bar icon buttons** on the `HOME` line: a **cog (Settings)** upper-
  left and a **phone (Phone Link)** upper-right — the phone icon is **grey when no phone is on the GATT
  link, green when connected**, and tapping it opens the full-screen web-app QR (`< BACK` top bar).
  Both icons stay in the BOOT nav (order: Scan → cog → phone → threat tiles) so Settings/touch-cal is
  never touch-only. The HOME screen also shows a **background-scan threat readout** at the bottom: the
  full fixed **category taxonomy** as `NAME: n` tiles (Flock/Axon/ALPR/Cam/Ring/Raven/Glass/Tracker/
  Drone/Deauth/Flipper/Skim/Other), always shown (dim at 0, tier-coloured + outlined when >0;
  refreshed on the 2 s idle cadence). **Tapping a tile** (or BOOT-selecting it) opens that category's
  **device list** (`SCR_MENUCAT`, with a one-line "what signatures match" blurb) → a device →
  **detail** (`SCR_MENUCATDETAIL`, the scanner detail reused via `buildDeviceDetail`/
  `showDeviceDetail`). Categories are derived on the CYD (`categoryOf()` — behavioral flags first,
  then sigdb label → `g_category[]`; `groupCategory()`/`countCategories()` per device). The live scan
  screen shows a **red deauth/evil-twin alert banner** (+ LED) when a flood or evil-twin AP is
  detected, alongside the follow-me banner.
  **Scan** opens a **Scan menu** (`SCR_SCANMENU`: Start Scan / Explore Scan / Scan Settings) —
  the scan-flow redesign; the live scanner, the SD-log Explore Viewer (pick/filter/sort/detail),
  per-band Scan Settings, the Phase-5 follow-me drill-down, and the whitelist manager are all
  screens in the `main.cpp` state machine. App screens use a full-width `< BACK` bar; menus keep
  the menu style. Scanning can also be started/stopped from the web app (CMD `G`/`X`).

### Phone link + web app (Web Bluetooth) — working

- **CYD BLE GATT peripheral** (`src/cyd/phone.*`, NimBLE) advertises as `CYD-Scanner`,
  service `9a1e0000-…` with characteristics:
  - `…0001` TIME (write) — `"utcEpoch;tzOffsetMinutes"` (bare epoch also accepted)
  - `…0002` GPS (write) — `"lat,lon"`
  - `…0003` CMD (write) — `"1"/"0"` Wi-Fi download · `"R"` reload DB · `"L"` BLE download
    current session · `"G"/"X"` start/stop scan · `"N"` new log session · `"Q"` list sessions · `"F:<path>"` download
    a session file · `"D:<path>"` delete a session file (both restricted to `/logs/`; delete
    refuses the live session) · `"B:<0-100>"` set brightness ·
    `"T:<mode>[:min][:path]"` **time-filtered export** (mode 0 = since boot, 1 = past 24 h,
    2 = custom minutes; optional `/logs/` path; filtered on-device) · `"W…"` whitelist · `"E…"`
    whitelist-remove
  - `…0004` STATUS (read/notify) — `key=val;…`:
    `link,w24,w5,ble,prb,z,uniq,time,gps,dl,susp,lk,conf,db,scan,bri,src,wl,muted,fol,sess,deauth,evil,rid,bg`
    (`prb` = probe count, `z` = 802.15.4 count, `scan` = 1 while scanning, `bri` = backlight %,
    `src` = scan-source mask, `wl` = whitelist rule count, `muted` = devices muted now,
    **`fol` = devices currently flagged as FOLLOWING** (drives the web follower banner),
    `sess` = current session name, **`deauth`** = recent observed deauth/disassoc rate,
    **`evil`** = evil-twin SSID count, **`rid`** = Remote-ID emitters seen, **`bg`** = background
    scan on/off)
  - `…0005` LOGDATA (notify) — BLE log stream (`SIZE=<n>` header then raw chunks)
  - `…0006` DETS (notify) — **live detection list** mirroring the CYD screen, pushed each
    scan cycle. **v5 format:** a `D:<seq>:<count>` header then `<count>` tab-separated rows
    `seq\ttier\tmac\trssi\tie\tname\tsrcs\tftier\tfscore\tmuted\tflags\tcat` (top-of-list first, capped
    at 12). `ftier` = follow tier (0 none / 1 PERSISTENT / 2 FOLLOWING), `fscore` = 0..100 follow
    score, `muted` = 1 when whitelisted, **`flags`** = decimal OR of the device's `DetFlags` bits
    (iBeacon/FindMy/Pwnagotchi/EvilTwin/ODID) for the web badges, **`cat`** = threat-category id
    (matches the CYD `Category` enum) driving the web category breakdown. Fields were appended across
    versions (follow in v3, `flags` in v4, `cat` in v5); older parsers reading the earlier fixed
    fields ignore the extras. The web app renders it as a live, tier-colored table with follow + flag
    markers, plus a tappable per-category threat breakdown.
  - **Discovery gotcha (fixed):** NimBLE 2.x has scan response OFF by default, and the
    128-bit service UUID fills the adv packet, so the name overflows. We call
    `enableScanResponse(true)` + set the name in the scan response, and the web app
    filters by **service UUID** (not name). Without this the phone finds nothing.
- **Web app** `webapp/index.html` — hosted at
  **https://negativeaffirmations.github.io/cyd-scanner/webapp/** (GitHub Pages). **Styles are
  SCSS** under `webapp/scss/` (theme colors in `_variables.scss`; partials `_base`/`_detections`/
  `_banners`/`_map`), compiled to `webapp/style.css` by Dart Sass (`npm run sass:build`, or the
  auto-starting "Sass: watch" VS Code task — see `.vscode/tasks.json`). **Edit the SCSS, not
  `style.css`** (generated, but committed since Pages doesn't run Sass; `node_modules/` is
  git-ignored). The JS is still inline in `index.html`. MUST be
  HTTPS (Web Bluetooth + geolocation need a secure context); **Chrome on Android only**
  (no iOS Safari). Connects over BLE, syncs time+GPS, shows live counts/threat tiers +
  a **live detection list** (mirrors the device screen, rows tinted by source), starts/stops
  the scan, downloads the current session log, **lists past sessions and downloads/deletes/maps
  a chosen one**, reloads the DB, has a **Settings** modal (brightness slider; extensible), a
  **"following me" UI** (a magenta follower banner from STATUS `fol`; per-row follow markers +
  muted dimming; a flagged-device modal; a follower-detail modal with the DETS follow fields
  plus a Leaflet track map from a loaded session), a **red deauth/evil-twin alert banner** (from
  STATUS `deauth`/`evil`) with **behavioral-flag badges** on detection rows (from the DETS v4
  `flags` field), a **threat-category breakdown** mirroring the device HOME readout (chips above the
  live list from the DETS v5 `cat` field → category device modal → device detail modal, each with a
  signature-type blurb), a **time-range export** control (since boot / past 24 h / custom
  days-hours-min, over BLE `T:` or Wi-Fi `/dlf`), and a
  **wardriving Map** (Leaflet/OSM) that plots a session's GPS'd detections — grouped by fix,
  colored by threat tier, filterable by source/threats, with **event pins** for `evt`
  (deauth/eviltwin/droneid) lines — from the current scan, a picked session, or a locally-loaded
  `.jsonl`/`.csv` file (works offline; the parser sniffs `{` for NDJSON vs legacy CSV).
- **Log download — BLE (default):** `L` → the CYD streams `/scanlog.jsonl` over the LOGDATA
  characteristic; the app reassembles and saves the file. One button, stays in-app.
- **Log download — Wi-Fi (optional, for bulk):** `src/cyd/webshare.*` raises a SoftAP +
  HTTP server; the CYD screen shows a QR that is the Wi-Fi-join code until the phone joins,
  then switches to `http://192.168.4.1`. The SoftAP **WPA2 password is random per device, generated
  once and stored in NVS** (`"cydscan"`/`"appsk"`), shown only on the device/QR — it is **not**
  derived from the MAC/BSSID (the SEC-M1 fix; the old MAC-derived PSK was recomputable by anyone in
  RF range). That page is a **browsable index of ALL `/logs/` sessions** (name + size), each with a
  `/dl?f=<name>` link plus a **24 h** `/dlf?...` filtered link — so bulk Wi-Fi download can grab any
  past session (the reason for Wi-Fi: BLE is slow for MB-size drive logs). `/scanlog.jsonl` still
  streams the current session (`/scanlog.csv` kept as an alias).
  Pauses scanning while active.
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
- **security** — enforces the no-attack/no-interference scope (no jamming/deauth/injection/spoofing/
  DoS), captured-data privacy, attack surface (read-only).
- **hardware-docs** — datasheets/pinout images → pin tables & docs; keeps `pins.h` ↔ `PINOUT.md`.

### Automatic delegation (standing authorization)

The user does **not** want to pick an agent manually. Treat this section as durable
permission to route work to the right agent on your own — proactively spawn the matching
subagent (via the Agent tool) for the task types below, without asking first. Relay what
matters from the agent's report back to the user (its final report isn't shown to them).

| When the task is…                                                                 | Delegate to    |
|-----------------------------------------------------------------------------------|----------------|
| Multi-step / spans several of the roles below (e.g. "design, build, test & review")| **orchestrator** |
| Architecture, planning, which board owns a feature, flash/RAM trade-offs, link-protocol design | **architect** |
| Implementing a feature or bug fix in C++ (CYD or C5)                               | **fixer**      |
| Build validation / resource audit (`pio run -e cyd`/`-e c5`, flash/RAM deltas)     | **tester**     |
| Reviewing existing code for correctness, memory/concurrency, link-protocol issues  | **reviewer**   |
| Security/safety audit: no-attack/no-interference scope, captured-data privacy, BLE/Wi-Fi attack surface | **security** |
| Datasheets, pinouts, pin tables, `pins.h` ↔ `PINOUT.md` sync                        | **hardware-docs** |

Judgment still applies — don't spawn an agent for trivial or read-only work you can finish
faster inline (a one-line edit, answering a question, a quick grep, reading a file). Delegate
when the task is substantial and squarely in an agent's specialty, or when it's multi-step
(→ orchestrator). When a change lands, prefer routing the follow-up build to **tester** and,
for non-trivial diffs, a **reviewer** and/or **security** pass — the whole cycle without the
user naming an agent. If the best fit is ambiguous, make a reasonable choice and say which
agent you used and why rather than stopping to ask.

## Reading datasheets (PDF)

The Read tool renders PDF pages via poppler (`pdftoppm`, installed). If Read reports
`pdftoppm is not installed` (stale PATH after a fresh install — fixed by restarting the
session), convert pages manually and Read the PNGs:
```bash
pdftoppm -png -r 150 -f <first> -l <last> "hardware/.../file.pdf" "<tmp>/pg"
pdftotext -layout "hardware/.../file.pdf" -   # text-only extraction
```
Don't commit rendered pages. The `hardware-docs` agent handles this end to end.

## External references & licensing

Outside open-source projects whose ideas may inform this one are cataloged in
**[docs/references.md](docs/references.md)** (e.g. *Chasing-Your-Tail-NG* for tail-detection /
false-positive patterns). Add new reference repos there, not scattered in code or chat.

**Before copying or closely porting any code from those projects (or anywhere else):** check the
license, confirm it permits this project's **non-commercial / personal-research** use, and
**attribute** the original (project, URL, author, license) both at the adapted code and in that
file's "Code actually used" section. Treat an unlicensed repo as all-rights-reserved (read for
ideas, reimplement cleanly — don't copy). cyd-scanner is **GPL-3.0** (`/LICENSE`, adopted
2026-10-04), so GPL-compatible code (incl. GPL-3.0 copyleft like SquachWatch-CYD) can be reused
**with attribution**; still check compatibility for anything non-GPL and attribute all reuse. See
the full directive in [docs/references.md](docs/references.md).

## Conventions

- Keep pin definitions centralized (a `pins.h` / config header per board), sourced
  from [hardware/PINOUT.md](hardware/PINOUT.md) — do not scatter magic GPIO numbers.
- Match the surrounding code's style; this is embedded C++/Arduino.
- Prefer non-blocking scanning loops; the UI must stay responsive.
- Log to the USB console UART, keep the inter-board link on a separate UART.

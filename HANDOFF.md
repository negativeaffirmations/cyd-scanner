# Handoff — cyd-scanner

Snapshot for the next session. Read this, then [CLAUDE.md](CLAUDE.md) for architecture
and gotchas, and [docs/signature-matching.md](docs/signature-matching.md) for the roadmap.

_Last updated: 2026-10-01. Repo: https://github.com/negativeaffirmations/cyd-scanner (public).
Latest commit: `b521221`. Working tree clean._

## What this is

Passive counter-surveillance firmware (detect Flock/ALPR cameras & similar RF surveillance)
on two boards: **CYD** (ESP32-2432S028R, host/UI/SD/phone-link) + **ESP32-C5** (dual-band
Wi-Fi + BLE + **802.15.4** scanner co-processor), one PlatformIO project, shared UART link
protocol. **Passive only** — never add jamming/deauth/injection. (The lone exception is the
separate `env:c5test` bench transmitter — a test tool, never the scanner; see Build section.)

## Current state — working on hardware

- Both boards bring up; inter-board UART link solid (framed, synchronous request/response).
  **Link protocol is at v5** — Detection carries flags/ie_hash/companyId/svc[16]/**panId**
  (66 bytes, `static_assert`-guarded); `ScanConfig.sources` mask is
  `MASK_WIFI24=1<<0, MASK_BLE=1<<1, MASK_154=1<<2, MASK_PROBE=1<<3, MASK_WIFI5=1<<4`.
  **A v-mismatch kills the link, so both boards MUST be flashed together on any protocol change.**
- **C5** (`src/c5/main.cpp`, `promisc.*`, `ieee154.*`): continuous async Wi-Fi (2.4+5 GHz) + BLE
  scan + **passive 802.15.4 capture** → mutex-guarded detection table, streamed on `StartScan`.
  The single 2.4 GHz radio is time-sliced across phases `PH_SCAN → PH_PROMISC → PH_154 → gap`.
  Honors the source mask **passively** (skips scan phases / drops disabled bands / starts-stops
  the BLE scan — only skips/drops, never transmits). BLE scan is callback-only + 5 s watchdog.
- **CYD** (`src/cyd/main.cpp`): polls ~2 s, scores vs the SD signature DB, logs first-seen
  devices to **per-session NDJSON files** (`/logs/sess-NNNNN.jsonl`, renamed to
  `/logs/YYYYMMDD-HHMMSS.jsonl` on time sync). Portrait UI + status bar + threat tiers + link dot.
- **Touch works**: bit-bang XPT2046, **pressure-based** presence over SPI (PENIRQ unused).
  Calibration persists in NVS (`touchcal`, `CAL_VERSION=2`).

### 802.15.4 / Zigbee-Thread (Phase 4 — NEW, validated on hardware)

- **C5 passive 802.15.4 sniffer** (`src/c5/ieee154.*`): `PH_154` time-slice hops channels
  {11,15,20,25,26} (~150 ms) for ~3 s. Receive-only (promiscuous, no TX/ED/CCA-TX/auto-ACK).
  ISR-safe: `IRAM_ATTR` parse → `xQueueSendFromISR` → mandatory `receive_handle_done` (20 RX
  bufs) → task-context `tick()` merges to the table. Carries PAN ID + short/EUI-64 source addr
  (OUI packed into `mac[]` so the existing `oui,…,4/A` sigdb rules match 15.4 for free) +
  beacon/extended flags.
- **15.4 is OPT-IN / default-OFF.** Continuous BLE scan (~99% duty) starves 15.4 RX via radio
  coexistence, so when 15.4 is enabled the C5 **time-slices BLE off during the `PH_154` window**
  (publish `g_phase=PH_154` before stopping BLE; 3 restart paths gated on `g_phase!=PH_154`).
  With 15.4 off, BLE runs bit-for-bit as before. **Do not flip the default back on.** Full
  rationale: memory `ieee802154-optin-ble-coexistence`. `enable()==ESP_OK` does NOT prove RX —
  only a transmitter does (see `env:c5test`).

### NDJSON scan logs (NEW this session — replaces CSV)

- Scan logs are **NDJSON** (`/logs/*.jsonl`, one JSON object per line): keys
  `epoch,ms,[lat,lon],src,mac,rssi,ch,[ie,cid,uuid,pan,name,sig],score,tier`. Device writer
  (JSON-escaped, overflow-guarded, non-finite GPS rejected), on-device Scan-Viewer reader
  (ArduinoJson **v6** `StaticJsonDocument<512>`, zero-copy on a mutable line buffer — pinned v6,
  v7 heap-allocates), and web app parser (`parseScanLog`/`parseScanNdjson`, `{`-sniff vs legacy
  CSV) all updated. **Old `.csv` sessions still list/read** (branch by extension). JSON escaping
  structurally killed the unescaped-comma + spreadsheet-formula-injection bugs. webshare serves
  `/scanlog.jsonl` (`.csv` alias kept). Future SQLite/pcap **import** is a host-side tool, not on
  the device — memory `storage-format-rethink`.

### Scan-flow UI (both surfaces; kept internally consistent per platform)

The entry point opens a **Scan menu**, it does not start a scan.

- **Scan menu:** Start Scan · **New Session** · Explore Scan · Scan Settings (CYD adds Back).
  A **`Session: <name>`** line (small font) shows the active session on both the CYD Scan menu
  and the web app scan panel, fed by STATUS `sess=`.
- **New Session** (Scan menu + phone CMD `N`): starts a fresh log (new file + **reset dedup**)
  without rebooting. Lazy file create (no empty files); works mid-scan. (A log file is otherwise
  one-per-boot; nothing else rotates it.)
- **Scan Settings:** enable/disable **BLE / Wi-Fi 2.4G / Wi-Fi 5G / 802.15.4** (15.4 default OFF,
  the rest ON). CYD persists the mask in NVS (`cydui`/`srcmask`), sends it each poll; web has a
  checkbox dialog (CMD `S:<n>`, reflected in STATUS `src=`). Switchable mid-scan.
- **Explore Scan → Scan Viewer:** pick a past session, browse oldest→newest, tap a row → detail
  view (Name, MAC, Source, RSSI, Channel, tier+score+signature, IE, BLE cid/UUID, **PAN ID**,
  time, GPS). Web detail adds a Leaflet mini-map; CYD has none. CYD streams the SD log via a
  512-row offset index (alloc on Explore enter, freed on exit).
- **Filter/Sort:** web full set; CYD subset. Reached from the viewer's `< Back / Filter / Sort`.

### Live detections / phone stream

- **DETS stream v2: seq-tagged atomic snapshots, merged by MAC.** Header `D:<seq>:<n>` then rows
  `seq\ttier\tmac\tbestRssi\tie\tname\ttag:rssi,...`. Web app dedups by MAC, renders complete
  snapshots only, keeps last list on a transient empty.
- **Web app** `webapp/index.html`: flexbox live table (per-band pills, `154` source, sticky
  header). Shared helpers: `parseScanLog`/`parseScanNdjson`/`parseScanCsv`, `renderDetList`, `newMap`.

## Build / flash / test

`pio` is not on PATH; it's at `~/.platformio/penv/Scripts/pio.exe` (Windows). Set
`PYTHONIOENCODING=utf-8` or uploads hang on Windows.
```bash
PYTHONIOENCODING=utf-8 ~/.platformio/penv/Scripts/pio.exe run -e cyd                      # build CYD
PYTHONIOENCODING=utf-8 ~/.platformio/penv/Scripts/pio.exe run -e c5                       # build C5
PYTHONIOENCODING=utf-8 ~/.platformio/penv/Scripts/pio.exe run -e cyd -t upload --upload-port COM14
```
- **Boards by USB VID:PID** (COM numbers are NOT stable): CYD = CH340 `1A86:7523`;
  C5 native USB = `303A:1001` (**flash the C5 here**; its UART bridge `2E3C:5740` won't flash).
  Two C5s share `303A:1001` — tell them apart by the chip MAC in the USB composite-device node.
- **`[env:c5test]` = 802.15.4 TEST SOURCE** (`src/c5test/`): bench transmitter for a SECOND C5
  (never the scanner's), cycling short/extended/beacon frames on ch15 to exercise PH_154. Build:
  `… run -e c5test`; flash via native USB (`303A:1001`), `-t upload --upload-port <COM>`. The only
  transmitting firmware in the project.
- **Protocol-version changes require flashing BOTH boards together** (FrameParser rejects a mismatch).
- Reflashing the live device-C5 over USB may be blocked by the host as "interfere with workloads";
  a normal flash of a freshly-set-up board was fine. If blocked, surface it to the user.
- To read serial without resetting the board, use a non-resetting reader (open COM, don't toggle
  DTR/RTS). The C5 native USB resets on open; for the CYD CH340, esptool `--before default_reset
  --after hard_reset flash_id` bounces it to run mode.

## Wiring (inter-board link)

CYD **GPIO22 (TX) → C5 GPIO4 (RX)**, CYD **GPIO27 (RX) ← C5 GPIO5 (TX)**, **GND↔GND**.
115200 baud, C5 link UART pinned to XTAL clock. (Single-cable power VIN→5V0 is documented in
[hardware/PINOUT.md](hardware/PINOUT.md) §3 but is currently NOT wired — C5 is USB-powered, or
5 V pin.) A second-C5 15.4 add-on is possible (CYD GPIO26 TX + GPIO35 input-only RX, spare UART)
but deemed premature — memory `ieee802154-optin-ble-coexistence`.

## Hard-won gotchas (don't rediscover these)

- **802.15.4 ↔ BLE coexistence:** continuous NimBLE scan at ~99% duty starves 15.4 RX on the
  shared 2.4 GHz radio → 0 frames received even with a close transmitter (while `enable()` still
  returns ESP_OK). Fix = pause BLE during `PH_154` (only when 15.4 is on). 15.4 default-off.
- **Touch read bug (fixed):** `readChan()` must skip the XPT2046 busy bit after the command byte,
  or conversions read at half-scale. Presence is from the **Z pressure channels over SPI**, not
  PENIRQ (GPIO36 input-only, unreliable). Read-scale change bumps `CAL_VERSION` → one-time recal.
- **C5 flashing**: native USB port only (`303A:1001`), auto-download, no button press.
- **BLE discovery**: NimBLE 2.x scan-response OFF by default + 128-bit UUID fills the adv packet →
  name overflows. Fixed via `enableScanResponse(true)` + name in scan response + web filters by UUID.
- **Link is synchronous**: CYD sends one command, waits for `Status(scanning=0)`. Never interleave.
- **Wi-Fi corrupts a UART on the shared PLL clock** → C5 link UART uses `UART_CLK_SRC_XTAL`.
- **BLE scan must be callback-only** (`setMaxResults(0)`) + a 5 s watchdog, or it stalls after ~10 min.
- **NDJSON logs:** append-only + power-loss-safe (a torn write loses only the trailing line, which
  both readers skip). ArduinoJson pinned to **v6** (v7 removes `StaticJsonDocument`/heap-allocates).
- **partitions**: all envs use `huge_app.csv` (c5test uses default — it's small).
- **Browser limits**: web app can't auto-join Wi-Fi, can't fetch() `http://192.168.4.1` (mixed
  content), loses BLE if it navigates there → BLE is the default download path.
- **Web app cache**: version tag at the bottom (`ui YYYY-MM-DDx`); GitHub Pages/browser caching is
  sticky — reload with `?v=N` and confirm the tag (currently `ui 2026-10-01d`).
- **Do NOT ask the user to press the CYD BOOT/RST button** (device is mounted; memory
  `no-boot-button-requests`). Drive via the web app or the non-resetting serial reader.

## File map

- `src/cyd/main.cpp` — CYD app: poll/scan/score/log(NDJSON)/render, status bar, menus + Scan menu
  (+ New Session, session-name line) + Scan Settings + **Explore Scan / Scan Viewer / detail /
  Filter / Sort** (all here), `beginSessionNamed`/`startNewSession`, shared `drawListMenu`/
  `drawFileList`/`drawDetRow`/`tierColor`/`listTouch`/`drawTopBarSeg`.
- `src/cyd/sigdb.*` — signature DB load + scoring (OUI layer gated on FLAG_154_EXTENDED for 15.4).
- `src/cyd/phone.*` — BLE GATT peripheral (CMD `S:`/`N`/… etc.).
- `src/cyd/webshare.*` — Wi-Fi SoftAP + HTTP log server (serves `/scanlog.jsonl`).
- `src/cyd/touch.*` — bit-bang XPT2046.  `src/cyd/pins.h`, `src/c5/pins.h` — pin maps (source: PINOUT.md).
- `src/c5/main.cpp` — async scanner + link responder; phase state machine + BLE time-slice.
- `src/c5/promisc.*` — Wi-Fi probe/IE capture.  `src/c5/ieee154.*` — passive 802.15.4 sniffer.
- `src/c5test/main.cpp` — **802.15.4 TEST SOURCE** (separate `env:c5test`; only firmware that TXs).
- `lib/link_protocol/link_protocol.h` — shared message types (**PROTOCOL_VERSION 5**), `encodeFrame`/`FrameParser`.
- `webapp/index.html` — phone control app (live table, Explore + viewer + detail + filter/sort, map).

## Signature DB format (`/signatures.csv` on the SD card)

```
thresholds,40,70,100
oui,B4:1E:52,70,A,Flock IEEE          # kind,pattern,weight,srcmask(W/B/4/A),label
prefix,Flock,50,W,Flock SoftAP
ie,1A2B3C4D,60,W,Flock IE fp          # 8-hex 802.11 IE fingerprint
bleuuid,<uuid>,...  /  blecid,<hex>,...# BLE service UUID / company ID
```
Editable on the card or reloadable from the phone. The signature `srcmask` letters (W/B/4/A) are
SEPARATE from the scan-source mask in `ScanConfig`. (`4`/`A` rules also match 15.4 EUI-64 OUIs.)

## Backlog / next steps

- **15.4 field validation:** reception confirmed vs the `c5test` transmitter; validate against
  real Zigbee/Thread gear and tune the channel set/dwell. Signature value is still speculative
  (no research it's a good surveillance vector) — memory `ieee802154-optin-ble-coexistence`.
- **Storage:** on-device NDJSON shipped (comma + formula-injection bugs resolved). Still open: the
  **host-side SQLite/pcap import + `.csv`→`.jsonl` conversion** tool for Phase 5/6 correlation.
- **Strict passivity (optional):** C5 BLE scan uses `setActiveScan(true)` (transmits scan-request
  PDUs) — pre-existing; switch to `setActiveScan(false)` for fully receive-only.
- **CYD viewer drag perf:** re-reads ~18 SD rows per drag redraw; cache the visible window if sluggish.
- **Roadmap (docs/signature-matching.md):** Phase 4 (802.15.4) **done**; **Phase 5** spatial/temporal
  correlation; **Phase 6** supervised capture + desktop correlation script.

## AI-assist notes

Custom agents in `.claude/agents/` (orchestrator, architect, fixer, tester, reviewer, security,
hardware-docs); CLAUDE.md grants standing authorization to auto-delegate. A PostToolUse hook
(`.claude/hooks/gitignore-sensitive.py`) auto-gitignores files with machine paths/secrets (note:
captured RF logs `/logs/`, `*.jsonl`, `*.pcap`, etc. are gitignored — never commit captured human
data to the public repo). `git`/`gh` authenticated as `negativeaffirmations`; commit messages end
with the `Co-Authored-By: Claude Opus 4.8` trailer. Persistent session memory lives under the
project's `memory/` — read `MEMORY.md` first.

# Handoff — cyd-scanner

Snapshot for the next session. Read this, then [CLAUDE.md](CLAUDE.md) for architecture
and gotchas, and [docs/signature-matching.md](docs/signature-matching.md) for the roadmap.

_Last updated: 2026-09-30. Repo: https://github.com/negativeaffirmations/cyd-scanner (public).
Latest commit: `6db0de5`. Working tree clean._

## What this is

Passive counter-surveillance firmware (detect Flock/ALPR cameras & similar RF surveillance)
on two boards: **CYD** (ESP32-2432S028R, host/UI/SD/phone-link) + **ESP32-C5** (dual-band
Wi-Fi + BLE scanner co-processor), one PlatformIO project, shared UART link protocol.
**Passive only** — never add jamming/deauth/injection.

## Current state — working on hardware

- Both boards bring up; inter-board UART link solid (framed, synchronous request/response).
  **Link protocol is at v4** — Detection carries flags/ie_hash/companyId/svc[16]; `ScanConfig.sources`
  mask is now `MASK_WIFI24=1<<0, MASK_BLE=1<<1, MASK_154=1<<2, MASK_PROBE=1<<3, MASK_WIFI5=1<<4`.
  **A v-mismatch kills the link, so both boards MUST be flashed together on any protocol change.**
- **C5** (`src/c5/main.cpp`, `promisc.*`): continuous async Wi-Fi (2.4+5 GHz) + BLE scan →
  mutex-guarded detection table, streamed on `StartScan`. Honors the source mask **passively**
  (skips scan phases / drops disabled bands by channel / starts-stops the BLE scan — only
  skips/drops, never transmits). BLE scan is callback-only + 5 s watchdog.
- **CYD** (`src/cyd/main.cpp`): polls ~2 s, scores vs the SD signature DB, logs first-seen
  devices to **per-session files** (`/logs/sess-NNNNN.csv`, renamed to `/logs/YYYYMMDD-HHMMSS.csv`
  on time sync). Portrait UI + status bar + threat tiers + link dot.
- **Touch WORKS now** (big change this session — see gotchas): bit-bang XPT2046, **pressure-based**
  presence over SPI (PENIRQ unused). Calibration persists in NVS (`touchcal`). A one-time
  recalibration was required after the read-scale fix (`CAL_VERSION=2`).

### Scan-flow UI (NEW this session — Phases 0-3 complete, both surfaces)

The entry point no longer starts a scan; **"Scan"** opens a Scan menu. Applies to the CYD screen
and the phone web app, kept internally consistent per platform (reusable components).

- **Scan menu:** Start Scan · Explore Scan · Scan Settings (CYD adds a Back row).
- **Scan Settings:** enable/disable **BLE / Wi-Fi 2.4G / Wi-Fi 5G** (default all on). CYD persists
  the mask in NVS (`cydui`/`srcmask`), sends it each poll; web has a checkbox dialog (CMD `S:<n>`,
  reflected in STATUS `src=`). The 2.4 toggle also gates probe capture (both are 2.4 GHz Wi-Fi).
- **Explore Scan → Scan Viewer:** pick a past session (newest first, default most recent) and
  browse its detections oldest→newest, styled like the live list + a timecode. Tap a row →
  **detail view** (Name, MAC, friendly Source, RSSI, Channel, Threat tier+score+signature, IE,
  BLE company-ID/UUID, friendly time, GPS). **Web detail adds a Leaflet mini-map; CYD has no map.**
  CYD streams the SD log via a 512-row offset index (alloc on Explore enter, freed on exit).
- **Filter/Sort:** web full set (sort 8 ways; filter by type/threats/RSSI-distance/time-range/text).
  CYD subset (sort oldest/newest/type/distance — no name; filter type/threats/distance — no
  time/text), reached from the viewer's `< Back / Filter / Sort` top bar.

### Live detections / phone stream (reworked this session)

- **DETS stream is v2: seq-tagged atomic snapshots, merged by MAC.** Header `D:<seq>:<n>` then
  rows `seq\ttier\tmac\tbestRssi\tie\tname\ttag:rssi,...`. The web app drives snapshots off the
  rows (tolerates a lost header), dedups by MAC, renders only complete snapshots, keeps the last
  list on a transient empty — fixed the old duplicates + flicker-clear.
- **Web app** `webapp/index.html`: live-detections table rebuilt with **flexbox** (full width,
  per-band pills, sticky column header, ellipsized names, expand caret only for multi-source).
  Shared helpers: `parseScanCsv`, `renderDetList` (live + viewer modes), `newMap`.

## Build / flash / test

`pio` is not on PATH; it's at `~/.platformio/penv/Scripts/pio.exe` (Windows). Set
`PYTHONIOENCODING=utf-8` or uploads hang on Windows.
```bash
PYTHONIOENCODING=utf-8 ~/.platformio/penv/Scripts/pio.exe run -e cyd                      # build CYD
PYTHONIOENCODING=utf-8 ~/.platformio/penv/Scripts/pio.exe run -e c5                       # build C5
PYTHONIOENCODING=utf-8 ~/.platformio/penv/Scripts/pio.exe run -e cyd -t upload --upload-port COM14
```
- **Boards by USB VID:PID** (COM numbers are NOT stable): CYD = CH340 `1A86:7523` (was COM14);
  C5 native USB = `303A:1001` (was COM18, **flash the C5 here**; its UART bridge `2E3C:5740` won't flash).
- **The VIN→5V0 single-cable-power jumper is NOT connected**, so the C5 can be flashed over USB
  any time without pulling a jumper.
- **Protocol-version changes require flashing BOTH boards together** (FrameParser rejects a mismatch).
- To read serial without resetting the board, use the non-resetting reader in the scratchpad
  (`serread_noreset.ps1`); `serread.ps1` pulses DTR/RTS to reset+capture boot.

## Wiring (inter-board link)

CYD **GPIO22 (TX) → C5 GPIO4 (RX)**, CYD **GPIO27 (RX) ← C5 GPIO5 (TX)**, **GND↔GND**.
115200 baud, C5 link UART pinned to XTAL clock. (Single-cable power VIN→5V0 is documented in
[hardware/PINOUT.md](hardware/PINOUT.md) §3 but is currently NOT wired.)

## Hard-won gotchas (don't rediscover these)

- **Touch read bug (fixed):** `readChan()` must skip the XPT2046 busy bit after the command byte,
  or every conversion reads at half-scale (idle pressure ~2048, X rails to the edge). Touch presence
  is now from the **Z pressure channels over SPI**, not the PENIRQ pin (GPIO36 is input-only and was
  unreliable). Changing the read scale bumps `CAL_VERSION` → forces a one-time recalibrate.
- **C5 flashing**: native USB port only (`303A:1001`), auto-download, no button press.
- **BLE discovery**: NimBLE 2.x scan-response OFF by default + 128-bit UUID fills the adv packet →
  name overflows. Fixed via `enableScanResponse(true)` + name in scan response + web filters by UUID.
- **Link is synchronous**: CYD sends one command, waits for `Status(scanning=0)`. Never interleave.
- **Wi-Fi corrupts a UART on the shared PLL clock** → C5 link UART uses `UART_CLK_SRC_XTAL`.
- **BLE scan must be callback-only** (`setMaxResults(0)`) + a 5 s watchdog, or it stalls after ~10 min.
- **partitions**: both envs use `huge_app.csv`.
- **Browser limits**: web app can't auto-join Wi-Fi, can't fetch() `http://192.168.4.1` (mixed
  content), loses BLE if it navigates there → BLE is the default download path.
- **Web app cache**: the page shows a version tag at the bottom (`ui YYYY-MM-DDx`) and sends a
  no-cache hint, but GitHub Pages/browser caching is sticky — reload with `?v=N` to force the
  latest, and confirm via the version tag.
- **Do NOT ask the user to press the CYD BOOT/RST button** (device is mounted; see memory
  `no-boot-button-requests`). Drive via the web app or the non-resetting serial reader instead.

## File map

- `src/cyd/main.cpp` — CYD app: poll/scan/score/log/render, status bar, menus + Scan menu +
  Scan Settings + **Explore Scan / Scan Viewer / detail / Filter / Sort** (all here; no scanview.cpp),
  shared `drawListMenu`/`drawFileList`/`drawDetRow`/`tierColor`/`listTouch`/`drawTopBarSeg`.
- `src/cyd/sigdb.*` — signature DB load + scoring. `src/cyd/phone.*` — BLE GATT peripheral (CMD `S:` etc.).
- `src/cyd/webshare.*` — Wi-Fi SoftAP + HTTP log server. `src/cyd/touch.*` — bit-bang XPT2046 (now used).
- `src/cyd/pins.h`, `src/c5/pins.h` — pin maps (source of truth: `hardware/PINOUT.md`).
- `src/c5/main.cpp` — async scanner + link responder; honors the source mask. `src/c5/promisc.*` — probe capture.
- `lib/link_protocol/link_protocol.h` — shared message types (PROTOCOL_VERSION 4), `encodeFrame`/`FrameParser`.
- `webapp/index.html` — phone control app (live table, Explore Scan + viewer + detail + filter/sort, map).

## Signature DB format (`/signatures.csv` on the SD card)

```
thresholds,40,70,100
oui,B4:1E:52,70,A,Flock IEEE          # kind,pattern,weight,srcmask(W/B/4/A),label
prefix,Flock,50,W,Flock SoftAP
ie,1A2B3C4D,60,W,Flock IE fp          # 8-hex 802.11 IE fingerprint
bleuuid,<uuid>,...  /  blecid,<hex>,...# BLE service UUID / company ID
```
Editable on the card or reloadable from the phone. The signature `srcmask` letters (W/B/4/A) are a
SEPARATE thing from the scan-source mask in `ScanConfig`.

## Backlog / next steps

- **Storage-format discussion (user wants this):** revisit CSV vs **SQLite** vs **pcap** for scan
  logs, and fix the **unescaped-comma** CSV bug regardless (a device name with a comma shifts
  columns). See memory `storage-format-rethink`.
- **Strict passivity (optional):** the C5 BLE scan uses `setActiveScan(true)` (transmits scan-request
  PDUs) — pre-existing; switch to `setActiveScan(false)` if fully receive-only is wanted.
- **CYD viewer drag perf:** re-reads ~18 SD rows per drag redraw; cache the visible window only if
  it feels sluggish on hardware.
- **Untested / light:** CYD viewer/detail/filter/sort layouts and the phone live-detection list are
  function-verified but eyeball the feel; portrait status-bar icons visually unverified.
- **Roadmap (docs/signature-matching.md):** tune the signature DB; **Phase 4** 802.15.4 presence
  (C5 differentiator); **Phase 5** spatial/temporal correlation; **Phase 6** supervised capture +
  desktop correlation script.

## AI-assist notes

Custom agents in `.claude/agents/` (orchestrator, architect, fixer, tester, reviewer, security,
hardware-docs); CLAUDE.md grants standing authorization to auto-delegate. A PostToolUse hook
(`.claude/hooks/gitignore-sensitive.py`) auto-gitignores files with machine paths/secrets.
`git`/`gh` authenticated as `negativeaffirmations`; commit messages end with the
`Co-Authored-By: Claude Opus 4.8` trailer. Persistent session memory (preferences, plans,
conventions) lives under the project's `memory/` — read `MEMORY.md` first.

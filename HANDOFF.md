# Handoff — cyd-scanner

Snapshot for the next session. Read this, then [CLAUDE.md](CLAUDE.md) for architecture
and gotchas, and [docs/signature-matching.md](docs/signature-matching.md) for the roadmap.

_Last updated: 2026-09-30. Repo: https://github.com/negativeaffirmations/cyd-scanner (public)._

## What this is

Passive counter-surveillance firmware (detect Flock/ALPR cameras & similar RF surveillance)
on two boards: **CYD** (ESP32-2432S028R, host/UI/SD/phone-link) + **ESP32-C5** (dual-band
Wi-Fi + BLE scanner co-processor), one PlatformIO project, shared UART link protocol.
**Passive only** — never add jamming/deauth/injection.

## Current state — working on hardware

- Both boards bring up; inter-board UART link solid (framed, synchronous request/response).
  **Link protocol is at v3** (Detection carries flags, ie_hash, companyId, svc[16]).
- **C5** (`src/c5/main.cpp`, `promisc.*`): continuous async Wi-Fi (2.4+5 GHz) + BLE scan →
  mutex-guarded detection table, streamed on `StartScan`. **Phase 2** promiscuous capture
  (probe reqs + IE fingerprints, hops 2.4 GHz 1/6/11). **BLE scan is callback-only**
  (`setMaxResults(0)`) + a 5 s watchdog — fixes BLE stalling ~10 min into a drive.
- **CYD** (`src/cyd/main.cpp`): polls ~2 s, scores vs the SD signature DB, logs first-seen
  devices to **per-session files** (`/logs/sess-NNNNN.csv`, renamed to
  `/logs/YYYYMMDD-HHMMSS.csv` on time sync), portrait UI + status bar + threat tiers + a
  **link dot** and, on the scan screen, a **STOP bar** across the top. Filters out its own +
  the connected phone's BLE MAC. **Session files are created lazily** (`ensureLogFile()`, on
  the first logged detection) so idle boots leave no empty file; `sweepEmptySessions()` deletes
  header-only leftovers at boot (≤16/boot).
- **Signatures**: `sigdb.*` + `/signatures.csv` (self-seeds; DB_GEN reseed trigger). Layers:
  OUI + name + **IE (Phase 2)** + **BLE UUID + company-ID (Phase 3)**. Flock GATT UUID seeded.
- **On-device UI**: BOOT-button **and touchscreen** (bit-bang XPT2046, `touch.*`) menu:
  Start Scan / Connect to Phone / **Settings** (Calibrate Touch, Brightness). Tap = next,
  hold = select. Boots to menu.
- **Phone web app**: connect, sync time+GPS, live counts/threats + **live detection list**
  (source-tinted rows), **start/stop scan** toggle, **Settings modal** (brightness),
  **Session logs** panel (list / download / **map** / delete; the newest file is labeled
  **(current session)** while scanning else **(latest session)** and floated to the top),
  **BLE log download**, **Wi-Fi bulk download = a browsable index of all sessions**, and a
  **wardriving Map** (Leaflet/OSM; loads current session, a picked session, or a local .csv offline).
- Hosted app: **https://negativeaffirmations.github.io/cyd-scanner/webapp/** (Android/Chrome).
- Recent fixes/changes (2026-09-30, all hardware-confirmed):
  - Stopping Wi-Fi download mode now repaints back to the main menu (was stuck on the last
    QR/download screen). `loop()` forces `g_screen = SCR_MENU; drawMenu()` on download exit.
  - Session logs created lazily + boot-time sweep of empty header-only files (see CYD bullet).
  - Session picker labels the newest file **(current/latest session)** (see web-app bullet);
    adds a third tab-separated flag field to each list row — the web app renders/floats it.
- Latest commit pushed: `971ea7f` (session picker labels). Working tree clean.

## Build / flash / test

`pio` is not on PATH; it's at `~/.platformio/penv/Scripts/pio.exe` (Windows).
```bash
pio run -e cyd                                   # build CYD
pio run -e c5                                    # build C5
pio run -e cyd -t upload --upload-port COM##     # flash CYD
```
- **Boards by USB VID:PID** (COM numbers are NOT stable): CYD = CH340 `1A86:7523`;
  C5 native USB = `303A:1001` (**flash the C5 here** — its UART bridge `2E3C:5740` will NOT flash).
- CYD's first upload attempt sometimes glitches — just retry.
- `pio device monitor` needs a TTY; to read serial in scripts, open the port directly
  (the session used a small PowerShell `System.IO.Ports.SerialPort` reader; pulse
  DTR=false/RTS=true then RTS=false to reset the CYD and capture boot).
- Changing `lib/link_protocol/link_protocol.h` rebuilds BOTH envs.

## Wiring (inter-board link)

CYD **GPIO22 (TX) → C5 GPIO4 (RX)**, CYD **GPIO27 (RX) ← C5 GPIO5 (TX)**, **GND↔GND**.
115200 baud, C5 link UART pinned to XTAL clock. Solid contacts matter — flip `LINK_MONITOR`
to 1 in both `main.cpp` for the heartbeat/loss connection tester.

**Single-cable power (in-car, confirmed working):** wire CYD **VIN (P1) → C5 5V0** (+ shared
GND) and power everything from one USB-C into the CYD (car charger/power bank ≥1 A). Each
board keeps its own 3.3 V regulator — feed 5 V only, never bridge the 3.3 V rails. Make the
VIN→5V0 wire a removable jumper so you can pull it before reflashing the C5 over USB (avoid
two 5 V sources). See [hardware/PINOUT.md](hardware/PINOUT.md) §3.

## Hard-won gotchas (don't rediscover these)

- **C5 flashing**: native USB port only (see above).
- **BLE discovery**: NimBLE 2.x scan-response is OFF by default; the 128-bit service UUID
  fills the adv packet so the name overflows. Fixed via `enableScanResponse(true)` + name in
  scan response + web app filtering by **service UUID**. Don't revert to name-filter.
- **Touch vs SD**: both want HSPI (display=VSPI). SD wins; touch is unused. An on-screen
  touch UI needs a software-SPI touch driver first — until then the **BOOT button** is the
  on-device control (shows the web-app QR).
- **Link protocol is synchronous**: CYD sends one command, waits for `Status(scanning=0)`.
  Never interleave commands mid-scan.
- **Wi-Fi corrupts a UART on the shared PLL clock** → C5 link UART uses `UART_CLK_SRC_XTAL`.
- **Log wipe**: bump `LOG_GEN` in `src/cyd/main.cpp` to force a one-time fresh `/scanlog.csv`.
- **Browser limits**: web app can't auto-join Wi-Fi, can't fetch() `http://192.168.4.1`
  (mixed content), loses BLE if it navigates there → BLE is the default download path.
- **partitions**: both envs use `huge_app.csv` (Wi-Fi+BLE overflow the default).
- **BLE scan must be callback-only**: the C5 sets `scan->setMaxResults(0)`. Without it,
  NimBLE buffers every advertisement (with `wantDuplicates`) and, driving past thousands
  of BLE devices, the cache fills the heap and the scan **silently stalls** (~10 min in a
  field test: BLE detections stopped while Wi-Fi kept logging). A 5 s watchdog in the C5
  `loop()` also restarts the scan if `isScanning()` is ever false.

## File map

- `src/cyd/main.cpp` — CYD app: poll/scan/score/log/render, status bar, QR screens, loop.
- `src/cyd/sigdb.*` — signature DB load + scoring. `src/cyd/phone.*` — BLE GATT peripheral.
- `src/cyd/webshare.*` — Wi-Fi SoftAP + HTTP log server. `src/cyd/touch.*` — (unused here).
- `src/cyd/pins.h`, `src/c5/pins.h` — pin maps (source of truth: `hardware/PINOUT.md`).
- `src/c5/main.cpp` — async scanner + link responder (`LINK_MONITOR` mode inside).
- `src/c5/promisc.*` — passive promiscuous capture (probe reqs + beacons → IE fingerprint).
- `lib/link_protocol/link_protocol.h` — shared message types, `encodeFrame`/`FrameParser`.
- `webapp/index.html` — phone control app. `/signatures.csv` — signature DB (on the SD card).

## Signature DB format (`/signatures.csv` on the SD card)

```
thresholds,40,70,100
oui,B4:1E:52,70,A,Flock IEEE          # kind,pattern,weight,srcmask(W/B/4/A),label
prefix,Flock,50,W,Flock SoftAP
exact,FS Ext Battery,50,B,Flock Penguin batt
ie,1A2B3C4D,60,W,Flock IE fp          # 8-hex 802.11 IE fingerprint (Phase 2; from field capture)
```
Editable on the card or reloadable from the phone (no reflash). Weights are seeds — tune
them via the supervised-capture workflow (Phase 6).

## Untested / needs eyes

- Portrait layout + hand-drawn status-bar icons (BLE/Wi-Fi/no-conn) — visually unverified.
- **New (this session), not yet hardware-tested:** BOOT-button main menu (tap/hold timing —
  `LONG_PRESS_MS` 550 ms), the status-bar link dot, and the phone live-detection list
  (DETS stream, 12-row cap, ~6 ms/row). Confirm the menu feels right and the live list
  keeps up on a real phone; the list only updates while the Start-Scan screen is active.
- BLE log-download chunk size is 180 B assuming a large negotiated MTU (worked on the test
  phone); add MTU-aware chunking if a download stalls.
- C5 **GPIO25/26** are unverified/possibly swapped — test before using.
- Device-side BLE + Wi-Fi-AP coexistence works in the download flow but is lightly tested.

## Next steps (roadmap — see docs/signature-matching.md)

1. **Tune the signature DB** — expand OUIs from community sources; add BLE names.
2. **Phase 2** — promiscuous-mode Wi-Fi capture (probe requests + IE fingerprint) on the C5:
   **DONE (first pass)** — `src/c5/promisc.*`, time-sliced with the AP scan, hops 2.4 GHz
   1/6/11; `PRB` source + `ie_hash` fingerprint carried in the link protocol (v2), scored
   via `ie,<hash>` DB rules, logged (`ie` column, log schema v3). **TODO:** 5 GHz
   promiscuous hopping, IE-content enrichment, behavioral scoring of the wildcard flag,
   and capturing real Flock IE hashes in the field to seed `ie,` rules.
3. **Phase 3** — BLE service-UUID / company-ID matching: **DONE** — C5 extracts BLE
   `companyId` + primary service `svc[16]` UUID (link protocol v3); CYD scores via
   `bleuuid,<uuid>` / `blecid,<hex>` DB rules, logs `cid`/`uuid` (log schema v4); Flock
   GATT UUID seeded. **TODO:** multiple UUIDs per device, Raven 0x3100–0x3500 rules.
   NOTE: existing SD cards keep their old DB — delete `/signatures.csv` (re-seeds with the
   new `bleuuid` rule) or add the line to adopt seeded rules.
4. **Phase 4** — 802.15.4 presence/fingerprint (C5; a differentiator no other Flock tool has).
5. **Phase 5** — spatial/temporal correlation ("seen at N GPS points", "following me").
6. **Phase 6** — supervised capture sessions + phone notes/photos/map + merge (BLE control,
   Wi-Fi bulk) + **desktop correlation script** (off-device) that emits candidate signatures.

## AI-assist notes

Custom agents live in `.claude/agents/` (orchestrator, architect, fixer, tester, reviewer,
security, hardware-docs) but only register after a Claude Code **session restart** — until
then use the built-in `Plan`/`general-purpose` agents. A PostToolUse hook
(`.claude/hooks/gitignore-sensitive.py`) auto-gitignores files containing machine paths/secrets.
`git`/`gh` are authenticated as `negativeaffirmations`; commit messages end with the
`Co-Authored-By: Claude Opus 4.8` trailer.

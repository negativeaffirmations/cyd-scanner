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
- **CYD GPIO beyond display/touch/SD:** 22/27 = C5 UART link; **35** (input-only, ADC1_CH7) =
  **battery-sense divider** (100k/100k off switched Vbat, `power.*`); **26** still free (no speaker,
  and the TP4056 CHRG pad is not wired); serial header 1/3 free. See [hardware/PINOUT.md](hardware/PINOUT.md) §4–§5.
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
  2.4 + 5 GHz beacons) and **two back-to-back passive promiscuous-capture windows** (Phase 2):
  **`PH_PROMISC`** sweeps all 2.4 GHz channels 1–11 (~160 ms dwell, ~4.5 s), then **`PH_PROMISC5`**
  sweeps the common US non-DFS 5 GHz channels (36/40/44/48/149/153/157/161/165, ~160 ms, ~4 s) so a
  camera on a 5 GHz AP is caught too (`esp_wifi_set_channel()` switches band by channel number; DFS
  52–144 skipped — an unassociated STA often can't park there). Both capture Wi-Fi **client probe
  requests** + **beacon/probe-resp 802.11 IE fingerprints** + **associated-client DATA frames** (the
  client MAC in the clear — `addr2` on an uplink/ToDS frame, `addr1` on a downlink/FromDS frame — of a
  device already joined to a home AP: a Ring/Nest/Wyze camera that neither beacons nor, once joined,
  probes; emitted as a Wi-Fi client / `Source::WifiProbe`, throttled by a recent-MAC ring) — all the
  client behavior `scanNetworks()` can't see.
  **BLE is PAUSED for both promiscuous windows** (and the 802.15.4 window) — a
  continuous NimBLE scan runs at ~99% radio duty and *starves* the passive promiscuous RX to near-zero
  (measured ~2 mgmt frames/window with BLE live vs ~100+ paused); it runs alongside the active AP scan
  only, which gets coex priority. The `blePaused()` helper gates every BLE (re)start path.
  All sources merge into a shared **detection table** (dedup by source+MAC, 30 s TTL,
  mutex-guarded because the BLE + promiscuous callbacks run in separate tasks; a captured
  IE fingerprint is preserved across scan refreshes). Scanning never blocks the link.
  Promiscuous capture itself is **receive-only** — it just monitors 802.11 frames (BLE scanning is
  active; see the Scope section). The C5 prints a per-window `mgmt/data/dataClient` capture-health
  line to its USB console.
- **Onboard GPS (C5, protocol v9 position; v10 adds time).** A GY-NEO6MV2 NEO-6M is wired to the C5 on
  **UART0** (console is USB-CDC, so UART0 is free; RX = GPIO15, TX = -1 → **receive-only**, 9600 8N1, XTAL
  clock). `src/c5/gps.*` runs a clean-room checksum-verified `$GxRMC`/`$GxGGA` NMEA parser, holds the latest
  fix (mutex-guarded), and `sendStatus()` folds `gpsLat`/`gpsLon` (deg·1e7) + alt/sats/fix **+ `gpsEpoch`
  (UTC secs from the RMC date+time, v10)** into every `Status` frame. The CYD routes position through a
  unified `posHasFix()/posLat()/posLon()` accessor and **time through `timeHasSync()/epochLocal()/
  timeSource()`** (log NDJSON, status bar clock, STATUS `gps=`/`time=`/`tsrc=`, follow scoring, session
  rename). **Phone is PREFERRED for both** (it fuses A-GPS/cell); the onboard fix/clock is the **fallback**.
  **Position freshness (fw 0.13.0, tightened 0.14.0):** the phone is an AVAILABLE source while connected and it
  wrote a fix within `PHONE_GPS_STALE_MS` (90 s) — a pocketed screen-off Android Chrome page stops
  `watchPosition` — and the onboard GPS while a Status carried a fix within 10 s. `posSource()`: the phone if
  its fix is < 10 s old (`PHONE_FIX_FRESH_MS`) or onboard has none; else live onboard; else **hold the most
  recent fix** of either (3). Log lines carry `"fix_age":<s>` when held OR the fix in use is > 10 s old
  (`posAged()`; see the SD-log notes). (0.13.0 kept a stale phone fix "live" for the whole 90 s — the
  2026-10-24 mini drive froze 86 s unmarked before onboard took over.) Follow scoring ignores fixes > 90 s old.
  **Source preference (fw 0.15.0, `g_timePref`/`g_gpsPref`, CMD `P:`):** *prefer phone* = the rules above
  (default); *prefer device* = live onboard fix / ready device clock (fresh GPS time, else valid RTC) first,
  the phone only while the device has nothing; *device only* = the phone's fix/time is ignored entirely (also
  for the held fallback and RTC discipline; the phone's tz offset is still learned).
  GPS time is UTC, so the CYD adds the **last phone tz offset, persisted in NVS** (`cydscan/tzoff`,
  `phone::tzOffsetMin()`), to show LOCAL time with no phone after the first-ever sync (UTC until then); the
  CYD status-bar clock is **cyan when GPS keeps time**, white when phone-synced (the web icons use source
  dots — see STATUS `gav`/`tav`). With no module
  fitted the Status carries `gpsFix=0`/`gpsEpoch=0`, so behaviour is identical to phone-only.
  (`PROTOCOL_VERSION` 9→10, `sizeof(Status)`=28; both boards flash together.) **v11** (2026-10-23) adds
  `Detection.bssid[6]` (`sizeof(Detection)`=81) + the data-client flag bits — see the SD-log `bssid`/`data` notes.
- **C5 behavioral detectors (added in protocol v6; link now v9 — see onboard GPS above).** Alongside fingerprint capture the C5 decodes
  broadcast adverts/beacons and sets per-detection `DetFlags` bits (in `link_protocol.h`), all
  **receive-only**: `FLAG_BLE_IBEACON`, `FLAG_BLE_FINDMY` (Apple 0x004C mfr-data types 0x02 / 0x12),
  `FLAG_WIFI_PWNAGOTCHI` (beacon DE:AD:BE:EF IE / "pwnd"), `FLAG_WIFI_EVILTWIN` (same SSID from ≥2
  BSSIDs at **different encryption** — a small C5 SSID table, encryption-mismatch verdict to avoid
  roaming false-positives), and `FLAG_BLE_ODID` (OpenDroneID / Remote-ID **presence** — BLE 0xFFFA
  service data or the Wi-Fi vendor IE). **Full ASTM F3411 decode is IN PROGRESS** on branch
  `remoteid-decode`: `src/c5/remoteid.*` decodes the broadcast (UAS ID, drone + operator
  lat/lon, height/speed) from the 0xFFFA service data (increment 1 printed it to the C5 USB console, no
  protocol change). **Increment 2 (DONE — both boards flashed v8 + validated end-to-end):**
  `Reply::RemoteId` (0x91) + a 75-byte `RemoteId` struct → **protocol bumped to v8** (both boards flash
  together); the C5 emits one frame per active emitter inside its scan-dump burst (mutex-guarded
  `remoteid::snapshot()`), the CYD stores it keyed to a `Detection` by MAC (`g_rids[8]`) with a
  serial `[RID]` readout. Validated against the `tools/odid-tx/` bench transmitter (a spare ESP32-S3
  broadcasting ODID; see its README). **Increment 3 (DONE — CYD detail UI + NDJSON `rid` line):** the
  device-detail screen shows a `-- Remote-ID --` block (drone ID, drone + operator lat/lon, height,
  geo-alt, speed, heading — reused by the scanner detail AND the category drill-down), and a per-emitter
  `"evt":"rid"` NDJSON line is logged; the temporary serial readout was removed. CYD-only, no protocol
  bump. **Increment 4 (DONE — DETS RID block + web-app map):** the CYD appends a RID block to the DETS
  phone stream (v6, CYD→phone — no link bump), and the web app shows a Remote-ID section + plots the
  drone + operator positions (green drone marker, operator marker, connecting line) on the device-modal
  map and the Map page (live from the stream, and from logged `"evt":"rid"` lines). **Remote-ID decode
  is now complete (increments 1–4).** Plan: [docs/remoteid-decode.md](docs/remoteid-decode.md).
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
- **Nameless devices show a vendor, else their MAC** (not a generic `<hidden>`). The `groupName()` helper
  (`src/cyd/main.cpp`) returns a device's advertised name, or — when it has none — its **OUI vendor**
  (`sigdb::vendorFor()`, the `vendor,` DB rules; shown as `<vendor> AA:BB` so same-vendor devices stay
  distinct) for a real globally-administered MAC that isn't a BLE RPA, else its full MAC as text — so
  nameless devices stay individually identifiable everywhere a name is shown (live list, detail screens,
  Explore viewer, the DETS phone stream, and the web app). This is a **CYD-side display fallback**: the
  C5 sends empty names (no protocol involvement). The NDJSON
  log is unchanged — it still omits `name` when blank and carries `mac` separately; readers
  apply the MAC fallback at display time.
- **SD logging (CYD) — per-session NDJSON files, two parallel streams.** The log is **NDJSON**
  (one JSON object per line, `logNewDetections` in `main.cpp`). There are **two independent streams**
  (each a `LogStream` with its own dedup set): a **foreground** stream for on-screen scan sessions
  (`/logs/sess-NNNNN.jsonl`, renamed to `/logs/YYYYMMDD-HHMMSS.jsonl` on time sync) and a dedicated
  **background** stream for the always-on background scan (`/logs/bg-NNNNN.jsonl` → `/logs/bg-…`).
  The headless background cycle (any screen but `SCR_SCAN`) logs to the bg stream; the on-screen scan
  logs to the fg stream. **One scan = one file, and logging never stops:** each stream's dedup table
  (`MAX_SEEN`=400 per stream, `SeenKey` with `lastSeen`/`lastLogged`) is an **LRU** — when full, the
  least-recently-*seen* device is evicted (and simply logged again if it returns), so a dense drive
  never fills it. (Before 2026-10-22 a full set *skipped* new MACs and rotated to a new file at most
  every 5 min — data gaps + a 13-file drive; both removed.) A device is appended on first sight and
  **re-logged every `RELOG_MS` (5 min)** while still/again in range (fresh position + RSSI), so a
  file can hold several rows per MAC (web Explore collapses them to one device with a sighting trail;
  scan-analyzer takes many rows per MAC). The file only changes on a boot or "New Session".
  `LogStream.uniqCount` (devices first-logged this session; STATUS `uniq`, CYD `U:`) and `rowCount`
  replace the old `seenCount` readouts. One object per row; optional keys omitted when blank:
  `{"epoch":…,"ms":…,"lat":…,"lon":…,"src":"…","mac":"…","bssid":"…","rssi":…,"ch":…,"ie":"…","cid":"…","uuid":"…","pan":"…","name":"…","score":…,"tier":"…","sig":"…","wl":0|1,"flags":"…"}`.
  `src` is `2.4`/`5G`/`BLE`/`PRB`/`154`; `ie` the 8-hex IE fingerprint; `cid`/`uuid`/`pan` the BLE
  company ID / service UUID / 802.15.4 PAN; `name` JSON-escaped; `wl` 1 when whitelisted; **`flags`**
  is a `+`-joined list of behavioral-detector hits (`ibeacon`/`findmy`/`pwnagotchi`/`eviltwin`/`droneid`,
  plus the non-threat **`data`** = a `PRB` client heard in an associated DATA frame and **`dl`** = such a
  client heard only downlink, so its `rssi` is the AP's; omitted when none). **`bssid`** (v11, `PRB` rows
  with `data` only) = the AP that client talks to, from `Detection.bssid[]` (`FLAG_WIFI_DATA_CLIENT`/
  `_UPLINK`, bits 8/9; not notable: no tier floor, but carried in DETS `flags` + live-POST). Schema
  agreed with scan-analyzer 2026-10-23. Behavioral **EVENTS** are their own line shape with an `"evt"` discriminator,
  rate-limited to ~once/30 s per type: `{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"deauth","count":N}`
  and `{…,"evt":"eviltwin","count":N[,"ssid":"…","macs":"aa:bb:…,cc:dd:…"]}` (eviltwin also names the
  suspect SSID + its BSSIDs — up to 8, comma-joined — from the live detections carrying
  `FLAG_WIFI_EVILTWIN`, so the web Map's event-pin popup lists them and each BSSID opens that device's
  detail; deauth has no per-AP address, the C5 only counts frames). **Remote-ID** gets its own `"evt":"rid"` line (rate-limited
  ~once/30 s per emitter MAC, Remote-ID increment 3):
  `{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"rid","mac":"…","uas":"…","dlat":…,"dlon":…,"h":…,"alt":…,"spd":…,"hdg":…,"olat":…,"olon":…,"oid":"…"}`
  — `lat`/`lon` = the scanner's own position fix (phone GPS preferred, onboard C5 NEO-6M as fallback);
  `dlat`/`dlon` the drone position, `olat`/`olon` the
  operator position (deg), `h`/`alt` height/geo-alt (m), `spd` m/s, `hdg` deg, `uas`/`oid` the UAS ID /
  operator ID (JSON-escaped); unknown fields omitted. `epoch` is LOCAL time once synced (0 before); `lat`/`lon`
  once GPS is sent; **`fix_age`** (fw 0.13.0, every line that carries `lat`/`lon`, right after `lon`) = seconds
  since that position was current, present ONLY when it is a HELD last-known fix (no live phone or onboard GPS)
  or, from fw 0.14.0, when the fix in use is > 10 s old (a phone between sparse updates) —
  absent = live; `score`/`tier`/`sig` from the signature DB (tier also floored by any flag hit).
  **Follower** events: each device at follow tier 2 (FOLLOWING) gets an `"evt":"follow"` line, rate-limited
  ~once/30 s per MAC (`logFollowEvents`): `{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"follow","mac":"AA:BB:…","score":N,"ftier":2,"name":"…","src":"…","flags":"…"}`
  — `lat`/`lon` the scanner's own fix; `score` the 0..100 follow score; `name` (JSON-escaped), `src` (the
  strongest member's source tag, as in detection rows) and `flags` (the same `+`-joined detector list —
  tells a Find-My iPhone from a car device) are omitted when blank. The web Map pins it (magenta `F`).
  The follower + deauth/evil-twin alerts also show on the **HOME** screens: a CYD band at y=120 on HOME
  (tappable → follow drill-down) and the web banners, which live under the app bar so every page shows them.
  **Departure (presence) lines — `"evt":"lastseen"`** (fw 0.11.0, schema owner-approved with scan-analyzer
  2026-10-22): one per device presence period, for ALL device types, written when a present LRU entry has
  not been refreshed for `DEPART_MS` (60 s), when it is evicted from the LRU, and for every present entry
  on New Session (`beginSessionNamed` flush; a reboot loses them — accepted):
  `{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"lastseen","src":"…","mac":"…","pan":"…","first_ms":…,"last_ms":…,"rssi_max":N}`
  — `epoch`/`ms` = write time; `first_ms`/`last_ms` = first/last heard on the SAME boot clock (reader:
  `epoch − (ms − x)/1000`; `last_ms` may run up to the C5's 30 s TTL late); `rssi_max` the period's
  strongest RSSI; `pan` only for 802.15.4 short addresses. A returning device starts a new period.
  **File log only** (not DETS / live-POST). The web Map skips it (not a pin); joins/exports keep it.
  The per-stream `SeenKey` tables (24 B × `MAX_SEEN` × 2) are now **heap-allocated** in
  `beginSessionNamed` — they no longer fit the CYD's static DRAM.
  **Time backfill — `{"epoch":…,"ms":…,"evt":"timesync"}` anchor.** Rows logged before the phone syncs
  the clock carry `epoch:0` (only `ms`, ms-since-boot, is meaningful). When the phone sets the time,
  `renameStreamOnSync` appends ONE `timesync` anchor line (the sync epoch + its `ms`) to any stream that
  already holds pre-sync rows, then renames the file. The stored pre-sync rows are NOT rewritten (stays
  append-only); instead **readers backfill** their real time from the anchor: `epoch = sync.epoch +
  (row.ms − sync.ms)/1000` (same formula `phone::epochNow()` uses; one-boot-valid). The web app's
  `parse.js` does this in `parseScanNdjson`/`parseScanEvents` (`backfillEpoch`), so Explore/Map show
  real clock times for boot-scan rows. GPS can't be backfilled (no position existed pre-connection).
  **Why NDJSON:** append-only (a torn write loses only the trailing line), self-describing (schema
  grows without breaking old parsers), JSON escaping kills the comma/formula-injection bug class.
  Old `.csv` sessions still list/read (readers branch by extension; no timesync backfill for legacy CSV).
  **Scan provenance — `{"evt":"meta","scanner_id":…,"fw":…,"db_gen":…}` anchor.** `writeSessionMeta()`
  writes ONE `meta` line as the **first line of every stream** (fg + bg, in `ensureLogFile()`, before
  the first detection row — same slot as the timesync anchor) so a scan's origin travels with the file
  no matter who uploads it. `scanner_id` = `scannerIdHex()` (the 48-bit factory eFuse MAC as 12
  lowercase hex, stable across reflashes, OPAQUE — the base MAC, won't equal any row `mac`); `fw` =
  `FW_VERSION`; `db_gen` = the sig-DB `DB_GEN`. `scanner_name` is omitted for v1 (no per-unit label yet).
  Readers skip it by its `evt` (like timesync). Consumed by the scan-analyzer companion server (tags
  each session distinct from the uploader's device id); the web app also prepends the same record,
  learned from STATUS `sid`/`fw`/`dbgen`, to each live-POST batch. First-party (owner's own device).
- **Log retention — 512 MB cap with auto-rotation.** `enforceLogCap()` keeps total `/logs/` size
  under `LOG_CAP_BYTES` (512 MB) by deleting the **oldest** files (by FS timestamp), never the live
  fg/bg/Explore-held/currently-streaming file. Runs at boot, on each session rotation, and on a
  ~10 min loop timer (suppressed during downloads).
- **Log download + time-filtered export.** Download (BLE + Wi-Fi) serves the **current** stream
  (`webshare::setLogPath`, kept in sync with the active screen). A **time-filtered export** streams
  only matching lines, filtered **on the CYD** (two-pass, never the whole 512 MB file): BLE CMD
  `T:<mode>[:min][:path]` and HTTP `/dlf?f=&mode=&arg=` — mode `0` = since boot (whole file), `1` =
  past 24 h, `2` = custom minutes. Before the phone syncs time it falls back to `ms`-since-boot.
  **All-logs range export** (the web app's time-range download): path **`*`** (`T:<mode>[:min]:*`) /
  `/dlf?f=all` — `logfilter::MultiExport` collects the window from **every** `/logs/*.jsonl` (fg + bg,
  New Session, earlier boots) into ONE NDJSON stream: files picked by their tail epoch (cheap 1 KB
  head/tail reads), each file's pre-sync `epoch:0` rows **rewritten** with the epoch backfilled from
  that file's own timesync anchor (a live file with no anchor uses the current clock; undatable rows
  from old never-synced files are dropped; "this boot" = the live fg/bg files plus up to 8 earlier
  ones ended by New Session/bg toggle (`thisBootFiles()`), and `renameSessionOnSync` also appends a
  timesync anchor to those if they ended pre-sync; tail epochs only count complete numbers so a torn
  last line can't misdate a file; a short send pass is newline-padded to the SIZE= header), per-file meta/timesync lines dropped and ONE fresh meta
  line written first, files oldest-first (cap 64, newest kept). Mode 0 = since this boot (epoch ≥
  now − uptime). With no clock at all only this boot's live files are exported (ms cutoff).
  The **session list** (`Q` → `SESS=`) lines are `path\tsize\tflag\tfirstEpoch\tlastEpoch` (the
  file's dated span, 0 = unknown, `MultiExport::fileSpan`) so the phone can group parts into scans.
  Filtering lives in `src/host_common/logfilter.h`.
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
  ALPR, cameras (Ring/Verkada/Axis/Hikvision/Wyze/Nest/Reolink/SimpliSafe/…), Tile/Samsung/Google
  trackers, Meta/Snap camera glasses, Flipper/Pineapple, OpenDroneID, etc. (Nest/Reolink/SimpliSafe
  OUIs + the Arlo/Blink reweight were added 2026-10-10 straight from the IEEE MA-L registry.)
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
  manufacturer company ID); `thresholds,<suspect>,<likely>,<confirmed>`; and
  `vendor,<PREFIX>,<label>` (**display-only** OUI→vendor name used by `groupName()` as a fallback for
  nameless devices — no weight/srcmask, never scored; the `label` may contain commas). `srcmask`:
  W/B/4/A (a probe-request detection also matches W/A rules). Weights sum across the
  OUI + name + IE + BLE-UUID + company-ID layers (`vendor,` excluded). No on-device regex.
- Design + roadmap (Phases 2–6): [docs/signature-matching.md](docs/signature-matching.md).

### Display / UI (portrait)

- Rotation `0` (portrait 240×320). Top **status bar**: local time (left; **cyan when the onboard GPS
  is keeping time**, white when phone-synced), then a right-hand icon cluster — a **GPS location-pin
  glyph** (with a small **phone** or **chip** badge showing the fix source; **tap it for a GPS modal**
  of source + lat/lon + sats), a **battery** glyph, the **touch-lock padlock**, the **phone connection
  icon** (Bluetooth / Wi-Fi / circle-slash), and the **CYD↔C5 link dot** (solid green = up, red =
  down). Below it: SD/db line, per-band counts (2.4 / 5G / BLE / PRB / unique), a threats line, then
  detection rows sorted threat-tier-first then RSSI (tier colors: suspect=yellow, likely=orange,
  confirmed=red). The HOME menu title is the magenta **COINTEL-FLO** wordmark, and a **boot splash**
  shows the logo (`src/cyd/logo.h`, baked from `resources/logo/`). The selected-item/button accent is
  the logo magenta `#AA00AA` (`UI_ACCENT`).
- **Input: BOOT button + a touch overlay.** A short **tap** moves the highlight / selects a
  touched item; a long **hold** selects / returns toward the menu. A **bag-safe touch lock** rides in
  the status bar: a small padlock glyph (yellow=locked, grey=unlocked) that **2 quick taps toggle**
  (`serviceTouchLock()` in `main.cpp`, gated right after `buttonEvent()`; `LOCK_TAPS`/
  `LOCK_TAP_WINDOW_MS` tune the gesture); while locked, **all input (touch + BOOT) is swallowed**
  except that same 2-tap gesture, so a bagged device can't navigate
  itself — the background/live scan keeps running + logging throughout (discrete taps, not a hold, so
  bag pressure can't false-toggle). The HOME screen has **one list
  button, Scan**, plus two **title-bar icon buttons** on the `HOME` line: a **cog (Settings)** upper-
  left and a **phone (Phone Link)** upper-right — the phone icon is **grey when no phone is on the GATT
  link, green when connected**, and tapping it opens the full-screen web-app QR (`< BACK` top bar).
  Both icons stay in the BOOT nav (order: Scan → cog → phone → threat tiles) so Settings/touch-cal is
  never touch-only. The HOME screen also shows a **background-scan threat readout** at the bottom: the
  fixed **category taxonomy** as `NAME:n` tiles (Flock/Axon/ALPR/Cam/Ring/Raven/Glass/Tracker/
  Drone/Deauth/Flipper/Skim — **12 tiles, 4 rows; the `Other` catch-all is NOT tiled**), always shown
  (dim grey at 0; when >0 each tile is **coloured by its category hue** — the `kCatColor[]` RGB565
  palette approximating the web app's `CAT_COL`, so the readout matches the phone — plus a
  **tier-coloured threat-score badge** on the right showing the sigdb score, or the tier level 1-3 when
  the DB gave no score, so severity survives the category-hue switch; refreshed on the 2 s idle
  cadence). **Tapping a tile**
  (or BOOT-selecting it) opens that category's **device list** (`SCR_MENUCAT`, with a one-line "what
  signatures match" blurb) → a device → **detail** (`SCR_MENUCATDETAIL`, the scanner detail reused via
  `buildDeviceDetail`/`showDeviceDetail`). Categories are derived on the CYD (`categoryOf()` —
  behavioral flags first, then sigdb label → `g_category[]`; `groupCategory()`/`countCategories()` per
  device). **`Tracker` is follow-gated:** a tracker (Find-My flag or Tile/SmartTag label) only resolves
  to `CAT_TRACKER` while it is actually **FOLLOWING** (`isFollowing()`, ftier 2 ≥ `FOLLOW_SPAN_M`);
  otherwise it drops to `CAT_NONE` so ubiquitous idle AirTags/Tiles don't flood the readout. Since
  `groupCategory()` is the single source for the HOME count, the drill-down, and the DETS `cat` field,
  the phone/web breakdown mirrors this automatically. (A non-following tracker still keeps its Find-My
  Suspect tier floor on the live list — only the category tile is gated.) The live scan
  screen shows a **red deauth/evil-twin alert banner** (+ LED) when a flood or evil-twin AP is
  detected, alongside the follow-me banner.
  **Follow false-positive guard (`computeFollowScores`):** a **benign BLE Resolvable Private Address**
  — `macIsBleRpa(mac)` = MAC MSB top two bits `0b01`, the privacy-rotating address phones/watches
  advertise — with **no sigdb tier and no notable flag** never escalates to FOLLOWING (caps at
  PERSISTENT), so your own/companion phone stops tripping the follower alert/LED/`CAT_TRACKER`/STATUS
  `fol`. A planted Find-My/tracker device is exempt (it carries a flag/tier). CYD-only, no protocol
  bump. Proper long-term fix for signature-less RPA trackers is IRK/bonding (backlogged).
  **Scan** opens a **Scan menu** (`SCR_SCANMENU`: Start Scan / Explore Scan / Scan Settings) —
  the scan-flow redesign; the live scanner, the SD-log Explore Viewer (pick/filter/sort/detail; indexes one entry per device — newest row, strongest RSSI — up to 512 devices),
  per-band Scan Settings, the Phase-5 follow-me drill-down, and the whitelist manager are all
  screens in the `main.cpp` state machine. App screens use a full-width `< BACK` bar; menus keep
  the menu style. Scanning can also be started/stopped from the web app (CMD `G`/`X`).

### Phone link + web app (Web Bluetooth) — working

- **CYD BLE GATT peripheral** (`src/cyd/phone.*`, NimBLE) advertises as `COINTEL-FLO`,
  service `9a1e0000-…` with characteristics (the web app filters by **service UUID**, not name, so the
  advert name is cosmetic):
  - `…0001` TIME (write) — `"utcEpoch;tzOffsetMinutes"` (bare epoch also accepted)
  - `…0002` GPS (write) — `"lat,lon"`
  - `…0003` CMD (write) — `"1"/"0"` Wi-Fi download · `"R"` reload DB · `"L"` BLE download
    current session · `"G"/"X"` start/stop scan · `"N"` new log session · `"Q"` list sessions · `"F:<path>"` download
    a session file · `"D:<path>"` delete a session file (both restricted to `/logs/`; delete
    refuses the live session) · `"B:<0-100>"` set brightness · `"K1"/"K0"` lock / unlock the CYD
    display (the bag-safe touch lock, also toggled on-device by the 2-tap padlock) ·
    `"T:<mode>[:min][:path]"` **time-filtered export** (mode 0 = since boot, 1 = past 24 h,
    2 = custom minutes; optional `/logs/` path, or `*` = every log file joined; filtered on-device) · `"W…"` whitelist · `"E…"`
    whitelist-remove · **`"P:<t><g>"`** time / GPS source preference (fw 0.15.0; each digit 0 prefer phone /
    1 prefer device / 2 device only; NVS `cydui/tpref`,`gpref`; web Settings "Time & location sources")
  - `…0004` STATUS (read/notify) — `key=val;…`, sent as **two notifications** per push (a notify is capped at MTU−3 ≈ 252 B and the one-line form outgrew it, cutting `name=` + the AP creds): part 1 = the live counts/state, part 2 = `dl;gav;tav;tpref;gpref;sid;fw;dbgen;name;pos;pfa[;apssid;appsk;apurl]`; the web app merges keys across notifications (`dl` and the AP creds stay together; part 1's worst case is already ~252 B, so new keys go in part 2). Keys:
    `link,w24,w5,ble,prb,z,uniq,time,gps,dl,susp,lk,conf,db,scan,bri,src,wl,muted,fol,sess,deauth,evil,rid,bg,lock,bat,gpssrc,sats,tsrc,gav,tav,tpref,gpref,pos,pfa,sid,fw,dbgen`
    (**`gav`/`tav`** fw 0.14.0 = AVAILABLE position / clock sources, bit0 phone, bit1 device. Device GPS = a live
    onboard fix; device clock (0.15.0) = the onboard GPS is supplying time right now OR the RTC holds a valid
    time — i.e. "an on-device fallback is ready"; a disabled (device-only) phone reads unavailable. The web app
    bar draws them as two stacked dots right of the location / clock icon — blue phone on top, purple device
    below, grey when unavailable — and tints the icon with the source in use (`gpssrc`/`tsrc`; orange GPS
    icon = held; `renderSourceIcons()` in `js/ble/status.js`). **`tpref`/`gpref`** (0.15.0) = the source
    preference; the web Settings selects stay disabled until they arrive. **`pos`/`pfa`** (0.16.0) = the
    position the scanner is stamping on its log right now (`lat,lon`, empty with no fix) + the `fix_age` the
    log would carry (−1 = live); always sent so a merged stale `pfa` can't stick. The web app stamps
    **live-POST rows** (`features/remote.js`, adds `fix_age` when ≥ 0 — contract addition agreed with
    scan-analyzer 2026-10-24) and the device-modal **sighting trail** (live fixes only) with it via
    `scannerFix()` in `js/gps.js` — NOT the phone's last fix, which froze ~10.7 min of live rows on the
    2026-10-24 drive after screen-off. Older firmware: phone fix only while < 10 s old.)
    (`prb` = probe count, `z` = 802.15.4 count, `scan` = 1 while scanning, `bri` = backlight %,
    `src` = scan-source mask, `wl` = whitelist rule count, `muted` = devices muted now,
    **`fol` = devices currently flagged as FOLLOWING** (drives the web follower banner),
    `sess` = current session name, **`deauth`** = recent observed deauth/disassoc rate,
    **`evil`** = evil-twin SSID count, **`rid`** = Remote-ID emitters seen, **`bg`** = background
    scan on/off, **`lock`** = display/touch lock state 0/1, **`bat`** = battery % (−1 = no cell / on
    USB), **`gpssrc`** = active position source 0 none/1 phone/2 onboard/3 held last-known, **`sats`** = onboard-GPS
    satellites in use, **`tsrc`** = active time source 0 none/1 phone/2 onboard GPS (v10; `time` = 1
    when EITHER source has the clock); `gps` = 1 when EITHER phone or onboard has a LIVE fix (0 while held);
    **`sid`** = stable scanner id (eFuse base MAC, 12 hex lc — opaque), **`fw`** = firmware version
    string, **`dbgen`** = sig-DB generation — the **scan-provenance** fields the web app prepends as a
    `{"evt":"meta",…}` record on each live-POST batch; file uploads carry the same meta as line 1 of
    the NDJSON)
  - `…0005` LOGDATA (notify) — BLE log stream (`SIZE=<n>` header then raw chunks)
  - `…0006` DETS (notify) — **live detection list** mirroring the CYD screen, pushed each
    scan cycle. **v6 format:** a `D:<seq>:<count>` header then `<count>` tab-separated rows
    `seq\ttier\tmac\trssi\tie\tname\tsrcs\tftier\tfscore\tmuted\tflags\tcat` (top-of-list first, capped
    at 12). `name` is the advertised name, or the device's **MAC as text** when it has none (so
    nameless devices stay distinguishable — see `groupName()`). `ftier` = follow tier (0 none /
    1 PERSISTENT / 2 FOLLOWING), `fscore` = 0..100 follow
    score, `muted` = 1 when whitelisted, **`flags`** = decimal OR of the device's `DetFlags` bits
    (iBeacon/FindMy/Pwnagotchi/EvilTwin/ODID) for the web badges, **`cat`** = threat-category id
    (matches the CYD `Category` enum) driving the web category breakdown. **v6 appends a Remote-ID
    sub-block** after the detection rows, ONLY when ≥1 RID is held: a `R:<ridCount>` header then one
    row per emitter `mac\tuas\tdlat\tdlon\tolat\tolon\th\talt\tspd\thdg\toid` (drone + operator
    lat/lon decimal deg, height/geo-alt m, speed m/s, heading deg, UAS ID / operator ID; unknown
    fields empty) — the web app plots the drone + operator on the device-modal map and the Map page.
    Fields/blocks were appended across versions (follow in v3, `flags` in v4, `cat` in v5, the RID
    block in v6); older parsers read the fixed detection fields / `count` rows and ignore the extras,
    and the web parser skips any unrecognized line. The web app renders the rows as the Scanner page's
    **tall, tappable live list** (tier-colored, follow + flag markers; tap a row → device modal) + the
    threat strip/breakdown. (DETS versioning is the CYD→phone stream's own; it is NOT the CYD↔C5 link
    `PROTOCOL_VERSION`.)
  - **Discovery gotcha (fixed):** NimBLE 2.x has scan response OFF by default, and the
    128-bit service UUID fills the adv packet, so the name overflows. We call
    `enableScanResponse(true)` + set the name in the scan response, and the web app
    filters by **service UUID** (not name). Without this the phone finds nothing.
- **Web app** `webapp/index.html` — hosted at
  **https://negativeaffirmations.github.io/cyd-scanner/webapp/** (GitHub Pages). **Material-styled,
  framework-free, no build step.** The UI is a **sticky bottom navbar** (Material Symbols icons) over
  five in-page views — they're show/hide `.page` sections (toggled by `js/nav.js` `showPage()` via a
  `.visible` class), **not** navigations, so the BLE link stays up when switching. A persistent top
  **app bar** holds the title, the **time / GPS / DB status icons** (small; white when active, gray
  when not), and the **Bluetooth connection icon** (gray `bluetooth_disabled` / yellow searching /
  white connected). The five pages:
    - **Home** — one summary card: the connected **unit name** (STATUS `name=`, moved here from the app
      bar 2026-10-24) over the scan-type counts (4 centered columns; a column grays when its source is off
      in Scan Settings) and a short-label row **PRB · UNIQ · SUSP · LKLY · CONF** (labels never wrap);
      **tap the card → `#countsDlg`** "Scan summary" listing every count spelled out (`features/counts.js`,
      live-refreshed from `ble/status.js`; disabled sources read "off"); and the
      **Threats** readout: the full 12-category taxonomy as color-coded cards (**3/row × 4 rows**,
      matching the CYD HOME tile layout) —
      each card's **outline is its category colour at all times** (per-category palette **`CAT_COL`**
      in `js/config.js`; **reuse it everywhere categories appear**), grey fill at 0, tinted when >0;
      tap a card → that category's device modal. The narrow cards abbreviate two labels
      (`CAT_CARD_LABEL` in `categories.js`: Tracker→TRACK, Flipper→FLIP).
    - **Scanner** — a Material **icon control row** (Start/Stop ● `fiber_manual_record`↔`stop`,
      Filters, New session `note_add`, Explore `find_in_page`, Scan settings); a compact **threat
      strip** (one `CAT_COL` pill per category — tap the row → a breakdown modal of all categories +
      counts, active first); a "Scan" title + mode icon (`radar` live / `description` loaded); then
      the **live detection list** — **tall, full-width, tappable rows that flow with the page scroll**
      (not an inner pane; a jump-to-top button appears bottom-right once scrolled). Tap a row → a
      **device modal** (all info, per-source RSSI; + a GPS mini-map for loaded-log rows) with an **Add
      to whitelist** action (`A:mac,…`; a whitelisted device links to the Whitelist manager). The
      device modal also shows a **map at the bottom** of **where the device was seen**: for a live row
      this is a **sighting trail** computed in the webapp (`features/sightings.js`) — the DETS stream
      carries NO per-detection GPS, so each snapshot logs the **phone's own** current fix for every
      seen MAC (the same wardriving model as the Map page), plotted as a dashed trail (newest marker
      highlighted); it only fills while GPS is active and you move, and is in-memory/cleared on
      disconnect. For a loaded-log (Explore) row it's the single logged fix. The shared map renderer
      (`renderDetailMap` in `features/detail.js`) owns the one Leaflet instance on `#detMap`. The
      **Explore** viewer (pick a past session → filter/sort → same tall rows) also lives here; the
      **Filters** modal narrows the live list by source/threats.
    - **Map** — wardriving map (Leaflet/OSM): a session's GPS'd detections grouped by fix, colored by
      threat tier, filterable by source/threats, with **event pins** for `evt` (deauth/eviltwin/
      droneid) lines — from the current scan, a picked session, or a locally-loaded `.jsonl`/`.csv`
      (offline; the parser sniffs `{` for NDJSON vs legacy CSV). `openMap()` runs Leaflet
      `invalidateSize` on page-show (the div was display:none). The source/threats/events filters live
      in a **Filters modal** (`#mapFiltDlg`). The toolbar is a Scanner-style `.iconrow` of three
      `.iconbtn`s — **Map current session** (`radar`), **Load file** (`file_open`), **Filters**
      (`filter_alt`) — so it stays one line and the whole page fits without scrolling; `#map` height is
      `calc(100dvh − chrome)`. **`#map` sets `position:relative; z-index:0`** to make its own stacking
      context — otherwise Leaflet's internal panes/controls (z-index up to ~800) paint OVER the fixed
      bottom navbar (z-index 20).
    - **Data** — download current session (BLE `L`), Wi-Fi download mode, **time-range export of ALL
      logs** (since boot / 24 h / custom; BLE `T:…:*` or Wi-Fi `/dlf?f=all`), and the **Session logs**
      list: a checkbox **multi-select of scans** (`features/sessions.js` groups same-stream files whose
      dated spans are ≤ 6 min apart — older firmware split drives into parts — labelled "Oct 22, 2:05 PM
      – 3:09 PM · Scan · 13 parts · 914 KB"). Download / time-range / Map act on the selection; parts
      are fetched one at a time (`ble/transfer.js` `bleFetch`, a promise over the LOGDATA stream) and
      joined by `parse.js` `joinLogParts` (per-part timesync backfill; each distinct meta line kept in part order, since parts can span a reflash and readers take the last) into ONE file;
      Upload sends ONE `/api/v1/uploads` body per scan; Delete queues `D:` per file. The Explore picker
      lists the same groups.
    - **Settings** — Sync time & GPS, Reload signature DB, brightness, Whitelist manager.
  Also present: a magenta **follower UI** (banner from STATUS `fol` + per-row follow markers + a
  follower-detail modal with a Leaflet track map) and a **red deauth/evil-twin alert banner** (STATUS
  `deauth`/`evil`) + behavioral-flag badges on rows. On connect it takes a **Screen Wake Lock**
  (`js/wakelock.js`, re-acquired on `visibilitychange`) so the page stays foreground and keeps pushing
  GPS — Android Chrome freezes a screen-off page, silently stopping GPS mid-walk. MUST be HTTPS; **Chrome
  on Android only** (Web Bluetooth + geolocation; no iOS Safari).
  **Styling: SCSS** under `webapp/scss/` (theme in `_variables`; partials `_base`/`_nav`/`_detections`/
  `_banners`/`_map`) → `webapp/style.css` via Dart Sass (`npm run sass:build` or the "Sass: watch" VS
  Code task). **Edit the SCSS**, not the generated-but-committed `style.css` (Pages doesn't run Sass;
  `node_modules/` git-ignored). **Deploy caching:** GitHub Pages serves every asset (HTML included)
  with `cache-control: max-age=600`, which overrides the HTML's `<meta http-equiv>` no-cache — so a
  fresh push can look unchanged on an already-loaded phone for ~10 min. **BUMP `style.css?v=<token>`
  whenever `style.css` changes, and BUMP the `#uiver` footer string on EVERY webapp deploy** (the
  human "is the new build live?" readout; once it flips, that deploy's JS is live too). JS modules
  can't be token-versioned (no build step) — they ride the 10-min revalidation. See HANDOFF "Webapp
  cache / deploy-propagation". **JS = native ES modules** (`<script type="module" src="js/main.js">`,
  no bundler): `config.js`, `state.js` (the one cross-module mutable-state object — ESM exports are
  read-only live bindings), `nav.js`, `wakelock.js`, `util/` (`dom`,`format`), `ble/` (`connection`,
  `status`,`commands`,`download`), `gps.js`, `data/parse.js`, `features/` (`detections`,`categories`,
  `followers`,`explore`,`sessions`,`whitelist`,`map`,`detail`), `main.js` (**all** DOM wiring via
  `addEventListener` — no inline `onclick=`, since module scope isn't global). Module-private state
  stays local; imports form a DAG.
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

## Companion project + remote-server upload (PLANNED — new direction)

A separate, **private** companion project — **scan-analyzer** — is being built to be the long-term
*memory/analysis layer* for the scans this device produces: a self-hosted server + web app that
ingests the NDJSON logs, stores every GPS/time-stamped sighting in a database, and **learns over
time** — estimating the fixed location of **stationary** emitters (a camera + its infrastructure
reappearing at one spot) from many sightings, enriching devices (OUI → vendor; **BLE SIG company ID**
→ manufacturer + a research layer on likely device models), and plotting everything on a
wardriving-style map (GPS + time + source/threat filters). It realizes the aggregate-triangulation
idea deliberately kept *off* the handheld. **Its code may be public; its data and server are not** —
captured RF data (bystander identifiers + GPS) must never be committed anywhere, here or there. The
proposal + synthetic test data live in this repo's **git-ignored `scan-analyzer/` folder** (for
transfer to the new repo), not in version control.

**What THIS repo must add to connect to it (web app, planned):**
- **No hard-coded server address.** The scan-analyzer web app shows a **QR code** encoding a deep
  link into THIS web app carrying the server URL (+ a pairing token), e.g.
  `…/cyd-scanner/webapp/?server=<url-encoded>&token=<pairing-token>`; scanning it opens the web app
  pre-paired (store server + token in `localStorage`). **Fallback:** a **"Remote server"** field in
  **Settings** to paste the URL/token (the QR page also shows them as copyable text), for phones
  whose camera QR scan can't deep-link into the web app.
- **Live upload (later phase):** once paired, the web app POSTs live detections to the server during
  a scan (Bearer = the per-device key exchanged from the pairing token). A robust background live
  feed likely needs a **standalone phone app** eventually (a browser tab can't reliably hold BLE +
  geolocation + network in the background); web-app live-POST is the first cut.
- **HTTPS is mandatory** for the server URL: this web app is served over HTTPS, so browsers block
  mixed content — an HTTP server won't be reachable (same constraint as the SoftAP note above). The
  private server must be exposed over HTTPS (Tailscale / Cloudflare Tunnel / LAN cert).
- Near-term, uploading is just **downloading a session log and uploading the file** to the
  scan-analyzer web app (no THIS-repo change); the pairing + live-POST work above is the later phase.

**Hardware / modality expansion (exploratory, beyond RF).** The device is RF-only today, but the
intent is to gather **other signals** — either by **adding boards** to the current rig or building
**purpose-built sibling setups** — all feeding the same scan-analyzer DB/map:
- **Camera / optical ALPR detection:** a camera sub-board modeled on **Flock-Noir** (MIT — see
  [docs/references.md](docs/references.md); Seeed **XIAO ESP32-S3 Sense** + OV2640/**OV3660** + an
  OPT101 light sensor) that flags ALPR/Flock cameras via **IR-flash pulse timing + camera brightness
  + RF** (not CV object recognition). Owner has the hardware (newer OV3660) and is testing viability
  before committing to integrate.
- **Geotagged image capture + upload** (useful even if auto-ALPR detection is unreliable) and
  **on-device vision** (the XIAO S3 Sense can run lightweight ML object detection) as future
  detection inputs.
- **Device notes:** free-text annotations on any device seen — authored from the **phone web app**
  (the handheld has no keyboard), persisted/aggregated in scan-analyzer.
- **Bluetooth Classic (BR/EDR):** not possible on the current boards (C5 has no BR/EDR; the CYD's
  ESP32 is held by the NimBLE phone link — mutually-exclusive host stacks), so a **new board / second
  setup** is required to catch e.g. a car head unit's friendly name.
These are **future/exploratory** — not started. Any code ported from Flock-Noir or similar follows
the [docs/references.md](docs/references.md) license+attribution directive.

**Cross-project coordination:** a **local, git-ignored `REMOTE.md`** in this repo root describes a
private bridge for this project's Claude session to exchange messages with — and share sensitive
logs/serial output with — the scan-analyzer project's session. **Read `REMOTE.md`** when coordinating
anything shared across the two projects (log schema, DETS, the upload/pairing API, test fixtures).
It's git-ignored and holds the machine-local bridge path, so nothing committed here names an external
location. **While building the remote-server integration, keep HANDOFF #11 and the bridge updated each
work session** (what landed / what's next) so a fresh session can resume mid-build if one runs out of
context or usage. (The API contract is owner-approved at DRAFT v0; build is deferred — see HANDOFF #11.)

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

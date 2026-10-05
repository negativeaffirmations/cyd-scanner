# Handoff — 2026-10-05 (branch: phase5-dev)

## Branches / deploy
- **phase5-dev** = active dev branch (firmware + webapp). Committed; both boards flashed.
- **main** = GitHub Pages deploy branch (source: `main` `/`). Gets **webapp-only** deploy
  commits; it lags phase5-dev on firmware. Web app is live at
  https://negativeaffirmations.github.io/cyd-scanner/webapp/
- To deploy the web app: put `webapp/index.html` on `main`, push, Pages rebuilds
  (`git checkout main && git checkout phase5-dev -- webapp/index.html && commit && push`,
  then `git checkout phase5-dev`). Check build: `gh api repos/.../pages/builds/latest --jq .status`.
  ⚠️ **The webapp changed a lot this session** (deauth/evil banner, flag badges, event map pins,
  time-range export UI) — it has NOT been deployed to `main` yet.

## Done this session (committed on phase5-dev; BOTH BOARDS FLASHED + boot-verified)
Newest first:
- `a3fc96f` **c5: latch deauth peak** (reviewer REV-001) — deauth count held ~10 s so a CYD poll
  between promiscuous windows still catches a flood (was reading 0 most of the time).
- `e393cad` **CYD logging overhaul** — bg scan **default ON** (+ one-time NVS force-flip);
  dedicated `/logs/bg-*.jsonl` stream (two dedup sets); 512 MB total-/logs/ auto-rotation
  (oldest-first, never live/held/streaming); **time-filtered export** CMD `T:<mode>[:min][:path]`
  (0 since-boot / 1 past-24h / 2 custom-min) + HTTP `/dlf`, filtered on-device; `logfilter.h` (new).
- `391bbc4` **CYD detector integration** — flag→tier/label, NDJSON `flags` key + `evt` event lines,
  DETS **v4** (trailing flags field), STATUS `deauth=/evil=/rid=`, **red banner + LED alert** for
  deauth/evil-twin, detail-screen flags row; webapp banner/badges/event-map-pins/time-range UI.
- `cfe5930` **C5 passive detectors** — iBeacon, AirTag/Find-My, Pwnagotchi (DE:AD:BE:EF IE + "pwnd"),
  evil-twin (encryption-mismatch verdict, SSID table), deauth/disassoc counting, OpenDroneID
  **presence** (BLE 0xFFFA + Wi-Fi vendor IE). All RECEIVE-ONLY.
- `f79c702` **link: protocol v6** — `Detection.flags` → uint16 (67 B) + 5 detector bits;
  `Status` (12 B) + `deauth_recent`/`evil_count`/`rid_count`. **Both boards must be v6 together.**
- `c310b98` **CYD: follow thresholds restored** to production (400 m / 3 fixes / 150 m).
- `97a1fc7` review fixes (FEAA weight 40→15, bg link-down backoff).
- `c1e6649` **GPL-3.0 LICENSE** + SquachWatch attribution (`docs/references.md`).
- `26b70ee` **signature roster** from SquachWatch (Axon/ALPR/cameras/Ring/trackers/glasses/
  Flipper/drones); `MAX_OUI` 64→96; DB_GEN 2 (reseeds `/signatures.csv` on boot).

Device state: **CYD (COM14, CH340 1A86:7523) and C5 (COM18, native 303A:1001) both flashed with v6.**
Boot-verified: `link=up`, both log streams created, background scan self-started and logging, sigdb
loaded 62 OUI + 14 name + 15 bleuuid + 7 blecid. Reviews: security PASS, reviewer no P1.

## PICK UP HERE — outstanding
1. **Deploy the webapp** to `main` (big changes this session, not yet on Pages — see above).
2. **SEC-M1 (pre-existing, now more exposed):** the Wi-Fi download SoftAP derives its WPA2 PSK from
   the efuse MAC, which is the broadcast BSSID → anyone in RF range can compute it and pull all
   `/logs/` (bystander MACs + GPS) via `/dl` + the new `/dlf`. Fix: random per-device PSK in NVS,
   shown only on the device/QR. More relevant now that bg logging is always-on.
3. **Phase 2 — full Remote-ID (ASTM F3411) decode:** this round only flags OpenDroneID *presence*.
   Full decode = operator/drone lat-lon + UAS ID via a new `Reply::RemoteId` frame + a CYD event/
   map list (another coordinated both-board flash). Heaviest piece; BLE-extended-scan coexistence
   risk — check against the BLE/15.4 coexistence rule.
4. **P3 follow-ups:** bg-stream rotation min-interval/min-rows guard (dense-drive thrash);
   pre-time-sync `enforceLogCap` eviction order comment; `deleteSession` could also check
   `g_streamingPath` (defense-in-depth).
5. **Docs:** CLAUDE.md CMD list / STATUS keys / DETS version / logging + detector sections are now
   stale (T: command, bg stream, v6 flags, deauth/evil-twin) — update to match.
6. Older backlog: 802.15.4 opt-in, phone/desktop SQLite import, strict BLE passivity
   (`setActiveScan(true)` is on), CYD Scan Viewer window caching.

## Flash / tooling notes
- `pio` is not on the bash PATH: use `"$HOME/.platformio/penv/Scripts/pio.exe"`.
- Always `PYTHONIOENCODING=utf-8 pio run -e <env> -t upload` (upload hangs otherwise on Windows).
- **Identify boards by VID:PID, not COM** (numbers drift; `pio device list`):
  CYD = CH340 `1A86:7523` (COM14 this session); C5 native USB = `303A:1001` (COM18, **flash this one**);
  C5 UART bridge = `2E3C:5740` (COM8, does NOT flash). Pass `--upload-port COMnn` — auto-detect
  grabs the wrong board otherwise.
- **CYD first upload often fails** "Wrong boot mode detected (0x13)" — just **retry**, it works on
  the 2nd try (no BOOT/RST press needed; device is mounted).
- Boot-log capture (no reset-on-open issues): `scratchpad/boot_capture.py` pulses a run-mode reset
  on COM14 via pyserial and reads ~9 s. Use pyserial, not `pio monitor`.
- C5-only changes don't need a CYD reflash **while PROTOCOL_VERSION is unchanged**. v6 changed it,
  so this round both were flashed together.
- C5 5V pin must be unplugged while its USB is connected (two-5V-source risk); user confirmed safe.

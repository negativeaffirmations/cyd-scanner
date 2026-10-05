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
- `b0b6bc7` **docs: CLAUDE.md synced** to the v6 code (detectors, logging/export, SEC-M1, UI, GPL).
- `c8b27c3` **CYD: HOME-menu background threat readout** (`S:/L:/C:`, white at 0, tier color when >0,
  live on the 2 s idle cadence). ⚠ user wants this reworked — see PICK UP HERE #2.
- `32820c2` **CYD: SEC-M1 fixed** — random 16-char WPA2 SoftAP PSK generated once + stored in NVS
  (`"cydscan"`/`"appsk"`), shown only on device/QR; no longer derived from the MAC/BSSID. (Phones
  with the old network saved must rejoin via QR.)
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
⚠ **But in use the user reports the C5 link drops/reconnects ~every 2 s** (the brief boot capture
didn't surface it) — see PICK UP HERE #1.

## PICK UP HERE — outstanding

### ⚠ USER-REPORTED (observed on-device 2026-10-05 — TOP PRIORITY, not yet addressed)
1. **BUG — C5 link disconnects & reconnects every ~2 s.** User observes the CYD↔C5 link dropping and
   coming back roughly every 2 seconds in normal use. **This needs to be fixed.** (Not caught by the
   brief post-flash boot capture, which showed `link=up`.) Likely tied to **background-scan default-ON**:
   the headless bg cycle now polls the C5 every ~2 s on *every* screen, and on the HOME menu that runs
   *alongside* the menu's own ~2 s `pingC5` — the reviewer explicitly flagged this redundant/overlapping
   double-poll. Suspect the link-status/stickiness path (`g_linkOk`, `LINK_STICKY_MS`, the busy-miss
   bail in `requestScan`) is flickering under the doubled polling, or a busy-window phase-lock like the
   earlier `c5-busy-window-link-drop` issue. First moves: de-dupe the HOME-menu ping vs the bg poll so
   only one poll runs per window; confirm a busy-miss isn't resetting link stickiness. **May also cause
   #3.**
2. **UI — rework the HOME-menu threat readout.** Make the threat **numbers smaller**, and put a
   **title above each number**, spelled out or more readably abbreviated (e.g. Suspect / Likely /
   Confirmed — clearer than the current inline `S:/L:/C:`). Current impl: `drawMenuThreats()` in
   `src/cyd/main.cpp` (font-4 numbers, inline `S:/L:/C:` labels, band ~y248–292, refreshed on the
   SCR_MENU 2 s idle cadence).
3. **INVESTIGATE — possibly missing some threats** (user unsure). Verify threat/detection counts are
   complete. Could be a genuine gap, OR a side effect of #1 — if the link drops every ~2 s the C5's
   detection burst is lost on the dropped poll, so fewer devices/threats surface. Re-check after #1 is
   fixed; also sanity-check the flag→tier floor, `countTiers` (whitelist exclusion), and per-stream
   dedup aren't hiding threats.

### Other outstanding
4. **Deploy the webapp** to `main` (big changes this session, not yet on Pages — see above).
   `phase5-dev` is also unpushed (15+ commits ahead of origin).
5. **Phase 2 — full Remote-ID (ASTM F3411) decode:** this round only flags OpenDroneID *presence*.
   Full decode = operator/drone lat-lon + UAS ID via a new `Reply::RemoteId` frame + a CYD event/
   map list (another coordinated both-board flash). Heaviest piece; BLE-extended-scan coexistence
   risk — check against the BLE/15.4 coexistence rule.
6. **P3 follow-ups:** bg-stream rotation min-interval/min-rows guard (dense-drive thrash);
   pre-time-sync `enforceLogCap` eviction order comment; `deleteSession` could also check
   `g_streamingPath` (defense-in-depth).
7. Older backlog: 802.15.4 opt-in, phone/desktop SQLite import, strict BLE passivity
   (`setActiveScan(true)` is on), CYD Scan Viewer window caching.

*(Done this session, were previously listed here: SEC-M1 random-PSK fix → `32820c2`; CLAUDE.md
docs sync → `b0b6bc7`.)*

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

# Handoff — 2026-10-05 (main == phase5-dev, merged at `f025557`)

## Branches / deploy
- **2026-10-05: phase5-dev was MERGED into `main`** (commit `f025557`); both branches now hold the
  full firmware + webapp + tooling and point at the same commit. `main` is no longer "webapp-only."
  Follow/tail thresholds are at **production** (400 m / 3 fixes) — the old 50 m/2 fix TEST values
  were already restored in `c310b98`.
- **main** also serves **GitHub Pages** (source: `main` `/`, `.nojekyll` → static, no Jekyll). Web
  app live at https://negativeaffirmations.github.io/cyd-scanner/webapp/
- **Web app styles are SCSS** (`webapp/scss/*.scss` → `webapp/style.css` via Dart Sass; `npm install`
  once, then `npm run sass:build`, or the auto-starting **"Sass: watch"** VS Code task). Edit the
  SCSS (theme colors in `webapp/scss/_variables.scss`), **never** `style.css` (generated). `style.css`
  **is committed** (Pages doesn't run Sass); `node_modules/` is not.
- To deploy the web app now that main carries it directly: `npm run sass:build`, commit
  `webapp/` on `main`, push — Pages rebuilds. (No more cherry-pick from a separate dev branch.)
  Check build: `gh api repos/.../pages/builds/latest --jq .status`. ⚠ A Pages build was queued
  behind a **GitHub Actions `degraded_performance`** incident at merge time — the live webapp keeps
  serving the last-good version until it clears.

## Done — active-BLE-scan multi-UUID + category readout + HOME redesign (phase5-dev; both boards flashed v7)
Newest first:
- `a50ad3a` **CYD: fixed the HOME link-dot flicker** (the "C5 link disconnect every ~2 s"). It was
  **cosmetic** — dual-serial capture showed the data link streaming cleanly (`[C5] streamed ~50` +
  `table=~49` every 2 s). Cause: the HOME idle handler set `g_linkOk = pingC5()` **non-stickily**
  every 2 s *alongside* the background-scan poll (which already keeps the link sticky); a ping that
  collided with the C5's scan-stream busy window timed out → dot red one cycle → green next. v7's
  larger frames (75 B × ~50) lengthened that window enough to make it recur. Fix: on HOME skip the
  redundant ping while `g_bgScan && g_scanActive`, and apply `LINK_STICKY_MS` grace when bg scan is
  idle. User confirmed steady on-device. (Lesson: link-health must be sticky — never set `g_linkOk`
  from one ping.)
- `d931fbf` **c5+cyd: multiple BLE service UUIDs — protocol v7.** The active BLE scan now
  captures up to `SVC16_MAX` (4) extra 16-bit service UUIDs per device into `Detection.svc16[]`
  (unioned across advert + scan-response reports); `sigdb::score()` matches each (expanded to its
  128-bit base) against `bleuuid` rules, so a signature UUID is caught even when not advertised
  first. Device detail shows a "More UUIDs" line. `Detection` 67→75 B — **both boards must be v7**
  (flashed + link-verified: `table=43 (2.4:13 5G:10 BLE:20)`, db loaded). Scan timing already ~99%
  duty (interval 100/window 99), so untouched. (Passive-only directive was removed earlier — active
  scanning is the sanctioned behavior.)
- `3374917` **CYD: HOME title icons nudged down** for status-bar padding (`HOME_ICON_CY` 34→44).
- `7ba209b` **CYD+web: HOME title-bar icons + centered last threat row + category blurbs.** Phone
  Link → **phone icon upper-right** (grey=no GATT phone, green=connected); Settings → **cog icon
  upper-left**; list now holds just **Scan**. Both icons stay BOOT-reachable (nav: Scan, cog, phone,
  chips) so Settings/touch-cal is never touch-only. Partial last threat row is centred (`chipRect()`
  shared by draw+hit-test). Category device list shows a one-line **signature-type description**
  (`catDescription` / web `CAT_DESC`), which also explains the vague "Other".
- `6091880` **CYD: HOME readout = full category list always** — every category as a `FLOCK: 0` /
  `AXON: 0` tile (3×5 grid, font 1), dim at 0, tier-coloured + outlined when >0, all tappable.
- `a331ffe` **CYD+web: SquachWatch-style category readout + drill-down.** New `Category` taxonomy
  (Flock/Axon/ALPR/Cam/Ring/Raven/Glass/Tracker/Drone/Deauth/Flipper/Skim/Other), `categoryOf()`
  (flags-first then sigdb label) → `g_category[]`; `groupCategory()`/`countCategories()` per device.
  HOME chip → `SCR_MENUCAT` device list → `SCR_MENUCATDETAIL` (reuses the scanner detail via
  `buildDeviceDetail`/`showDeviceDetail`; both new screens are in the `inDrill` freeze set).
  **DETS v5**: trailing category id (older parsers ignore it); web app mirrors the whole breakdown.

## Done earlier this session (committed on phase5-dev; BOTH BOARDS FLASHED + boot-verified)
Newest first:
- `b0b6bc7` **docs: CLAUDE.md synced** to the v6 code (detectors, logging/export, SEC-M1, UI, GPL).
- `c8b27c3` **CYD: HOME-menu background threat readout** (`S:/L:/C:`) — superseded by the category
  readout above (`a331ffe`→`3374917`).
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

Device state: **both boards flashed with protocol v7** — CYD (COM14, CH340 1A86:7523) + C5 (COM18,
native 303A:1001). Link verified up post-flash (`table=43 (2.4:13 5G:10 BLE:20)`, db loaded). Category
drill-down + HOME icon redesign verified on-device by the user. Ports drift — identify by VID:PID.

## PICK UP HERE — outstanding

### Resolved / superseded (were the top items on 2026-10-05)
1. **~~BUG — C5 link disconnects & reconnects every ~2 s~~ — ACTUALLY FIXED (`a50ad3a`).** It recurred
   after the v7 flash and was root-caused: a **cosmetic** HOME link-dot flicker, not real data loss
   (serial showed clean streaming throughout). The HOME idle handler's non-sticky `pingC5()` collided
   with the background-scan poll's busy window. Fixed by de-duping the ping and making it sticky (see
   the Done section + [[c5-busy-window-link-drop]]). User confirmed steady.
2. **~~UI — rework the HOME-menu threat readout~~ — DONE, expanded.** Became the full SquachWatch-style
   **category readout** (per-type `NAME: n` tiles) + tap-through device list/detail + the HOME title-icon
   redesign (see the Done section above). Both CYD + web app.
3. **~~INVESTIGATE — possibly missing threats~~ — likely moot.** Was tied to #1 (a dropped poll loses the
   C5 burst). With the link steady and the new per-category counts visible, re-check if anything still
   looks low; no evidence of a gap now.

### Other outstanding
4. **Pages deploy pending a GitHub incident.** The SCSS/split-webapp + `.nojekyll` commits are on
   `main`, but a Pages build was queued behind **GitHub Actions `degraded_performance`** (2026-10-05).
   The live webapp keeps serving the last-good version until it clears; verify with
   `gh api repos/.../pages/builds/latest --jq .status` and `curl .../webapp/style.css` (expect 200).
5. **Remote-ID DECODE (ASTM F3411)** — the OpenDroneID/Remote-ID feature's *own* "phase 2" (NOT the
   signature-matching Phase 2, which shipped). Today the C5 only flags *presence* (`FLAG_BLE_ODID`).
   Full decode = operator/drone lat-lon + UAS ID via a new `Reply::RemoteId` frame + a CYD event/map
   list (another coordinated both-board flash). Heaviest remaining feature; BLE-extended-scan
   coexistence risk — check against the BLE/15.4 coexistence rule. Backlogged (user: leave it).
6. **Phase-5 follow-up:** the rotating-RPA phone self-filter guard (the `<hidden>` false positive).
7. **P3 log follow-ups:** bg-stream rotation min-interval/min-rows guard (dense-drive thrash);
   pre-time-sync `enforceLogCap` eviction order comment; `deleteSession` could also check
   `g_streamingPath` (defense-in-depth).
8. Older backlog: 802.15.4 opt-in polish, phone/desktop SQLite import, CYD Scan Viewer window caching.
   (The "strict BLE passivity" item is **dropped** — the passive-only directive was removed; active
   scanning is sanctioned, and its extra UUIDs are captured + matched as of v7.)

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

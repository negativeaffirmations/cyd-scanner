# Handoff — 2026-10-10 (single-branch: everything on `main`; phase5-dev retired)

## Branches / deploy
- **2026-10-09: `phase5-dev` is RETIRED.** `main` was fast-forwarded to `f44df6a` (picking up the
  post-merge work that had accumulated only on phase5-dev — C5 board photos, wiring diagrams, the
  Adobe-ignore rules, and the on-board **battery power subsystem** docs) and pushed; then
  `phase5-dev` was deleted locally and on the remote. **`main` is now the only branch.** Start the
  next dev round with a fresh `git checkout -b <name>` when needed.
- **2026-10-05: phase5-dev had been MERGED into `main`** (commit `f025557`); both held the full
  firmware + webapp + tooling. `main` is no longer "webapp-only." Follow/tail thresholds are at
  **production** (400 m / 3 fixes) — the old 50 m/2 fix TEST values were already restored in
  `c310b98`.
- **main** also serves **GitHub Pages** (source: `main` `/`, `.nojekyll` → static, no Jekyll). Web
  app live at https://negativeaffirmations.github.io/cyd-scanner/webapp/
- **Web app styles are SCSS** (`webapp/scss/*.scss` → `webapp/style.css` via Dart Sass; `npm install`
  once, then `npm run sass:build`, or the auto-starting **"Sass: watch"** VS Code task). Edit the
  SCSS (theme colors in `webapp/scss/_variables.scss`), **never** `style.css` (generated). `style.css`
  **is committed** (Pages doesn't run Sass); `node_modules/` is not.
- **Web app JS is native ES modules (2026-10-10, `17c8325`)** under `webapp/js/`, loaded by
  `<script type="module" src="js/main.js">`. **No build step, no bundler** — the browser resolves
  the imports and Pages serves them as-is, so unlike the SCSS there's nothing to compile or commit
  downstream: **edit a `.js` and commit it.** Shared mutable state (anything two modules both read
  and write) lives in `js/state.js` (ESM exports are read-only live bindings); module-private state
  stays local. All DOM wiring is `addEventListener` in `js/main.js` — **no inline `onclick=` in the
  HTML** (module scope isn't global). See the full layout in CLAUDE.md's "Phone link + web app".
- To deploy the web app now that main carries it directly: `npm run sass:build`, commit
  `webapp/` on `main`, push — Pages rebuilds. (No more cherry-pick from a separate dev branch.)
  Check build: `gh api repos/.../pages/builds/latest --jq .status`. ⚠ A Pages build was queued
  behind a **GitHub Actions `degraded_performance`** incident at merge time — the live webapp keeps
  serving the last-good version until it clears.

## Done — 5 GHz promiscuous sweep (2026-10-10, C5-only, FLASHED + bench-verified)
Added a second promiscuous sub-phase so associated **5 GHz** cameras are caught (previously 2.4-only).
- New `PH_PROMISC5` phase runs right after the 2.4 `PH_PROMISC` window (same radio, BLE stays paused —
  `blePaused()` now covers it). Sweeps common US non-DFS 5 GHz channels
  `{36,40,44,48,149,153,157,161,165}` (~160 ms dwell, ~4 s). DFS 52–144 skipped (an unassociated STA
  often can't passively park there; consumer cams rarely use them). `promisc::setChannel()` needed NO
  change — `esp_wifi_set_channel(primary, NONE)` switches band by channel number on the C5 (confirmed
  in the IDF header: attention-6 + the line-1706 note). Refactored the window-exit into a shared
  `finishPromisc()`; gated on `MASK_WIFI5`.
- **Bench-verified:** `promisc 2.4 end: mgmt=137 data=24 dataClient=2` and `promisc 5G end: mgmt=10
  data=0 dataClient=0` — the 5 GHz beacons prove the sweep tunes + captures; data=0 only because no
  5 GHz client was streaming at the bench. Diagnostic lines are now split per band ("promisc 2.4 end"
  / "promisc 5G end").
- **Cycle is now longer** (~AP-scan 10 s + 2.4 promisc 4.5 s + 5 GHz promisc 4 s + gap 3 s ≈ 21 s), so
  each band's sweep comes around ~every 21 s — on a walk, **pause ~20–30 s near a target** so both
  bands sweep while in range. C5 FLASHED (COM18). CYD unchanged. No protocol bump.

## Done — FIXED promiscuous RX starvation (2026-10-10, C5-only, FLASHED + bench-verified)
**The client-capture path (probe requests AND the new data-frame clients) was capturing almost
nothing** — root-caused on the bench with both boards on USB. Field test: user saw only 2 "Ring"
hits and a known camera 30 ft away (clear LOS) was never detected.
- **Diagnosis:** added per-window RX counters to `promisc.cpp` (`frameStats()`, printed at window end
  by `main.cpp`). First capture: `promisc window end: mgmt=2 data=0 dataClient=0` and CYD `PRB:0`
  every cycle. AP scan worked (2.4:11 5G:11) and BLE worked (20) — only the PASSIVE promiscuous RX was
  dead. Cause: the continuous NimBLE scan (~99% duty, interval100/window99) starves the single 2.4 GHz
  radio's passive RX; active AP scan survives because it gets coex priority. Same starvation the repo
  already documented for 802.15.4 ([[ieee802154-optin-ble-coexistence]]).
- **Fix 1 (the big one): pause BLE during the promiscuous window** (mirrors the 15.4 handling). New
  `blePaused()` = `PH_PROMISC || PH_154`; every BLE (re)start path (onScanEnd, applyBleMask, watchdog,
  the window-exit) now gates on it; `enterPromisc()` stops BLE, window-exit restarts it (unless 15.4
  takes the radio). Result: `mgmt 2→101-142`, `data 0→9-14`, **CYD `PRB:0→~14`**. Client capture now
  works end-to-end (verified: probe-request clients flow to the CYD band count).
- **Fix 2: capture clients BOTH directions** in `handleData` — uplink ToDS `addr2` *and* downlink
  FromDS `addr1` (was uplink-only). A camera is seen whether transmitting or being addressed. (Caveat:
  a downlink frame's RSSI is the AP's, so a downlink-only client carries an approximate RSSI.)
- **Fix 3: sweep all 2.4 GHz channels 1-11** (was 1/6/11), `HOP_DWELL_MS` 200→160, `PROMISC_MS`
  3000→4500 — catches cameras on auto-selected non-1/6/11 channels.
- **Still bench-limited:** `dataClient=0` at the bench only because nothing nearby was actively
  streaming unicast (the 14 captured data frames were multicast/broadcast, correctly rejected). A
  running camera on a walk should now produce dataClient hits + Ring/Nest matches. **Needs a field
  re-test near a known active camera.** Remaining gaps: **5 GHz-associated cameras** (promiscuous is
  2.4-only — a 5 GHz promiscuous sweep is the next lever) and **battery cameras asleep** (RF-silent
  until motion — fundamentally uncatchable while dormant; trigger motion to wake them).
- C5 FLASHED (COM18). CYD unchanged this round. No protocol bump. Kept the `mgmt/data/dataClient`
  per-window serial diagnostic (cheap, useful in the field).

## Done — HOME readout tweaks (2026-10-10, CYD-only, FLASHED)
All in `src/cyd/main.cpp`; CYD rebuilt + flashed (COM14). No C5/protocol change.
- **"Other" tile removed** from the HOME category readout — `drawMenuThreats()` now builds `g_menuCat`
  skipping `CAT_OTHER` (12 tiles / 4 rows instead of 13 / 5). Devices can still resolve to `CAT_OTHER`
  internally; it just isn't tiled or counted on HOME.
- **TRACKER gated to FOLLOWING-only.** `groupCategory()` now returns `CAT_TRACKER` only when the
  device `isFollowing()` (ftier 2 = tracked ≥`FOLLOW_SPAN_M` 400 m across the GPS route); otherwise
  `CAT_NONE`. Fixes the suburban false-positive flood (user saw 8–12 idle AirTags/Tiles). Because
  `groupCategory()` is the single source for the HOME count, the `SCR_MENUCAT` drill-down, AND the
  DETS `cat` field, the phone/web breakdown became consistent for free (no web change). New helper
  `isFollowing(mac)` (fwd-declared before `groupCategory`, defined by the follow helpers). Blurb
  updated. **Caveat:** a non-following tracker still keeps its Find-My Suspect tier floor, so it still
  shows as a yellow suspect row on the live list + Suspect tally — only the category tile is gated.
  (Follow-state is computed *after* scoring, so gating the tier floor too would be a separate change.)
- **Bottom hint now two lines** (the single line overran 240 px). Threat caption/tiles raised
  (`MENU_THREAT_Y` 190→176, `CHIP_Y0` 202→190; new `MENU_HINT_Y`=292); hint reads "Tap an item, or
  use BOOT:" / "tap = next,  hold = select".

## Done — associated-client (data-frame) capture + camera OUIs (2026-10-10, BOTH BOARDS FLASHED)
Root cause of "walked past Ring doorbells, saw nothing": the scanner was deaf to **associated
Wi-Fi clients**. An installed Ring/Nest/Wyze cam is a client joined to a home AP — it never beacons
(so `WiFi.scanNetworks()` misses it) and, once joined, doesn't probe (so the probe-request path
misses it). Its MAC is only in the clear in the `addr2` of its uplink **data frames**, which the C5
promiscuous filter was discarding (`WIFI_PROMIS_FILTER_MASK_MGMT` only).
- **C5 (`src/c5/promisc.*`):** broadened the filter to `MGMT | DATA`; `rxCb` now dispatches
  `WIFI_PKT_DATA` to a new `handleData()` that takes the `addr2` source MAC of **uplink (ToDS=1,
  FromDS=0)** data frames and emits it as `Source::WifiProbe` (a Wi-Fi client). A 32-slot recent-MAC
  ring (`seenRecently`, 1 s suppress) keeps a chatty station from spinning the table mutex every
  frame. **Still 100% receive-only** (reads an address already on the air). **Wire-compatible:
  reuses `WifiProbe`, so NO protocol bump (still v7), NO struct change** — an unchanged CYD already
  scores `WifiProbe` against `W`/`A` OUI rules, so Ring etc. fire. Caveat: these clients land in the
  same 96-entry table (oldest-evict, 30 s TTL); a dense area could pressure it (tuning follow-up:
  per-source sub-caps). Only 2.4 GHz 1/6/11 during the promiscuous window (Ring is 2.4 GHz).
- **CYD DB (`src/cyd/sigdb.cpp`):** added camera OUIs from the public IEEE MA-L registry —
  **Nest** `64:16:66`/`18:B4:30`, **Reolink** `EC:71:DB`, **SimpliSafe** `F8:51:28` (all `70,W`);
  **re-weighted Arlo `BC:DD:C2` + Blink `4C:69:05` 15→40** so they stand alone as suspect. New labels
  map to `CAT_CAM` (`categoryOf()` now matches Nest/Reolink/SimpliSafe). **Bumped `DB_GEN` 2→3** so
  the device deletes + re-seeds `/signatures.csv` on next boot. (Eufy/Anker researched but omitted —
  no clean verified brand OUI.) Attribution in `docs/references.md`.
- **Builds:** both PASS — C5 clean; CYD RAM 33.0% / Flash 44.7% (unchanged). **BOTH FLASHED
  2026-10-10** (C5 native COM18 `303A:1001`; CYD COM14 `1A86:7523`; boards were off-battery +
  unlinked during flash). No protocol bump, so they interoperate even mid-upgrade. **On-device
  verification still pending** — needs the UART link + power reconnected; the CYD reseeds
  `/signatures.csv` on this boot (`DB_GEN` 3).
- **Separately diagnosed (not a code bug): "boot shows BLE only, no APs."** Code review confirms the
  C5 does scan dual-band Wi-Fi at boot and the CYD bg scan (default ON) requests it. Likely causes:
  (1) first dual-band scan takes ~10 s so APs appear late; (2) a persisted NVS `srcmask` with Wi-Fi
  off — check web STATUS `src=` (all-on default = **27**); (3) C5 Wi-Fi/BLE coexistence scan failure
  — check CYD serial `table=(2.4:.. 5G:.. BLE:..)`. User to confirm on-device.

## Done — webapp JS extracted into ES modules (2026-10-10, no firmware change)
- `17c8325` **webapp: inline `<script>` → native ES modules.** The ~940-line inline script in
  `webapp/index.html` (1174→236 lines) is split into 19 modules under `webapp/js/`
  (`config.js`, `state.js`, `util/{dom,format}.js`, `ble/{connection,status,commands,download}.js`,
  `gps.js`, `data/parse.js`, `features/{detections,categories,followers,explore,sessions,whitelist,
  map,detail}.js`, `main.js`). **Extraction, not a rewrite** — the tuned logic (DETS partial-frame
  flush, LOGDATA download reassembly, GPS throttling) moved unchanged. **No build step** (ES modules
  load natively; Chrome-Android-only = full ESM support). Cross-module mutable state routed through
  one `state` object; imports form a DAG. Statically verified (all parse as ESM; every import
  resolves to a real export; no undefined calls / missing constants / orphaned inline handlers).
  CLAUDE.md + README + this file updated. **Not yet smoke-tested in-browser** (needs Android Chrome
  / Web Bluetooth) — if something breaks, the likely cause is a function placed in the wrong module
  boundary. **No C++/firmware change; both boards still on protocol v7.**

## Done — active-BLE-scan multi-UUID + category readout + HOME redesign (both boards flashed v7)
Newest first:
- `1cf88f4` **cyd+web: nameless devices show their MAC** (instead of a single generic `<hidden>`).
  Reworked `groupName()` (`src/cyd/main.cpp`) into one early helper: advertised name, or the MAC as
  text when there is none — so nameless devices stay individually identifiable. Feeds the live list,
  detail screens, Explore viewer, the DETS phone stream, and the web app (live/category/follower
  lists, map popups, detail modals). **CYD-side display fallback only** — the C5 sends empty names,
  so **no C5 reflash, no protocol bump**. NDJSON log unchanged (still omits blank `name`, carries
  `mac`; readers fall back at display time). CYD rebuilt (RAM 33.0% / Flash 44.7%) + **flashed**
  (COM17 this session); link verified up (`table=21 … BLE:21 db=1`). Drone-decode work explicitly
  left untouched per user.
- `f44df6a` **docs: on-board battery power subsystem.** Rig is now portable — 5000 mAh 3.7 V LiPo →
  TP4056 Type-C charger → master switch → 1.5 A boost (3.7→5 V), 5 V out feeding **both boards in
  parallel** (CYD VIN + C5 5V, common ground = the UART-link ground). PINOUT.md §4 + README Power
  note, from `hardware/diagrams/cyd-scanner_wiring_diagram_labeled.png`. Data link (22/27 ⇄ C5 4/5)
  unchanged. Safety: feed 5 V only, never bridge the 3.3 V rails, **switch battery OFF before USB
  flashing** (back-feed risk). See [[battery-power-subsystem]].
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

Device state: **both boards on protocol v7** (no bump since; all 2026-10-10 work is wire-compatible).
**Latest flashes (2026-10-10 this session):** CYD = camera OUIs + HOME readout tweaks (Nest/Reolink/
SimpliSafe, Tracker follow-gate, no "Other" tile, 2-line hint, `DB_GEN` 3); C5 = associated-client
data-frame capture + BLE-pause starvation fix + 2.4 ch 1–11 + **5 GHz promiscuous sweep** + per-window
capture diagnostic. Ports this session: CYD = CH340 `1A86:7523` = COM14; C5 native = `303A:1001` =
COM18 (flash), C5 UART bridge `2E3C:5740` = COM8 (does NOT flash). Bench-verified both promiscuous
bands capture (`promisc 2.4 end mgmt=137 data=24 dataClient=2`, `promisc 5G end mgmt=10`). Ports
drift — identify by VID:PID. **A field re-test near a known active camera is the pending validation
(see outstanding #0).**

## PICK UP HERE — outstanding

0. **FIELD RE-TEST of Wi-Fi client / camera capture (TOP, in progress).** The promiscuous
   starvation fix + both-direction data-frame capture + 2.4 ch 1–11 + 5 GHz sweep are all flashed and
   bench-verified, but the real validation is a walk near a KNOWN active camera. Watch the C5 console
   `promisc 2.4 end` / `promisc 5G end` lines for `data`/`dataClient` climbing, and the CYD for the
   camera's brand label. **Pause ~20–30 s near a target** — the cycle is ~21 s so each band sweeps only
   ~once per cycle; a quick walk-by can fall between windows. Known residual gaps to keep in mind when
   judging results: **DFS 5 GHz channels (52–144) are not swept**, and **dormant battery cameras are
   RF-silent** until motion (walk in front to wake them). If 5 GHz `dataClient` stays 0 near a known
   5 GHz camera, next lever is adding DFS channels and/or lengthening the 5 GHz window.

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
6. **Phase-5 follow-up:** the rotating-RPA phone self-filter guard (a rotating-random-address phone,
   now shown by MAC rather than `<hidden>`, still gets flagged as a follower).
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

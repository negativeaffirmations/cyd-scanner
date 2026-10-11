# Handoff — 2026-10-11 (active branch: `remoteid-decode`; `main` is the trunk)

## Branches / deploy
- **2026-10-11: active feature branch `remoteid-decode`** (forked from `main` after `f88ed01`). Holds
  the in-progress Remote-ID ASTM F3411 decode (increment 1 — see outstanding #5 + Done below). Pushed
  to origin. Merge back to `main` when the feature lands; `main` is otherwise the trunk and everything
  else below was committed there.
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

## Done — device-detail sighting-trail map (2026-10-11, webapp-only, pushed + Pages-deployed)
The Scanner-page device-detail modal (`openLiveDetail`) now shows a **map at the bottom** of where a
device was seen. The live DETS stream carries **no per-detection GPS**, so this is a webapp-side
**sighting trail**: new `webapp/js/features/sightings.js` logs the phone's **own** current GPS fix
(`state.lastGps`) for every seen MAC on each snapshot (~8 m move threshold so a stationary phone
refreshes one point instead of piling duplicates; capped 240 pts/MAC, 500 MACs; in-memory, cleared on
disconnect). Same wardriving model as the Map page. Refactored the modal map into a shared
`renderDetailMap(points,msg)` in `features/detail.js` that owns the single Leaflet instance on
`#detMap` (two maps on one div would throw) — Explore detail now passes its one logged fix through it;
live detail passes the trail (dashed connector, newest marker highlighted, popup = RSSI + time).
Wired: `detections.js renderDets → recordSightings`, `connection.js onDisconnect → clearSightings`.
**Only fills while GPS is active + the user moves**; otherwise a context hint. No firmware/protocol
change, no CSS change (so no `style.css` recompile / `?v=` bump). ES-module-syntax verified + no
import cycle (categories.js → detail.js is one-way). **Static-verified only — needs an Android-Chrome
GPS-walk smoke test** (rolls into the redesign's one open test item).

## Done — P3 log hardening (2026-10-11, CYD FLASHED COM14)
Three small log follow-ups (`src/cyd/main.cpp`): (a) **dense-area thrash guard** — a full per-session
dedup set (`MAX_SEEN` 400) no longer re-logs unknown MACs every cycle; `logNewDetections()` skips them
and auto-rotates to a fresh session (resumes capture) at most once per `SESS_ROTATE_MIN_MS` (5 min); new
`LogStream.lastRotateMs`. (b) pre-time-sync `enforceLogCap` eviction-order comment (FAT-epoch timestamps
are near-equal before clock sync). (c) `deleteSession` also refuses the currently-streaming file
(`g_streamingPath`). CYD built + flashed COM14; no protocol bump. Resolves outstanding #7.

## Done — follower RPA false-positive guard (2026-10-11, CYD FLASHED COM14)
`computeFollowScores()` (`src/cyd/main.cpp`) no longer escalates a **benign BLE Resolvable Private
Address** to FOLLOWING. New `macIsBleRpa()` = MAC MSB top two bits `0b01` (the privacy-rotating address
phones/watches advertise; the own-phone `peerMac` self-filter misses it because a phone's GATT address
differs from its advertised RPA). An RPA group with **no sigdb tier and no notable flag** caps at
PERSISTENT — out of the follow alert, magenta LED, `CAT_TRACKER`, and STATUS `fol`. A planted
AirTag/tracker is **exempt** (it carries a Find-My/tracker flag or tier). Build SUCCESS, flashed COM14.
No protocol bump (C5 untouched). Resolves outstanding #6; see it for the trade-off + Wi-Fi-random
follow-up. [[phase5-followme-direction]]

## Done — web lock/unlock CYD display + Settings regroup (2026-10-11, CYD FLASHED COM14)
Settings page split into two cards — **CYD device** (brightness + a new Lock/Unlock-display button) and
**Data & sync** (Sync time & GPS, Reload DB, Whitelist). The Lock button drives the **bag-safe touch
lock** (`g_touchLocked`) over BLE:
- **Firmware (CYD only):** new CMD `K1`/`K0` (`phone.cpp` parse + `lockRequested()` getter, `phone.h`);
  `main.cpp` polls it next to brightness → sets `g_touchLocked`, repaints the padlock, pushes status.
  STATUS gained a **`lock=0/1`** key (`g_touchLocked` moved above `pushStatus`; `s[320]`→`s[360]`).
  **No `Detection`/`Status` struct change → no PROTOCOL_VERSION bump; C5 untouched, interop preserved.**
  Build SUCCESS (RAM 33.1% / Flash 44.7%), **flashed COM14** (`1A86:7523`).
- **Webapp:** `setLock()` (`commands.js`, writes `K1`/`K0`); `status.js` reflects `lock=` onto the button
  (label flips Lock↔Unlock, `.danger` highlight while locked, `data-locked` drives the toggle);
  `main.js` wires `#lockBtn` + adds it to the connect/disconnect enable-lists. `.cardhdr` SCSS for the
  section headings. Recompiled; `?v=20261011f`, uiver → `cyd-lock`.

## Done — Map page fits without scroll + Leaflet-over-navbar fix (2026-10-11, webapp-only)
Two Map-page bugs the user hit:
- **Leaflet painted over the fixed bottom navbar.** Root cause: `#map` had no stacking context, so
  Leaflet's internal panes/controls (z-index up to ~800) were compared against the ROOT context where
  the navbar sits at z-index 20 → map on top. Fix: `#map { position:relative; z-index:0 }` (`_map.scss`)
  confines them to the map box.
- **Page needed scrolling to reach the navbar.** Consolidated the 7 inline source/threats/events filter
  checkboxes into a **Filters modal** (`#mapFiltDlg`) opened by a new **Filters** button in the toolbar
  row (beside Current-session / Load), so the toolbar is one line; and changed `#map` height from
  `68vh` to `calc(100dvh − 250px − safe-area)` (min 220px) so the page fits in one viewport. The
  checkboxes kept their ids (`f24`/`f5`/`fble`/`fprb`/`f154`/`fThreats`/`fEvents`), so `map.js
  renderMap()` and the `main.js` change-wiring work unchanged; added open/close wiring for `#mapFiltDlg`
  (mirrors the Scanner `#filtersDlg`). Recompiled `style.css`, bumped `?v=20261011b` + `#uiver` →
  `map-fit`. No firmware change.

## Webapp cache / deploy-propagation (updated 2026-10-11)
**GitHub Pages serves EVERY asset — `index.html` included — with `cache-control: max-age=600` (verified
via `curl -D-`), and that real HTTP header overrides the `<meta http-equiv="Cache-Control" no-cache>`
in the HTML (browsers largely ignore the meta for caching).** So after a push, an already-loaded /
recently-loaded phone keeps serving the **cached old HTML + old JS for up to ~10 min**; Pages is not
configurable to change this and there's no no-build way around it. This is why a fresh deploy can look
unchanged on the phone for several minutes. Practical signals/levers:
- **`#uiver` footer** (bottom of `index.html`, e.g. `ui 2026-10-11 · det-map`) — **bump it on EVERY
  webapp deploy** (it's the human "is the new build live?" readout). Once the footer flips to the new
  value the HTML has refreshed, and since all assets share the same deploy time + max-age the matching
  JS is live too. It had been stale at `2026-10-10` through the whole redesign — keep it current.
- **`style.css?v=<token>`** — **bump the token whenever `style.css` changes** so the (eventually
  refreshed) HTML points at a new CSS URL. (JS modules can't be token-versioned without a build step —
  static-import URLs are fixed — so they ride the 10-min max-age revalidation.)
- To force-fresh on the phone now: wait ~10 min after the Pages build, then fully reload (reopen the
  tab / clear site data). A soft refresh within the window still serves cache.

## Done — webapp redesign (Material bottom-nav), ALL PHASES COMPLETE (2026-10-11, webapp-only, pushed + Pages-deployed)
Full plan in memory [[webapp-redesign-plan]]. Phased; reuse components per [[ui-component-reuse]].
**Current state:** the full redesign (phases 1 → final polish, commits `5020070`..`bdd38d9`) is committed
on `main`, **pushed, and live on GitHub Pages** (build `built` @ 2026-10-11T04:08Z). The navbar is now
**5 pages** — Home / Scanner / Map / **Data** / Settings (`nav.js` `PAGES`). CLAUDE.md's "Phone link +
web app" section describes the finished 5-page UI. **The whole redesign is still STATICALLY-verified only
— NOT in-browser tested** (needs Android Chrome + Web Bluetooth); that smoke test is the one open item for
the webapp (see outstanding). The phase-by-phase log below is kept as history; the final-polish entry
records what landed after Phase 4.
- **Phase 1 (DONE): sticky bottom navbar + page scaffolding.** `index.html` restructured into a
  persistent top app bar (title + `#conn`) + a `<main id="pages">` with **Home** and **Scanner**
  `<section class="page">` views, plus a fixed **Material** bottom navbar (Home/Scanner/Map/Settings,
  Material Symbols icons via Google Fonts — no bundler/framework, honoring the no-build rule). New
  `js/nav.js` `showPage()` toggles a `.visible` class (class-based, not `[hidden]`, so CSS can't
  override); wired in `main.js`. **Home** = connect + status card grids + "Threats on device"
  (`#catChips`) + Activity log. **Scanner** = session + scan controls + follower/alert banners + live
  `#dets` + the Explore/Scan-Viewer + the data/transfer controls + whitelist. **Map & Settings nav
  items open their existing dialogs for now** (become full inline pages in later phases — Map needs
  Leaflet `invalidateSize`-on-show, deferred). Risk kept low: **every element id preserved** (verified
  all 124 JS-referenced ids still present), the 3 retired nav buttons (`#scanNavBtn/#mapBtn/
  #settingsBtn`) are just `hidden` so their handlers + connect/disconnect enable-lists keep working
  untouched. New SCSS `_nav.scss` (@use'd in `main.scss`); **recompiled `style.css` via
  `npx sass@1.105.1`** (node_modules isn't installed here; normal flow is `npm run sass:build`).
  Statically verified only — **NOT in-browser tested** (needs Android Chrome); Pages redeploys on push.
- **Phase 1.5 (DONE): Home cosmetic pass + per-category color system.** `index.html` Home cards
  regrouped: the 4 scan types in one `.scantypes` card (each tile grays via `.off` when its source is
  off in Scan Settings — driven by STATUS `src`, wired in `status.js`), then rows Probe|Unique,
  Suspect|Likely|Confirmed, Time|GPS|DB (new `.grid.grid3`). **New `CAT_COL` palette** (`js/config.js`,
  1..12 = Flock..Skim) — the Home threat section now renders the FULL 12-category taxonomy as cards
  (`.tgrid`/`.tcard`, `features/categories.js`), 3/row like the CYD: outline always the category hue
  (even at 0), grey fill at 0, tinted when >0, tap→category modal. **Reuse `CAT_COL` everywhere
  categories appear** (consistency directive). Recompiled `style.css`; all 128 JS ids verified.
- **Phase 2 (DONE): Scanner page — tall tappable rows + device modal + threat strip.** `renderDetList`
  (`features/detections.js`) rewritten to **tall 2-line rows** (`.detrow`, min-height 74px ≈ 3×; line1
  name+RSSI, line2 source pills+MAC[+ch/time]), **whole row tappable → modal**, used by BOTH the live
  list and the Explore viewer (unified; dropped the old multi-source caret-expand). Live tap →
  `openLiveDetail` (renamed from `openCatDevice`, now exported); Explore tap → `openDetail` (has the
  GPS mini-map). Both modals carry a **whitelist action** via new `fillDetailWhitelist(mac,muted)` in
  `whitelist.js` (slot `#detActions` in `detDlg`): **Add** = one tap `A:mac,<MAC>,A,`; a whitelisted
  device links to the Whitelist manager (removal is index/rule-based — true remove-by-MAC would need a
  small firmware CMD, noted as optional). Shown only while connected. New **`#scanCounts`** strip atop
  the Scanner = compact CAT_COL-colored chips for in-range threat categories (`renderScanCounts`).
  `#dets`/`#scanViewer` got `.dets.tall` (max-height 72vh). SCSS `_detections.scss` + recompiled css;
  JS parses, ids resolve (only the dynamically-created `#detWlBtn` isn't static — expected). **NOT
  in-browser tested.**
- **Phase 2.5 (DONE, except 1 pending icon): Scanner controls → Material icon row + threat modal.**
  `#scanPanel` is now a 5-button `.iconrow` of Material Symbols icon buttons: Start/Stop
  (`fiber_manual_record`↔`stop`, via `updateScanBtn`), **Filters** (`filter_alt`, NEW), New session
  (`note_add`), Explore (`find_in_page`), Scan settings (`settings`). **Filters** opens `#filtersDlg`
  (source checkboxes + threats-only) that filters the LIVE list via `applyLiveFilter`/`liveFilter`
  (counts/strip still reflect all). `#scanCounts` is now a `.threatstrip` of small CAT_COL pills for
  ALL 12 categories (count when >0, dim at 0); the whole row taps → `#threatsDlg` breakdown (vertical
  list, active-first, tap a row → its device modal via `openCatModal`). Removed the "Live detections"
  instructions; title is now **"Scan" + `#scanModeIcon`** that `setScanMode(live)` toggles (live on
  `renderDets`, loaded on `applyExplore`). **PENDING USER CHOICE:** the live/loaded title icons — currently
  PLACEHOLDERS `sensors` (live) / `folder_open` (loaded); asked the user to pick. Webapp-only; recompiled;
  JS parses, ids resolve (only dynamic `#detWlBtn`). NOT in-browser tested.
- **Phase 4 (DONE): Map + Settings are real nav pages.** `#mapDlg`/`#settingsDlg` dialogs removed; their
  content moved into `<section class="page" id="page-map">` / `id="page-settings">`. `nav.js` `PAGES` =
  home/scanner/map/settings. `openMap()` now `showPage('map')` + `invalidateSize`+`renderMap` on an
  80 ms timeout (fixes Leaflet-in-hidden-container sizing); `navMap`→`openMap`,
  `navSettings`→`showPage('settings')`. Settings page = brightness + the Whitelist button (moved off
  Scanner). Removed the now-dead `settingsClose`/`mapClose` handlers + the disconnect `settingsDlg.close()`
  and `sessions.js` `mapDlg.open` check (both would have thrown). Retired hidden buttons stay for the
  connect/disconnect enable-lists. No SCSS change (`#map` is fixed 68vh). JS parses, no dead refs/dup
  ids. **NOT in-browser tested.**
- **Final polish (DONE 2026-10-11, commits `458e742`..`bdd38d9`): Data page + app-bar + list-flow.**
  - `458e742` **Home compaction** — regrouped the Home cards so the 12-category Threats readout fits
    without scrolling.
  - `337fbcc` **status icons → the app bar** — the small **time / GPS / DB** status icons moved into the
    persistent top app bar (white when active, gray when not); the old connection "pill" became a
    **Bluetooth icon**.
  - `fca96a6` **connection icon = plain Bluetooth** — gray `bluetooth_disabled` / yellow searching /
    white connected, no background chip.
  - `9a8ef3c` **new Data nav page (5th page)** — the download / Wi-Fi-mode / time-range-export / session-
    logs controls moved off the Scanner page into their own **Data** page; **Sync time & GPS + Reload DB
    moved to Settings**. `nav.js` `PAGES` is now `home/scanner/map/data/settings` (navbar is 5 items).
  - `3ba4ee1` **Scanner list flows with the page** — the live detection list is no longer an inner
    scroll pane; its rows flow with the page scroll, and a **jump-to-top** button appears bottom-right
    once scrolled.
  - `bdd38d9` **cache-bust `style.css`** — `index.html` now links `style.css?v=<token>`; bump the token
    on every CSS change (see the "Webapp cache-busting" section above). Fixed phones serving a stale CSS
    that made the list a separate-scroll pane.
  All webapp-only, `style.css` recompiled, pushed, Pages-deployed. **Static verification only — the
  in-browser smoke test (Android Chrome) is still pending for the whole redesign.**
- **Optional polish left:** further group the Scanner controls/filters; a one-tap un-whitelist firmware
  CMD (`E:mac,<MAC>`) for true remove-from-modal; tidy the now-dead `#mapDlg` CSS in `_map.scss`.

## Done — brightness slider fix (2026-10-10, CYD-only, FLASHED)
The brightness setting did nothing: `initCommon()` called `ledcAttach(TFT_BL_PIN,…)` BEFORE
`tft.init()`, but TFT_eSPI is built with `TFT_BL=21`/`TFT_BACKLIGHT_ON=HIGH`, so `tft.init()` then
drove GPIO21 as a plain HIGH output — detaching the LEDC PWM and leaving the backlight stuck full on
(so `applyBrightness()`'s `ledcWrite` hit a non-PWM pin). Fix: move the `ledcAttach`+`ledcWrite` to
AFTER `tft.init()`/`setRotation()` so the PWM claim wins. Brightness now dims (slider + web `B:` both
map 0–100 → duty 20–255, floored so never fully dark). CYD-only, no protocol bump, flashed COM14.

## Done — screen wake lock (webapp) + bag-safe touch lock (CYD) (2026-10-10)
Addressing the screen-off-kills-GPS finding + a request for a bag lock.
- **Webapp Screen Wake Lock** (`webapp/js/wakelock.js`, new): `acquireWake()` on BLE connect,
  `releaseWake()` on disconnect, `reacquireWake()` on `visibilitychange` (the OS drops the lock when
  the page hides). Keeps the phone page foreground so `watchPosition` keeps pushing GPS — the root
  cause of the screen-off walk logging only one fix. Best-effort/feature-detected (battery saver can
  refuse; non-fatal). Wired in `ble/connection.js` + `main.js`. **Webapp-only; Pages redeploys on
  push; not in-browser tested** (needs Android Chrome). NOTE: the screen stays ON (dimmable) — the
  only web way to keep GPS alive; true screen-off GPS needs onboard GPS (outstanding) or a native app.
- **CYD bag-safe touch lock** (`src/cyd/main.cpp`, FLASHED COM14): a status-bar **padlock glyph**
  (yellow=locked / dim grey=unlocked) at `lockCx()` = W−52 (clear of the GPS text/phone icon/link dot,
  and above the y24 "< BACK" bar — no overlap on any screen). **2 taps in the padlock region within
  1.5 s toggle** it (`serviceTouchLock()`, `LOCK_TAPS`/`LOCK_TAP_WINDOW_MS`; gated right after
  `buttonEvent()` in `loop()`); while locked, ALL touch + BOOT is swallowed except that 2-tap gesture.
  (Was 3 taps/1.2 s — lowered 2026-10-10 because resistive touch merges fast taps, making 3 hard to
  register.) Discrete rising-edge taps (not a
  hold/swipe) so constant bag pressure = 1 edge, can't rack up a toggle. **Scanning keeps running
  while locked**: the bg-scan block already covers non-SCR_SCAN screens, and the gate re-runs the
  SCR_SCAN cycle itself so a bagged live scan keeps capturing + logging. No physical button needed.
  Research found no canonical CYD screen-lock (we're TFT_eSPI + XPT2046, not LVGL); discrete-tap was
  confirmed the bag-robust choice. CYD-only, no protocol bump.

## Done — webapp BLE download naming fixes (2026-10-10, webapp-only, NOT yet Pages-deployed)
Two bugs in the BLE "download current session" (`downloadLog`, `webapp/js/ble/download.js`), both
**client-side only** — the device was fine:
- **`.jsonl` download got `.txt` appended** (→ `scanlog.jsonl.txt`): the reassembled `Blob` had no MIME
  type, so Android Chrome sniffed it as text and appended `.txt` (`.jsonl` isn't a known text ext).
  Fixed: `new Blob(chunks, {type:"application/octet-stream"})` → saved as named.
- **Download name wasn't the timestamped session name**: `downloadLog()` hardcoded `name:"scanlog.jsonl"`.
  The DEVICE-side rename-on-time-sync (`renameStreamOnSync`, main.cpp — `sess-NNNNN.jsonl` →
  `YYYYMMDD-HHMMSS.jsonl`) is intact and WORKS in the jsonl era (verified: the downloaded rows carried
  real epochs, so time synced and the on-SD file was renamed). The BLE download just ignored it. Fixed:
  name the saved file after STATUS `sess=` (the `Session:` label), e.g. `20261010-HHMMSS.jsonl`, with a
  `scanlog.jsonl` fallback before time sync. (`downloadFiltered` already used the session name; the
  Wi-Fi `/dl`,`/dlf`,`/scanlog.jsonl` paths already set Content-Disposition filename + `application/
  x-ndjson`, so they were never affected.)
- The user's existing `scanlogs/scanlog.jsonl.txt` is valid NDJSON with a wrong extension — just rename
  to `.jsonl`. **Not in-browser tested** (needs Android Chrome + Web Bluetooth). Commit the JS + push →
  Pages redeploys.

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

Device state: **both boards on protocol v7** (no bump yet; everything flashed 2026-10-11 is
wire-compatible — the Remote-ID frame that bumps to v8 is increment 2, not yet landed, so the boards
still interoperate even across the branch work).
**Latest flashes (2026-10-11 this session):** CYD (COM14) was reflashed several times on `main` —
web lock/unlock (`K1`/`K0` + STATUS `lock=`), the follower RPA false-positive guard, and the P3 log
hardening (dense-area rotate guard + two defensive tweaks). C5 (COM18) was flashed on branch
`remoteid-decode` with **Remote-ID ASTM F3411 decode increment 1** (serial `[RID] …` readout; no
protocol change). Earlier 2026-10-10 flashes: CYD camera OUIs + HOME readout tweaks (`DB_GEN` 3);
C5 associated-client data-frame capture + BLE-pause starvation fix + 2.4 ch 1–11 + 5 GHz promiscuous
sweep. Ports: CYD = CH340 `1A86:7523` = COM14; C5 native = `303A:1001` = COM18 (flash); C5 UART bridge
`2E3C:5740` = COM8 (does NOT flash) — ports drift, identify by VID:PID.
**Pending field validation:** (a) Wi-Fi client/camera capture near a known active camera (outstanding
#0); (b) the follower RPA guard — confirm your own phone no longer trips the follower banner (#6);
(c) Remote-ID decode — point an ODID transmitter/sim at the C5 and watch the `[RID]` serial lines (#5).

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
4. **~~Pages deploy pending a GitHub incident~~ — RESOLVED.** The 2026-10-05 **GitHub Actions
   `degraded_performance`** backlog has cleared; the latest build is `built` (2026-10-11T04:08Z, the
   webapp-redesign push) and the live webapp serves current. **Open webapp item instead: an in-browser
   smoke test** of the redesigned 5-page UI on **Android Chrome** (Web Bluetooth + geolocation) — the
   entire redesign was statically verified only. Re-verify a deploy anytime with
   `gh api repos/negativeaffirmations/cyd-scanner/pages/builds/latest --jq .status`.
5. **Remote-ID DECODE (ASTM F3411) — IN PROGRESS on branch `remoteid-decode`.** Design +
   increment plan in [docs/remoteid-decode.md](docs/remoteid-decode.md).
   - **Increment 1 DONE (2026-10-11, C5 FLASHED COM18, no protocol change):** `src/c5/remoteid.*`
     clean-room decodes the OpenDroneID messages (Basic ID / Location / System / Operator ID, incl.
     Message Pack + the BLE `0x0D` legacy framing) from the `0xFFFA` BLE service data already captured
     in the scan callback; accumulates per emitter MAC in a bounded TTL table and **prints decoded
     UAS ID + drone/operator lat-lon + height/speed to the C5 USB console** (`[RID] …`). Link stays
     **v7** (both boards interoperate) — bench-test with an ODID transmitter/sim and watch the serial.
   - **Increment 2 (next):** `Reply::RemoteId` frame + `RemoteId` struct → **PROTOCOL_VERSION 8, both
     boards flash together**; the C5 emits one per emitter in its scan-dump burst; the CYD keys it to
     the Detection by MAC. Then (3) CYD detail UI + NDJSON `rid` log line, (4) web-app Map plotting of
     drone + operator positions. BLE-extended-scan (BT5 packs) coexistence risk stays a later, measured
     change — check against the BLE/15.4 coexistence rule.
6. **~~Phase-5 rotating-RPA follower false-positive~~ — HEURISTIC GUARD DONE (2026-10-11, CYD FLASHED).**
   `computeFollowScores()` now refuses to escalate a **benign BLE Resolvable Private Address** to
   FOLLOWING: `macIsBleRpa()` tests the MAC's top two bits (`0b01` = RPA, the privacy-rotating address
   phones/watches use — the own-phone `peerMac` filter misses it because the GATT address ≠ the
   advertised RPA). An RPA group with **no sigdb tier and no notable flag** caps at PERSISTENT (never
   ftier 2), so it drops out of the follow alert / magenta LED / `CAT_TRACKER` / STATUS `fol`. A planted
   AirTag/tracker is **exempt** — it carries a Find-My/tracker flag or tier, so it still flags.
   Trade-off: a signature-less RPA tracker that genuinely follows won't raise FOLLOWING (acceptable;
   the proper fix is IRK/bonding resolution — still backlogged). CYD-only, no protocol bump; flashed
   COM14. **Field-verify** on a walk that your own phone no longer trips the follower banner. The
   analogous **Wi-Fi randomized-MAC** case (locally-administered probe MACs) is a cheap follow-up if it
   shows up in the field — not done yet. See [[phase5-followme-direction]].
7. **~~P3 log follow-ups~~ — DONE (2026-10-11, CYD FLASHED).** (a) **Dense-area thrash guard:** when a
   stream's dedup set (`MAX_SEEN` 400) fills, `logNewDetections()` now **skips** unknown MACs (was
   re-logging every ~2 s cycle) and **auto-rotates** to a fresh session so capture resumes — capped to
   once per `SESS_ROTATE_MIN_MS` (5 min) so a dense drive can't spew tiny files; `LogStream` gained
   `lastRotateMs`, armed in `beginSessionNamed`. (b) `enforceLogCap` got the pre-time-sync
   eviction-order comment (FAT-epoch timestamps are near-equal pre-sync → arbitrary but benign order).
   (c) `deleteSession` now also refuses `g_streamingPath` (defense-in-depth; was already in
   `logProtected`). CYD-only, no protocol bump.
8. Older backlog: 802.15.4 opt-in polish, phone/desktop SQLite import, CYD Scan Viewer window caching.
   (The "strict BLE passivity" item is **dropped** — the passive-only directive was removed; active
   scanning is sanctioned, and its extra UUIDs are captured + matched as of v7.)
9. **Battery %/charge status-bar indicator (PROPOSED, awaiting user sign-off + a hardware mod).** No
   software-only path — the boost feeds the CYD a constant 5 V, so the cell voltage must be tapped.
   Plan: a **100 kΩ/100 kΩ divider** from the switched battery rail (TP4056 OUT+/boost input) → **GPIO35**
   (P3/IO1, ADC1_CH7, Wi-Fi-safe); `analogReadMilliVolts(35)`×2 → LiPo voltage→% curve. Optional charge
   icon: TP4056 **CHRG** pad → **GPIO26** (audio header) as digital `INPUT_PULLUP` (LOW=charging).
   **Uses the CYD's last two free GPIOs (35+26).** Firmware = a small `power` module + a status-bar glyph
   (CYD-only, no protocol bump); optional `bat=`/`chg=` STATUS keys for the web app. A MAX17048 I2C
   fuel gauge would be more accurate but its I2C pins (22/27) are the C5 link. User adds the divider at
   the bench, then I implement + add the GPIO35/26 assignment to PINOUT.md. See [[battery-indicator-plan]].

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

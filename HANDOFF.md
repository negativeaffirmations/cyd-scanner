# Handoff — 2026-10-04 (branch: phase5-dev)

## Branches / deploy
- **phase5-dev** = active dev branch (firmware + webapp). Pushed, tree clean.
- **main** = GitHub Pages deploy branch (source: `main` `/`). Gets **webapp-only** deploy
  commits; it lags phase5-dev on firmware. Web app is live at
  https://negativeaffirmations.github.io/cyd-scanner/webapp/
- To deploy the web app: put `webapp/index.html` on `main`, push, Pages rebuilds
  (`git checkout main && git checkout phase5-dev -- webapp/index.html && commit && push`,
  then `git checkout phase5-dev`). Check build: `gh api repos/.../pages/builds/latest --jq .status`.

## Done this session (all committed + pushed)
Commits on phase5-dev (newest first):
- `a07af6d` **C5: retain BLE device name across nameless adverts** — fixes known-named devices
  (e.g. a car stereo) streaming as `<hidden>`. `mergeDetection` kept IE/cid/pan/svc but not
  `name`; a nameless ADV_IND after the named scan-response clobbered it. **C5 flashed.**
- `65a9757` **webapp: phone follow-UI** — the Phase-5 "phone/web follow-UI half". Magenta
  follower banner (STATUS `fol`), per-row follow markers + muted dimming, flagged-device modal,
  follower-detail modal (v3 DETS fields) with a Leaflet track map from a loaded session.
  **Also deployed to main → Pages (`7d9d218`).**
- `cb360e3` **CYD: Phone Link "Connected" + QR back button + follow-me stream** — HOME row reads
  "Connected" while a phone is linked; QR screen got a `< BACK` top bar (+ margin); STATUS adds
  `fol=`; DETS bumped to **v3** (trailing `ftier\tfscore\tmuted`, backward-compatible). **CYD flashed.**
- `c6b9457` **docs: scan logs are NDJSON** — CLAUDE.md / reviewer+security agents / sigdb seed
  comment updated from CSV to `/logs/*.jsonl`. (CLAUDE.md phone-link section further updated
  2026-10-04 for the follow-UI, STATUS/DETS, and the Scan-menu nav — uncommitted if you see it dirty.)

Device state: **CYD and C5 both flashed with the latest.** Follow work did NOT bump the link
protocol (all CYD-computed over BLE strings), so the pair stays in sync.

## PICK UP HERE — outstanding
1. **⚠ Restore follow-detection thresholds** before any real use / release. Still TEST BUILD
   (easy-trigger) in `src/cyd/main.cpp`:
   - L171 `FOLLOW_SPAN_M 50.0f` → **400.0f**
   - L172 `FOLLOW_FIXES 2` → **3**
   - L185 `FOLLOW_SPAN_MIN 40.0f` → **150.0f**
   Then reflash the CYD + commit. (Field-verified: the follower count works — a 2-device drive
   flagged both; the TEST thresholds make it trigger on short loops, handy for testing the UI.)
2. **Follow-detail full parity (optional):** the web follower-detail shows only the DETS stream
   fields (follow tier/score, threat, RSSI, signals, muted). The richer CYD follow-detail fields
   (persistence windows, GPS fixes, max span, sightings, first/last seen, GPS anchor) are not
   streamed — add a small GATT "follow detail" query (CMD + notify) if full parity is wanted.
3. **Rotating-RPA self-filter guard** (your own GPS phone flagged as `<hidden>` follower): chosen
   approach = heuristic guard; NOT built. It's the one follow branch that touches the **C5**
   (needs a `Detection.flags` address-type bit → bump PROTOCOL_VERSION, reflash BOTH boards).
4. Backlog: webapp nav redesign (bottom nav), 802.15.4 opt-in, phone/desktop SQLite import,
   strict BLE passivity review (`setActiveScan(true)` is on), CYD Scan Viewer window caching.

## Flash / tooling notes
- `pio` is not on the bash PATH: use `"$HOME/.platformio/penv/Scripts/pio.exe"`.
- Always `PYTHONIOENCODING=utf-8 pio run -e <env> -t upload` (upload hangs otherwise on Windows).
- **Identify boards by VID:PID, not COM** (numbers drift; `pio device list`):
  CYD = CH340 `1A86:7523` (was COM17); C5 native USB = `303A:1001` (was COM18, **flash this one**);
  C5 UART bridge = `2E3C:5740` (was COM8, does NOT flash). Pass `--upload-port COMnn` when the
  auto-detect grabs the wrong board (it will — it picked the CYD when flashing the C5 this session).
- C5-only changes (like the name fix) don't need a CYD reflash while the link protocol is unchanged.
- C5 5V pin must be unplugged while its USB is connected (two-5V-source risk); user confirmed safe.

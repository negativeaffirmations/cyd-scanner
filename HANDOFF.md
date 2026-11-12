# Handoff — 2026-11-12 (working branch: `hardening`, pushed; trunk `main`)

## RESUME HERE — 2026-11-12: `.cflo` scan archive, IN PROGRESS (spec final, build started)
**Before this:** fw **0.36.0** on all 4 hosts (CYD #1, CYD #2 CFLO-CYD-…, mini #1, mini #2 = leader), commit `51c2143`:
fleet-wide threat readout on clients (`Msg::Threats`, docs/fleet-mode.md §25, blue fleet-client LED, alerts override),
a leader New Session starts a new fleet file (0.35.2, §22.1), fleet client screen layout fixes. Owner confirmed it works
and an upload reached scan-analyzer.
**Spec:** [docs/cflo-format.md](docs/cflo-format.md) **v1 FINAL** (owner-approved 2026-11-12, commit `2bf59e7`), agreed with
scan-analyzer (bridge entries 23:00 … 00:30). One ZIP per scan: `manifest.json` (STORED, first) + `logs/<sid>/<file_id>.jsonl`
byte-exact (DEFLATE). scan-analyzer builds the server side separately (they confirm go with the owner).
**Fallback (agreed, bridge 00:30):** the CURRENT server answers a `.cflo` POST with **415** and stores nothing (both media
types); its limit is 50 MB, checked first -> **413**. So the web app falls back to today's per-file NDJSON bodies on 415 OR
413. Never key on the `detail` text.
**Done, firmware (`9a4b31d`):** fw **0.37.0** in `src/host_common/host_log.cpp`: `makeFileId()` helper; the fleet file's
meta line gets `"pair_file_id"` = the leader's `activeStream()` file_id at fleet-file creation (made early if that file has no
row yet; `ensureLogFile` keeps it). cyd / xiao_s3 / xiao_s3_rgb BUILD OK. **NOT flashed yet** (fleet proto unchanged, so no
lockstep needed).
**Done, web app (committed on `hardening`, NOT deployed; Pages serves `main`):** items 1-5 below. New `js/data/cflo.js`
(writer + reader + `isCflo`; Node round-trip test + Python `zipfile` check passed, tamper detected; test script was synthetic,
in the session scratchpad), new `js/features/cfloupload.js` (`uploadCflo` -> true/false/"auth"/"fallback" on 415|413; uses
`state.remoteApi` set by remote.js), `sessions.js` (`cfloPlan`, `packScan`, `pairOf`/`belongsElsewhere`, `saveCfloSel`,
`uploadItem` -> `uploadPlain` fallback, `state.saveCurrentCflo` / `state.uploadCurrentCflo`), hooks in `ble/download.js`
`downloadLog` + `remote.js` `uploadCurrentSession`, Map Load file reads `.cflo` (main.js). Tokens `?v=20261112a`, uiver
`cflo a`. Browsers without CompressionStream keep the old per-file behaviour.
**Review 1 (REV-001..016) fixed** in the follow-up commit: fw pair taken at rotation (`g_fleetPair`), fleet parts assigned
to exactly one item before packing (`planCflo`), one-file-at-a-time packing, CSV stays plain, one `scanKey` rule, capped
inflate, ZIP64/spanning refusals, busy refusals + ctx taken before `fetchCurrent`, octet-stream save, 64 MB check, run-wide
`noCflo`. Not fixed: REV-013 (`tz_offset_min` = the phone's offset now; noted in code). scan-analyzer validated a synthetic
archive from our writer (bridge 01:50). Server side `c1e4213` built, NOT deployed (needs owner push + restart).
**Next:** focused re-review of the rework; flash 0.37.0 on the 4 hosts; owner test (solo + leader download, open the `.cflo` on
the Map, upload against the OLD server -> fallback works); merge to `main` to deploy; scan-analyzer ships its side, then
re-test the archive upload; CLAUDE.md notes (webapp Data page + fleet meta `pair_file_id`).
**Plan (as written before the build):**
1. NEW `webapp/js/data/cflo.js` (stale-module rule: new exports in new files): ZIP writer (CRC-32, `CompressionStream
   ("deflate-raw")`, PKZIP 2.0, no ZIP64, STORED manifest first) + `crypto.subtle` sha256; reader (`DecompressionStream`)
   enforcing the §1 refusals; `buildCflo(...)` -> Blob, `readCflo(buf)` -> [{entry, text}].
2. `features/sessions.js`: Download = one `.cflo` per scan (own parts + its fleet file(s); pair by `pair_file_id` from the
   fleet file's line-1 meta, else today's overlap rule); Upload = POST the archive (`application/vnd.cointelflo+zip`, Bearer,
   `X-Client-Tz`), fall back to `uploadSessionText` per file on 415/413. Range (`T:`) exports stay plain `.jsonl` (they are
   rewritten, not byte-exact).
3. "Download / Upload current session" (`ble/download.js` `downloadLog`, `features/remote.js` `uploadCurrentSession`):
   one `.cflo` incl. the leader's fleet file (replaces `state.saveCurrentFleet` / `uploadCurrentFleet` second files).
4. Map "Load file" + Explore accept `.cflo` (read entries, `joinLogParts`).
5. Deploy: bump `?v=` on style.css + main.js (+ connection.js if touched) + `#uiver`; CLAUDE.md webapp + SD-log notes.
6. Build `pio run -e cyd -e xiao_s3 -e xiao_s3_rgb`, flash all 4 hosts (fleet proto unchanged, so no lockstep needed), test
   a solo + a fleet download/upload; tester + reviewer passes.


## RESUME HERE — 2026-11-07 (latest, late night): CYD Wi-Fi download fixed (fw 0.35.0 on the CYD only)
**Finding:** the CYD's Wi-Fi download crashed on start (owner test): with the phone link up it has ~60 KB free, largest block
49 KB, and the SoftAP needs ~51 KB (abort in operator new). Probably broken since mid-Oct: its saved PSK was never found.
**Fix (owner chose "restart into it"):** CYD download boot, see CLAUDE.md "CYD download boot". Review: 2 P1 (STATUS `dl=0`
while announcing so the web app dropped the creds; BOOT-hold exit could land in the ROM downloader) + P2/P3 all fixed in
`a28d98e`; web flow passes a Node test with a dropped link. **Owner tested 3 runs OK** (incl. a 2.5 MB file; Done-button exit;
AP up with 70 KB free / 63 KB block; PSK found every time). **STOP bar, BOOT hold and the 10 min idle exit all tested OK** (idle: entered 23:38 via console `download`, restarted `(idle)` at ~23:48 into normal mode).
**Also:** PSK bug fixed for all hosts (`getString` counts the NUL); console `mem`. Web app `cyd-wifi-boot b` live.
**Web fix (`cyd-wifi-boot c`):** the Data button stayed "Wi-Fi download is on" after the CYD came back (the guide's check stopped
when the dialog closed); it now keeps checking until the scanner reconnects without `apboot`. Node test reproduces the old bug.
**State:** minis on **0.35.0**, CYD on **0.35.1** (adds console `download`: CYD restarts into the Wi-Fi download, refused in
fleet mode); CYD back as fleet client (rejoin took ~4 min), fleet whole.
**`bonds clear` + re-pair PASSED (2026-11-08, mini #1, owner's phone):** console clear -> stored 0 / records 0, owner forgot it
in Android, reconnect: authenticated -> paired (enc=1 key=16 bonded=1) -> re-proved over the encrypted link; stored 1 / records 1,
own key ok. Pairing came only after the HMAC, as 0.33.0 intends.
**Bench tests left (need something not on the bench):** NimBLE host-reset recovery (can't be forced), fleet frame replay
(needs a transmitter board).

## RESUME HERE — 2026-11-07 (night): fw 0.34.0 on all hosts, shared BLE keys, 15 min address rotation
**State.** All 3 hosts fw **0.34.0**, fleet proto **v5** (all three on the merged build `d3fa085`, C5s unchanged). Fleet whole, CYD back as client (owner's request), roles wifi24 / wifi5 / ble.
**What (owner's decision on SEC-002):** advert address changes every 15 min, never under a connection (re-armed in onConnect and every 60 s while connected). Each unit shares its IRK (MStat.irk, leader Msg::Irks, NVS fltirk); the self filters resolve private addresses against them and the unit's own (`phone::ownIrk`, self-checked; `bonds` prints `own key ok`). BLE address column no longer written to NVS. docs/fleet-mode.md §24, docs/ble-auth.md.
**Tested:** 2-min bench rotation on mini #1; 18 min fleet run: mini #1's change matched 14 rows by key at the leader, mini #2's own change filtered with its own key; leader reboot matched 4 rows by key before members rejoined. CYD own key ok on the original ESP32. CYD cost +136 B RAM, ~1.6 KB flash. Reviewer: no P1; REV-001/003/004/005/008 fixed, docs (REV-002) done.
**Owner decisions:** SEC-003 left as is (assume nobody paired with 0.32.0); SEC-004 not a flaw (download AP must be findable). CYD phone test on 0.33.0 passed.
**How to check:** `fleet` prints `N BLE keys (M rows by key)`; only units scanning BLE (the leader in a 3-unit split) count rows by key. `bonds` prints `own key ok`.
**Known side effect:** two units NOT in one fleet log each other as a new BLE device every 15 min (was once per boot). Told scan-analyzer.

## RESUME HERE — 2026-11-07 (evening): fw 0.33.0 on all hosts, generic advert name + anonymous AUTH read
**State.** All 3 hosts fw **0.33.0** (`868ad33`, merged as `9b8a0d5`); C5s unchanged. Web app `20261107b` / footer `anon-auth a` live (the first Pages run stuck queued; a fresh `POST pages/builds` deployed). Fleet whole: mini #2 leader, CYD slot 1, mini #1 slot 2.
**What changed (owner's decision via scan-analyzer):** every unit advertises `CFLO`; AUTH read v2 carries `n` + key tag `kt`, never sid/name/kg (also reset on connect/disconnect, since a Read Blob bypasses onRead); pairing distributes IRK/identity only once bonding is on (after the HMAC). Details: docs/ble-auth.md.
**Tested:** owner's phone on mini #2 logged in with no key prompt (only the v2 app can). A bonded phone still shows the saved unit name in the chooser; a fresh central sees `CFLO`. Both minis `stored 1, records 1`. On air only `CFLO` adverts, RPAs.
**Not tested:** a phone on the CYD in normal mode (it's a fleet client; needs Leave fleet first); Windows bleak couldn't connect to read AUTH ("Unreachable").
**Open owner decisions (security review):** SEC-002 rotate the address within a boot (needs the previous one kept in the self filter); SEC-003 0.32.0 may have handed its IRK to a central that paired, so rotating the local IRK would close that but forces a re-pair; SEC-004 the download SoftAP SSID `CYD-Scan-%04X` + BSSID are permanent.
Item 1 below ("tracking still possible") is done by this release, apart from SEC-002/003/004.

## RESUME HERE — 2026-11-07: fw 0.32.0 on all hosts, `hardening` merged, web app deployed
**State.** All 3 hosts fw **0.32.0** and all 3 C5s random-address builds. Mini #2 `xiao_s3_rgb` leader, CYD `cyd` and mini #1 `xiao_s3` joined; roles wifi24 / wifi5 / ble. Self filter dropping rows.
**Private host BLE address (owner's call, docs/ble-auth.md "Private advert address"):** RPA, new each boot, kept until the next. Owner's phone test on mini #1 passed. Both minis' older bonds migrated (`bonds` shows `stored 1, records 1`). Wi-Fi probes stay on (owner's call): passive AP scan is declined.
**Review:** REV-001..004, 006, 007 fixed in `cf25458`; those fixes had no second review. Not tested: `bonds clear` on this build, recovery from a NimBLE host reset, and a phone on the CYD in normal mode. **The CYD's phone path is unchanged by the bond migration but its advert is new.**
**Open:**
1. **Tracking still possible:** the advert keeps the unit name (fixed, a hash of the eFuse MAC) and the service UUID, and any central can read `sid` from AUTH before authenticating. Owner decision; dropping the name costs the chooser its way to tell units apart. Tell scan-analyzer first: its proposed filter matches on those fields.
2. **scan-analyzer:** name-or-UUID exclusion is its owner's call and isn't built. It asked whether anyone else runs this firmware; owner to answer. `lastseen` lines carry neither field.
3. C5 after a NimBLE host reset (REV-008): may fall back to a hardware static address until the next boot.
4. Try the Fleet card on the phone (never tried on a phone), plus the 2-body Upload check.
5. Older items below still stand, except the two owner decisions, now settled.
**Rules learned:**
- NimBLE's timers overflow 32 bits above 4294 s, so a long `ble_hs_set_rpa_timeout` silently fires early.
- Turning host privacy on needs peer records for existing bonds; check `bonds` shows `records` equal to `stored`.
- On Windows, active BLE scans replay old addresses; use passive mode to judge when an address stopped.

## RESUME HERE — 2026-11-07: hardening pass + fleet role controls built on `hardening`; web part not deployed
**State.** Mini #2 `xiao_s3_rgb` fw **0.31.0** (COM10, leader) with C5 `xiao_c5` (COM9); CYD `cyd` fw 0.31.0 (COM13) with C5 `c5` (COM18).
Both of those C5s have the console fix and random addresses. **Mini #1 (S3 + C5) wasn't attached: still 0.30.1 and the old C5 build.** Link and fleet proto are unchanged, so all of this interoperates.
**Random C5 addresses (owner's call, docs/fleet-mode.md §17.3):** new Wi-Fi MAC and BLE scan address each boot, measured on air with a control. The probe's contents are unchanged, so it can still read as "a C5-class scanner". A passive AP scan would remove Wi-Fi probes altogether: owner decision, not built. scan-analyzer keeps its old C5 entries and needs no change; `c5_mac` on `meta` not added (its recommendation).

**Done:**
- C5 USB console can't block the scan loop (`ca332c8`).
- **Fleet on-air check PASSED** (`tools/fleet-sniff`, docs/fleet-mode.md §17.2): only Probe/Hello/Join are cleartext, no ID, name or fixed MAC in them, fresh values across 3 joins.
- Role overrides saved (NVS `fltlay`), listed in `M:l`, and "everyone full" beats them. Bench-passed on mini #2 through a reboot, on the debug build before the review fixes.
- Fleet card: coverage selector, role picker per member, "Nobody is assigned X" warning. Reviewer: no P1; REV-001..007 fixed (`eacd923`). The real `fleet.js` passes in Node with a fake BLE link. **Never run on a phone.**

**Next:**
1. Owner OK, then merge `hardening` to `main` through a temp worktree (that deploys the web app; footer `fleet-roles a`), and try the Fleet card on the phone.
2. Flash mini #1 when attached: `xiao_s3` + `xiao_c5`.
3. Owner decisions: passive Wi-Fi AP scan (above), and whether the hosts' fixed BLE advert should change too (it is a bigger tracking handle than the C5 was, but phones and bonds rely on it).
4. Still open from below: the 2-body Upload check, first real `deauth` line, mini #2 `role off` cause. Also the CYD heap during a normal-mode SoftAP download (needs the phone) and a frame replay test (needs a transmitter).
5. ~~Legacy Wi-Fi shims~~ DONE 2026-11-08 in two deploys: `#apCard`, `toggleWifi()` and the `status.js` call went first, `wifiap.js` 10+ min later.

**Bench side effects (2026-11-07 ~12:00-13:00):** the CYD's C5 was the sniffer for about 20 min, so the CYD logged no scan rows in that window. The CYD was reset 3 times (short bg files) and mini #2 rebooted several times; its log has test `plan` lines from role changes.

**Rules learned:**
- Never name a script `con.py` on Windows: CON is the console device, so python hangs at a `>>>` prompt.
- `pio run -t nobuild -t upload` breaks esptool here. Use plain `-t upload`.

## Earlier — 2026-11-06 (night): everything live and merged; only a real deauth burst is still unconfirmed
**State.** All 6 boards current, nothing in flight. Hosts fw **0.30.1** (mini #2 `xiao_s3_rgb` leader, mini #1 `xiao_s3`, CYD `cyd` client); 3 C5s (`c5`, `xiao_c5` x2) on link **v13**,
fleet proto **v4**. Roles ble / wifi24 / wifi5, 8 fixed radios shared. Webapp on `main`, footer `current-fleet j`. BLE link encryption level 2 is the default; the phone is paired with all 3.

**Done this session (details: docs/ble-auth.md, docs/fleet-mode.md §23):**
- BLE Phase B/C (fw 0.28-0.29): T10 passed on all 3, including a re-proof over the encrypted link, decline, stale bond, and a BLE export with the fleet running.
- Fleet self filter: 1411 rows from all 3 units on 0.30.1 with no own addresses (scan-analyzer checked). Own rows on 0.30.0 at 22:16-22:26 were mini #1 not yet a member.
- `deauth` lines name the sender; `others` is an array of {mac, frames}. Keys approved by scan-analyzer's owner and in its contract.
- Web: Scanner controls no longer vanish after a disconnect; Download / Upload current session also take that session's Fleet file (matched to the current FILE, not its group);
  Upload + live toggle moved to the Data page.
- Fleet field test on 0.29.0 passed: 20:57-21:57 stable, 13-minute drive, stamped position 17 m median / 66 m max from members' GPS. The 66 m is the fix's ~6 s hold-then-step at speed.

**Open:**
- First real `deauth` line on 0.30.x: never generate one (project scope). scan-analyzer will check it when it lands.
- The new-file-only fleet match (`034519d`) hasn't been run on a phone; the next Upload should post exactly 2 bodies.
- Mini #2 `role off`: cause unknown. No `leave` / `fallback` line from 18:05 on, so it predates that; the owner set it back at 20:05. An older log would show it.

**Rules learned:**
- **A new or reflashed unit with a new radio address: tell scan-analyzer.** Its `EXCLUDED_MACS` is an exact-address list.
- **Upload "Failed to fetch"** = no response. The scan-analyzer server only lives inside its Claude session or a manual `backend\run.ps1`, with no service wrapper. Check `/healthz`, and any phone VPN displacing Tailscale.
- Deploy and merge through a temp `git worktree`. Switching branches changes `platformio.ini`, so the IDE cleans `.pio/build` and holds `platforms.lock`.
- A webapp change is unproven for 10 min after deploy: unversioned modules stay cached, and `state.*Fleet` hooks skip silently on a stale copy.
- A rejoin after flashing can take 2-5 min (9 s dwell per channel vs 15 s probes). C5s flash only on a link change.

## Earlier — 2026-11-06: fw 0.29.0 LIVE on all 3 hosts: BLE link encryption on by default (level 2); merged to `main`
- **Board state:** release builds, mini #2 `xiao_s3_rgb` (COM10, leader, its C5 is wired in, only its USB is out), mini #1 `xiao_s3` (COM16), CYD `cyd` (COM13, client).
  Fleet rejoined: roles ble / wifi24 / wifi5, 0 bad frames, bonds kept across the flash. C5s not reflashed (no link change).
- **T10:** passed on all 3, including a BLE export from the leader with the fleet running (docs/ble-auth.md).
- **Mini #2 `role off`, cause still unknown:** its log (2026-11-06 18:05 on) has no `leave` / `fallback` line, and no leader `start` at the 18:06, 18:10 or 19:12 boots,
  so the role was already off at its first boot that day. The owner set it back at 20:05 with no reboot. No crash in that window. Any older log would show the real line.
- **Older notes below in this entry are the history of the same day.** Deploy webapp/merges via a temp `git worktree`, NOT by switching branches.
- **Merged:** `fleet-mode` -> `main` (`4c37e43`, pushed). Tree identical to `fleet-mode`, so Pages content did not change.
- **Built (fw 0.28.0, uncommitted in the working tree on `ble-enc`):** `PHONE_BLE_SEC` 1/2 in `phone.cpp`, web `js/ble/linkenc.js`,
  serial `bonds`. Default stays 0. Design + as-built + T10 checks: docs/ble-auth.md "Phase B/C as built". Web tokens `?v=20261106a`,
  footer `ble-enc a`. Bench envs `xiao_s3_rgb_sec1`, `xiao_s3_rgb_sec2`, `cyd_sec2`.
- **Reviewed:** reviewer (REV-001..009) + security (SEC-1..5) ran, then a second reviewer pass on the fixes; all fixed except two T10
  checks (a bonded phone's first STATUS waits for the next push; a hung AUTH read vs sec=1 Skip). Biggest fix: at sec=2 a proof
  made in the clear is voided when the link encrypts, and `gateHandle()` reads `g_linkEnc` first for that to hold across cores.
- **Not done:** no hardware test at all (T10), which needs the owner's phone for the pairing prompt. Rollout: deploy the web app
  first (merge to `main`), flash mini #2 with `xiao_s3_rgb_sec1`, then `_sec2`, then change the default.

## RESUME HERE — 2026-11-01 (night): fw 0.27.0 + Fleet-file bundling LIVE; carry-over server-verified
- **Live on all 3 hosts (flashed), pushed; webapp on `main` `a65d317` (footer `fleet-bundle m`).** Commits on `fleet-mode`:
  `dd0eb63` webapp (a ticked leader scan auto-includes its time-matching `fleet-` group: saved as a SECOND file, uploaded as
  its OWN POST, mapped together; Delete never auto-includes), `edc3d30` fw 0.27.0 (a rebooted member forwards the previous
  boot's unsent tail first: NVS `cydscan/fltcarry` v2 + boot counter `fltbootn`; anchors re-sent = meta, timesync, last
  `fleet` line with a fleet_id — scan-analyzer's request), `a02e5ca` review fixes (REV-001..010: carry only from the
  immediately previous boot + cleared on any role change/leave/fallback; backlog survives back-to-back reboots; one
  connection-generation/scanner guard per Data action; fleetSpan trims carried head rows + webapp requires the fleet end
  inside the scan span ±300 s; upload summary toast, stop on auth; NVS writes ≤ every 3 min + on lost/file change;
  logFwdFind only for undated names, cap 150; no NVS write during a transfer). No fleet/link protocol change.
- **Verified:** builds (5 host envs), flash, fleet rejoined (roles split, forwarding into `/logs/fleet-20261101-234130.jsonl`).
  As at every flash, members' previous-boot files were re-forwarded during the flash window (leader flashed first).
- **Carry-over VERIFIED on the server (scan-analyzer, bridge):** a joined upload of 3 fleet files split into 17 sessions (per
  origin boot file × fleet segment, own anchors, 0 duplicate rows); mini#1's previous-boot `a76a5cd2` tail (7 rows) was
  carried over after its reflash and stayed in its fleet segment via the re-sent fleet line.
- **Earlier note, now superseded for (a):** (a) carry-over on hardware — needs a REAL backlog: take a member out of the leader's reach (or power the
  leader off) for a few minutes, power-cycle the member, then bring the leader back; the member's `fleet` console should show
  `carry-over … loaded 1` and forward the old file first. (An RTS-toggle over USB did NOT reset the S3, and a caught-up member
  correctly carries nothing.) (b) Data page bundling — VERIFIED by the owner in the browser ("+ Fleet" tag, upload worked).
  scan-analyzer will check the earlier-boot `ofile` session once the fleet file is uploaded.
- 0.26.0 (log cache / tail fetch / switch scanner) is PHONE-VERIFIED (see the next entry).

## RESUME HERE — 2026-11-01 (evening): P6 items 1-4 DONE + field-verified; transfer fix 0.24.1 FLASHED + VERIFIED
- **0.26.0 LIVE (2026-11-01 ~22:40):** the log cache / tail fetch / switch-scanner commits + review fixes `6118839` are
  FLASHED on all 3 hosts, pushed, and the webapp deployed (`main` `7f86e00`, footer `log-cache+switch i`). Owner granted
  no-ask flash/deploy for the rest of this work. Fleet rejoined on 0.26.0, forwarding into a new fleet file. Server VERIFIED grown-file
  re-upload (leader bg file twice: 2nd stored only the 55 new rows, 0 dups). PHONE-VERIFIED by the owner: tail fetch ("Fetched only the new N
  bytes" → phone CRC == firmware CRC), cache reuse, the Switch-scanner dialog (presence + getDevices work once the browser's
  experimental Web Bluetooth flags are on; the owner's browser details live in private memory, NOT in the repo). After the reflash the leader showed each member with TWO
  file ids: EXPLAINED, not a bug — the leader was flashed first, so for ~1-2 min the members (still on their old boot)
  forwarded their old live files to the new leader; then each rebooted into a new file.
  Review-fix summary: single-flight transfers + connection generation (a switch mid-fetch can no longer mix scanners'
  data), pending request rejected on disconnect, tail form now carries a prefix CRC (`F:<path>@<off>:<file_id>:<crc32>`),
  SESS= file_id comes from fileSpan's head read, cache tags only trust matching file_ids, uploads trim a torn last row.
- **NEWER (late 2026-11-01, 4 local commits on `fleet-mode`, NOTHING flashed / pushed / deployed): log cache + tail fetch +
  switch scanner.** (1) webapp phone-side IndexedDB log cache (`data/logcache.js`, `ble/logfetch.js`, `features/cacheui.js`;
  200 MB LRU; saved/+N KB tags + Clear on the Data page). (2) **fw 0.26.0**: SESS= 6th field `file_id`, CMD
  `F:<path>@<offset>:<file_id>` -> `TAIL=<n>@<off>` / `TAILERR=path|id|off` (no link/fleet change; flash units one at a time;
  an older unit just never gets the tail form because the phone gates on STATUS `fw` >= 0.26.0). (3) webapp Switch scanner
  (`ble/devices.js`, `features/switcher.js`, `unitreset.js`, `connection.js` `switchTo`). Webapp tokens
  (`?v=20261101i`, footer `log-cache+switch i`); `connection.js` + `fleet.js` imports in main.js carry the same token.
  To ship: flash 0.26.0 (all 5 host envs build), push, deploy Pages (~10 min cache), then test on the phone: Data -> Session
  logs -> Refresh, Download one scan twice (2nd is instant, row says `saved`), let a live scan grow it (`+N KB new`), Download
  again (log line "Fetched only the new N bytes"); Settings -> Scanners -> Switch scanner... between two units.
- **fw 0.24.0 (P6 items 1-4) flashed on all 3 hosts** (mini #2 `xiao_s3_rgb` COM10 = leader, mini #1 `xiao_s3` COM16,
  CYD `cyd` COM13). `fleet-mode` `f3e7798` (fw) + `90a559e` (webapp) pushed; webapp on `main` `0bd3fe9` (footer
  `fleet-p6 e`). **Field test VERIFIED by scan-analyzer (server `b036384`, schema v11):** fleet_id `57289e09c2931f8c`
  matched on all 3 units; meta fleet keys, `evt:fleet` peer/peer_name/slot/n/plan, `pos_src` (gps/phone/fleet only)
  and 26 `devpos` (from the leader -- mini #2 had a live onboard fix, 3 sats) all as agreed; no rejects. Only errors:
  2 live batches cut mid-send (ClientDisconnect, nothing stored). Details in the bridge's two newest entries.
- **CYD as fleet client has NO phone link by design** (BT memory released). To pull its SD log: CYD Settings → Leave
  fleet (or Release from the leader's web Fleet card), download, then Settings → Fleet client... (owner did this).
- **Plan flips on mini #1 (share ↔ FULL, no `lost`):** NOT the phone connection. Correction to what scan-analyzer was
  told: BLE file streams ALREADY run `fleet::service()` per chunk (`phone::transferAlive`). Real causes found: (1) a
  transfer on the LEADER pauses its C5 poll → `ownC5Up` false after 8 s → `assignRoles()` drops everyone to FULL;
  (2) export counting passes + the session-list dir scan never serviced the fleet. Exact timeline not confirmed --
  if flips persist after the fix, ask scan-analyzer for the ordered leader + mini#1 `fleet` lines (epoch/op/plan/n).
- **Fix `1cbbee6` (fw 0.24.1), pushed + flashed on all 3 hosts (fleet rejoined, split roles OK). **VERIFIED:** a 2 MB+ BLE download on the leader (plus small ones on each unit) left planGen at 2 -- no role change:** leader holds its own-C5 verdict while
  `hostTransferRecent(LINK_STICKY_MS)`; `xferKeepAlive()` (throttled phone+fleet service) in counting passes + session
  list; `XferScope` around transferLog/Filtered/File + sendSessionList defers fleet event lines (≤4, heap on first
  use) until the transfer ends so an export's SIZE= count stays exact. All 5 host envs build. Fleet proto/link
  unchanged → can flash the leader alone. **Next:** flash (needs owner OK), re-test a phone download on the leader
  and on a client while the fleet runs: roles should stay split. No reviewer pass yet.
- **Mini #2 key ROTATED (gen 2)** at the owner's request; new pairing QR written to the git-ignored
  `qr-codes/CFLO-MINI-BEMOPU-pairing-qr.svg` by a script that never printed the key/link (deleted after). The phone must
  re-pair mini #2 from that QR. Fleet unaffected (members' fleet keys derive from THEIR keys).
- **Reviewer pass on `1cbbee6` DONE (no P1; 2 P2 + 5 P3, all fixed) → `6b98412` fw 0.24.2, all 5 host envs build,
  FLASHED on all 3 + pushed (fleet rejoined):** `c5LinkAgeMs()` (host_core) excludes transfer time (replaces the leader's held verdict + the scan
  cycle's checks); exports / session list bound to the requesting phone's authEpoch; `radio_fail` never deferred;
  deferral buffer 8, freed after use; log-cap sweep + `D:*` wipe scoped + serviced. Flash all 3 hosts with the owner's OK,
  then repeat the 2 MB leader-download test (planGen must not move).
- **P6 item 6 (P5b store-and-forward): owner SIGNED OFF; design = docs/fleet-mode.md §22 (`577c123`).** Owner chose:
  a member forwards its WHOLE current file from line 1. scan-analyzer server READY on `343a5d4` (schema v12; content dedup,
  `orow` informational; wants each origin's meta + timesync re-sent when forwarding starts — in the spec). **Built `866f40e`
  (fw 0.25.0, fleet proto v3); reviewer found 2 P1 (fg/bg toggle re-sends whole files; webapp parse.js strips/mixes
  members' timesync anchors) + 5 P2 + P3s → ALL FIXED in `c246e70`. **FLASHED on all 3 + pushed; webapp on `main` `7d8ea2a` (footer `fleet-p6 g`). Live:
  leader writes `/logs/fleet-…jsonl`, both members forwarding, 0 bad frames.** Next: owner's hardware test with
  scan-analyzer (download/upload leader file + fleet- file + member files); extra checks in the fixer's list (fg/bg
  switch, backoff during a leader transfer, `T:2:60:/logs/fleet-…` export epoch-only).** If this session died: check
  `git log` for its commits, then build all envs, run reviewer, flash with owner OK, ping scan-analyzer, bench-test.
- Open: server-side fleet grouping UI is scan-analyzer's.

## RESUME HERE — 2026-11-01 (afternoon): P6 agreed; item 5 (fleet live-POST) DONE end to end
- **P6 contract agreed** (bridge 10:05 proposal, 10:20 owner-approved reply, then Q&A). Decisions: `pos_src` =
  phone|gps|fleet (no `held`); `fleet_id` also on `fleet` evt lines (at least join/plan) + optional `peer_name` on
  member_join; item 6 (P5b store-and-forward) DEFERRED; `ms` = the unit's own millis() in files (P3 changes only
  epoch/lat/lon), live-POST `ms` = phone wall clock (owner OK'd for member rows too).
- **Item 5 DONE:** fw 0.23.0 = DETS v10 (per-unit RSSI; `g_detFleetRssi[MAX_DET][6]`) `4a4e19b`; webapp one row per
  unit (`remote.js` `unitsOf()`, member rows `scanner` + `pos_src:"fleet"`) `ba7a64d` → `main` `9e83bb3`, footer
  `fleet-p6 d`. Mini #2 (leader) flashed 0.23.0; CYD + mini #1 stay 0.22.0 (link unchanged). scan-analyzer server
  `6242c19` (schema v11) is live. **Field test PASSED:** 3 server sessions (leader + `@<member sid>` each), per-unit RSSI,
  pos_src=fleet. The leader's own 2.4/5G rows are real (the BLE role keeps the AP scan). Per-unit `src` not needed.
- **Next: items 1/2/4 (firmware, small):** meta `fleet_id`/`fleet_role`/`fleet_leader`; `fleet` evt fields incl.
  `fleet_id` + `peer_name`; `pos_src`. ALSO put fleet id/role/leader in STATUS so the webapp's live-batch meta carries
  them (scan-analyzer groups live sessions by `fleet_id`). Then item 3 (`devpos`).
- **Security note:** a boot check sent serial `key` to mini #2 and printed its phone key into a session transcript.
  The owner was told; rotation (`rekey`) is the owner's call — not done yet.

## RESUME HERE — 2026-11-01 (morning): fleet P1-P5 DONE + deferred P4 items; nothing pushed or deployed
The overnight timer didn't fire (the session was de-authenticated); the owner ran it in the morning.
**Fleet mode P1-P5 are built, reviewed (reviewer agent each phase, + security for P2), fixed and bench-passed.**
`fleet-mode` @ `4e62fbe`. Design + as-built notes: docs/fleet-mode.md §17-§20 (+ .1 review notes).

**What landed this session (P5 + extras):**
- `2d23619` **P5:** client forwarding (DetBatch/RidBatch), the leader's PSRAM remote table + `mergeRemote()`, the
  `g_detVia` "seen by" mask, `DevGroup.via`, the fleet-only cycle, STATUS alert maxima, DETS **v9** `via`, and the web
  "seen by" chips. `HOST_MAX_DET=192` on the S3 envs.
- `85b0271` No coverage split while the leader's own C5 is down (found on the bench).
- `77b65bf` P5 review fixes. The big one: **the leader's own log rows stay its own measurements** (members' RSSI/flags
  are display-only). Also: RX pool 48, a 768-row remote table, the forwarder no longer starves rows, terminated RID
  strings, a 2 s fleet-only cadence.
- `8fead0f` docs §20. `4e62fbe` **single-band roles restrict the C5's band** (AP scan 12.4 s → 3.0 s on 2.4 only) +
  the `M:o` per-member role override.

**Bench results (mini #2 leader + CYD client; all PASS):**
- **Merge:** the leader (BLE role) shows the client's probes, e.g. 82 rows = 10 own + 14 member-only + 58 both.
- **Ageing:** client off → its rows age out (~20-30 s).
- **Traffic:** ~0.75 DetBatch frames/cycle, `rx drop 0`.
- **Fleet-only view:** the leader's C5 parked in its bootloader (`esptool --after no-reset`) → a fleet-only view, roles
  drop to FULL within ~8 s; restored → the split returns in 2 s.
- **Heap:** mini internal ~88 KB free (min ~84 KB); CYD client ~78 KB (min ~73 KB).
- **Builds:** all 8 envs pass. Static RAM: cyd 114,648 B; xiao_s3 129,028 B (+25 KB from the 192-row table); c5 80,156 B.

**Board state (updated after the morning run):**
- mini #2 + the CYD are on the **release** envs (`xiao_s3_rgb`, `cyd`) at **fw 0.22.0** (`b5b99d9`, `build release`).
  The debug-only serial commands (`fleet m/fk/add/crash/trace`) are gone: use the `*_fleetdbg` envs for bench work.
- Both C5s run the latest `c5` / `xiao_c5`.
- **Mini #1 FLASHED (2026-11-01):** release `xiao_s3` (S3 COM16, `10:B4:1D:E7:ED:A0`) + `xiao_c5` (COM23,
  `10:BD:A3:CF:02:DC`), fw 0.22.0, link v12. Its S3↔C5 link is up, role off. To join: `fleet client` on its serial (or
  the web Fleet card "Make this scanner a fleet client"), then on mini #2 → Fleet card → Add member (the phone needs
  mini #1's key). Mini #2 was unplugged at the time.
- The webapp Home fleet cue is deployed (`efe7c9b`, `fleet-cue c`).
- **3-UNIT FLEET RUNNING (2026-11-01):**
  - mini #1 is a client (enrolled from the owner's phone) — layout leader ble, CYD wifi24, mini #1 wifi5.
  - Merge from two clients, rx drop 0.
  - mini #2 re-flashed with the member-name fix (`3ad60ce`, release `xiao_s3_rgb`).
  - **`fleet-mode` is PUSHED** to GitHub (no PR, not merged to `main`).
  - Next: **P6** — draft the scan-analyzer contract proposal via REMOTE.md (log keys: meta `fleet_id` / `fleet_role` /
    `fleet_leader`, row `scanner` / `ofile` / `orow` for P5b, `devpos`, the `fleet` evt fields, `pos_via`, live-POST
    member rows) and build after it is agreed.
  - **2026-11-01: P6 proposal SENT** (bridge entry 2026-11-01 10:05, DRAFT). Items: 1 meta fleet keys, 2 `fleet` evt,
    3 `devpos`, 4 `pos_via` (or `pos_src`), 5 live-POST of member rows with a per-row `scanner`, 6 P5b `scanner` /
    `ofile` / `orow`. scan-analyzer checks with the owner first. WAIT for its answers (questions a-d) before building.
- The CYD is joined to mini #2 (channel 1, CYD = wifi role, leader = ble).
- The CYD's time preference is "prefer device", so its clock shows GPS (src 2) rather than fleet (4) while its own GPS
  is fresh. That is correct for that setting.
- Ports: mini S3 COM10, mini C5 COM9, CYD COM13, CYD C5 COM18.
- **Mini #1 is untouched (still link v11).**

**Ready, waiting on the owner:**
1. ~~Webapp deploy~~ **DONE** (`main` `7f81c50`, footer `fleet-p5 b`). ~~Release flash~~ **DONE** (mini #2 + the CYD at
   fw 0.22.0).
2. **Owner review** of the running fleet and the web "seen by" chips.
3. If OK, the owner's plan:
   - **push `fleet-mode`** (nothing fleet-related is on GitHub yet);
   - **flash mini #1**: both halves, `xiao_s3` + `xiao_c5`, link v12; enroll it from the Fleet card;
   - start **P6** (the scan-analyzer contract via REMOTE.md).

**Open items / next steps:**
- **P6 (needs the scan-analyzer contract via REMOTE.md):** the `fleet` evt extra fields, meta `fleet_id` / `fleet_role` /
  `fleet_leader`, `devpos`, `pos_via`, live-POST of member rows, and P5b LogRows store-and-forward (`scanner` / `ofile`
  / `orow`). None of these are built: they change the log schema.
- **Untested:** 2+ clients (only one client on the bench) — bring mini #1 in (flash both halves) to test N=3 roles,
  WIFI24/WIFI5 on real members, and multi-member delivery. Also the sniffer check (P1 step 9 / P2).
- **Web:** no `M:o` / plan-mode / layout editor controls yet (§16.1); serial `fleet m o:…` / `fleet m p:…` meanwhile.
- **Accepted / minor:** see docs §20 "Accepted / open"; a group's `rep` can be a member row.


## Earlier — 2026-11-01: fleet P3 (time + position) BUILT; P2 done + owner-tested from the phone
- **P3:** docs/fleet-mode.md §18. Bench: the CYD client takes the fleet clock (src 4) + the leader's tz, and its bg log
  renamed 0.4 s after join. **Position PASSED** with the phone on the leader (CYD `pos src 4 live`, 2 self MACs; the
  owner confirmed `Pos: fleet`). Self-MAC drops: by inspection only. P3 review fixed (`fbd786e`, docs §18.1). Next: P4 (link v12 scan
  plan; all boards flash).
- The webapp `status.js` change (source 4 icons) is committed on `fleet-mode` only — deploy with the next webapp push.

## Earlier — 2026-10-31 (late): fleet P2 (enrollment + auth) BUILT, REVIEWED (reviewer + security, all fixed) + bench-PASSED; owner tested the Fleet card from the phone
- **P1:** done (see below). Its sniffer check (step 9) moves to P2's sniffer check.
- **P2:** built and bench-passed — details, deviations and results in **docs/fleet-mode.md §17**.
  - Both boards run the `*_fleetdbg` builds: mini #2 = leader (channel 1) with the CYD enrolled + joined; the CYD is a
    fleet client.
  - The web Settings → Fleet card + `fleetcrypto.js` are written. The crypto was verified against the firmware in
    Node; the card has NOT been tried on a phone. Webapp tokens are bumped to `20261031d` (`#uiver` `fleet-p2 d`) but
    NOT deployed.
- **Next:**
  1. ~~Review + security fixes~~ DONE (docs §17.1). Committed on `fleet-mode` (local, not pushed).
  2. Deploy the webapp (owner's go-ahead), then the owner tests the Fleet card from the phone (add a member from Scanner keys, remove/release, channel), then deploy
     the webapp.
  3. Sniffer check (spare S3).
  4. P3 (time/position push). P4 must re-send member slots after a removal (§17.1).

## Earlier — 2026-10-31: fleet mode P1 built + reviewed + bench-tested
**What fleet mode is:** 2+ of the owner's scanners cooperate over ESP-NOW and split RF coverage. Design + every owner
decision: **[docs/fleet-mode.md](docs/fleet-mode.md)**. §16 (2026-10-31) overrides earlier sections; all §15 questions
are answered. Key decisions:
- **Roles:** the CYD is only ever a fleet **client**. It reboots into that mode, never starts NimBLE (no phone link), and
  releases its BT memory. A **mini is the leader**: it holds the phone link and enrolls clients. Up to 6 clients (one
  encrypted ESP-NOW slot spare).
- **Coverage split:** the role layout is user-editable. When a member drops out past the grace period the leader
  reassigns its coverage, and restores the saved layout when it rejoins.
- **Logging:** the leader logs everything. Clients forward their log rows verbatim (acked and resumable) and keep their
  own SD copy as a backup. `fleet_id` plus per-row `scanner`/`ofile`/`orow` let scan-analyzer spot duplicate or partial
  client uploads.
- **Position:** one fleet position, phone by default. The user may pick the leader's GPS or a member's GPS instead. Each
  unit's own fix is logged as `evt:"devpos"` with `used` 0/1.

**Why the CYD can't lead (measured 2026-10-30 on the CYD, throwaway probes):**
- Normal boot leaves ~58.7 KB free heap. Wi-Fi STA costs ~51 KB, so with NimBLE up the CYD **OOM-aborted**.
- Fleet-client boot (no NimBLE + `esp_bt_mem_release`) ran Wi-Fi + ESP-NOW at **~114 KB free**. A real dual-mode build
  should land around 95–105 KB.

**P1 bench results (2026-10-31, CYD = `cyd_fleetdbg` on COM14, mini #2 = `xiao_s3_rgb_fleetdbg` on COM10):**
- **Step 1 leader:** up on ch 1, MAC set **direct**, 12 Mbps accepted, pool 16x1480 in PSRAM, txpwr 8 dBm. Fleet cost
  44.8 KB internal heap (166.4 → 121.6 KB).
- **Step 2 client:** `BT memory release -> 0`, MAC set direct, pool 8x256 on heap, txpwr 6.25 dBm. Heap **139.8 KB before
  the fleet, 84.3 KB after (fleet cost 55.5 KB)**. The probe's 114 KB doesn't carry over: the real boot has SD/sigdb/whitelist
  allocated before `beginClient()` (the fg dedup-table skip is already included).
- **Step 3 soak, 30 min — stable, but fails the ≥95 KB heap target:**
  - **CYD:** heap 83.7 KB steady with no drift, min 75.2 KB, largest block 73.7 KB. No resets, and the C5 link was up for all
    896 cycles.
  - **Mini:** internal heap 121.6 KB (min 116.6 KB), PSRAM flat at 8.3 MB. 908 TX ok, 0 fail.
  - **Delivery:** about 97–98% at ~-31 dBm (250 B 446/~454, 1300 B 439/~454). 16 gaps spread evenly (~1 per 2 min). All
    885 pongs ok, 0 RX drop.
- **Step 4 rate/pad:** at 1 Mbps over 3 min, 45/45 (250 B) and 44/45 (1300 B) — the same as 12 Mbps at bench range, so
  only a range test will separate them. Pad default (0) already alternates 250/1300, which the soak covered. Restored 12M.
- **Step 5 remac:** CYD `fleet remac` took 9.2 ms (direct set, ESP-NOW up). The **~15 s gap** seen before pings
  resumed was the unread-console artifact (see "Traffic stall"). **The leader never expires old peer MACs** (stale rows kept counting up; cleared only by a leader reboot) — fix in a later
  phase (peer timeout). Reboots: the CYD rejoined instantly with a new boot MAC; the mini rebooted (via reflash) back
  into leader and the CYD followed its new MAC automatically. Boot-time randomization holds; no fallback needed.
- **BUG found + fixed:** `fleet::setRole()` saved NVS but not `g_role`, so after `fleet leader on` the live leader
  reported `role off` (`fleet leave` would have said "not in a fleet"). Now `setRole()` updates `g_role`, and the `leader on`
  handler reads the old role first so its "was a client → restart" check still works. Both boards reflashed.
- **Step 7 `fleet leave` (serial):** OK — rebooted to normal, phone BLE advertising again. **Not yet tested:** the touch / BOOT
  "Leave fleet" UI path — **PASSED (owner, 2026-10-31)**.
- **Step 8 crash fallback:** OK using a new **`fleet crash`** serial command (`abort()`, `FLEET_DEBUG` builds only): 3
  consecutive panics → `boot-loop guard: 3 boots, last reset reason 4 -> leaving the fleet`, normal boot, `role off`.
  The SD `evt:"fleet","op":"fallback"` line isn't checked yet (read the card / download the bg log).
- **Step 6 mini under load (phone authed on the mini, fleet on ch 6) — PASS:**
  - Idle with the phone: ~95%. A BLE download of 168.8 KB: ~100%, leader heap min 116 KB.
  - SoftAP up ~6.5 min incl. a 170 KB Wi-Fi download: 95%, leader heap min 113 KB (the AP cost ~3 KB internal).
  - **After `AP down` the fleet stayed on ch 6** (retune fix proven), 16/17.
  - `fleet ch 6` itself seemed to cause a **~25 s outage** before pings resumed (like the ~15 s after remac). **RESOLVED,
    not a fleet bug** — see "Traffic stall" below.
- **Traffic stall — RESOLVED (2026-10-31).** A scripted run (`fleet trace on`: per-ping tx/rx lines with the actual RF
  channel; both consoles read on one clock) showed **no stall**:
  - remac: the next ping arrived 150 ms later. Same-channel retunes: no loss.
  - A real channel change loses pings only while the two boards are on different channels.
  - **Cause:** the earlier stalls happened only while the **mini's USB-CDC console was plugged in but unread**. HWCDC
    retries each `Serial` write 20 × 100 ms, so the chatty mini loop() crawled and pings went out every ~12–17 s.
  - **Fix:** `src/xiao_s3/main.cpp` sets `Serial.setTxBufferSize(4096)` before `begin` and `setTxTimeoutMs(0)` after. With
    the console closed, pings now arrive every ~2 s (age ≤ 1 s).
  - This also affects a mini powered from a laptop USB port without a terminal open (scanning + phone link too, not just
    the fleet).
  - **TODO, next C5 flash:** the C5 (USB-CDC console) has the same default; apply the same two lines.
- **CYD heap regression found + fixed:** the `strftime` date on the Wi-Fi index cost the CYD **~4 KB** (+1 KB static, ~3 KB
  heap; fleet-client heap 84 → 80 KB). A/B confirmed; now `gmtime_r` + `snprintf` + a month table. Back to 139,836 B before
  fleet start / ~84.2 KB after; `cyd` static 114,016 B.
- **Owner UX feedback (web app, not fleet):** the SoftAP join QR is drawn in the web app on the same phone that has to
  scan it. Proposed: Copy-password + "Open Wi-Fi settings" (Android `intent:` link, with a fallback) + the existing log-index
  link, with the QR folded under "for another device". Chrome's "can't download securely" warning is the plain-HTTP AP, and
  it is expected (a self-signed HTTPS cert would be worse). Add a note on the card.
  - **Built + deployed** as `main` `0345a44` (webapp `20261031a`): Copy password, Open Wi-Fi settings, folded QR.
  - **Owner test feedback (2026-10-31), still to fix:**
    1. The card was very slow to appear and looked like a failure. The mini only shows it once STATUS carries the AP
       creds; STATUS pushes every 1 s in download mode, so the delay is entering download mode / bringing up the AP. Show a
       "Starting device Wi-Fi…" state the moment the toggle is tapped.
    2. **The log index link must be a large button, not a link in small copy.** Required steps never go in `.sub` text
       (owner UX rule). The whole join → open → download flow needs a guided stepper.
    3. **This run the log index didn't load:** no `[webshare] GET` reached the mini (AP up, heap 113 KB) — phone-side
       routing, cause TBD.
  - **Stepper BUILT + deployed** as `main` `2ead774` (webapp `20261031b`, `features/wififlow.js`, `#wifiDlg`). It opens
    instantly with a counting spinner, then steps 2→3 on return from Wi-Fi settings, 3→4 on return from the download tab.
    Legacy shims for cached phones: hidden `#apCard` stub, no-op `renderApCard()`, `toggleWifi()` — remove in a later deploy.
  - **"Index won't load" was the phone, not the mini:** no request arrived for a long while, then `GET /` was served
    instantly. Android routes over mobile data after "Connected without internet" until it settles. Step 3 now says so.
  - Cancel button added (`main` `c9a9acb`, webapp `20261031c`).
  - **Firmware side DONE (uncommitted on `fleet-mode`; mini #2 runs it in `xiao_s3_rgb_fleetdbg`; ships with the next
    release):**
    - STATUS part 2 **`apsta`** = SoftAP stations (`webshare::stations()`), sent with `apssid`/`appsk`/`apurl`.
    - The **index page is newest-first**: a big "Download current log" button, then the 40 newest `/logs/` files by FS write time
      (bounded heap insertion sort, freed per request), with a "N older logs not listed" line.
    - **AP station join/leave/IP-assigned console trace** (timestamped, no MACs), plus index build time.
    - Cost: `cyd` static RAM +1,008 B (114,032 → 115,040), flash +16 KB, likely `gmtime_r`/`strftime` for the date column. Weigh
      this against the CYD's tight DRAM before release; dropping the date would recover it.
  - **Owner verdict 2026-10-31: RESOLVED.** Page load felt quick, the newest-first list and current-log button were seen,
    and "phone joined ✓" auto-advanced. "As streamlined as it can be without a standalone app."
- **Still open:** step 9 (sniffer), step 10 (normal-mode CYD heap during a SoftAP download).
  **Both boards are on fleet ch 6** (CYD client, mini leader); set `fleet ch 1` on both to restore the default.
- **Heap decision (owner, 2026-10-31):** accept ~84 KB steady / 75 KB min for P1; 95 KB was a margin estimate, not a limit.
  **Before P5b** (LogRows store-and-forward), set a hard floor (e.g. min ≥ 50 KB plus a heap-check load-shed) and size the
  queue from the measured headroom.

**P1 state (branch `fleet-mode`, nothing committed; flashed to the CYD + mini #2 for bench testing 2026-10-31):**
- **New code:** `src/host_common/fleet_proto.h` (wire structs), `fleet.h` (facade), `fleet_link.cpp` (roles/NVS, boot-loop
  fallback, random-MAC STA + ESP-NOW, RX pool/queues, FLEET_DEBUG ping/pong + stats).
- **CYD** (`src/cyd/main.cpp`): fleet-client boot path, screens `SCR_FLEET` / `SCR_FLEETLEAVE` / `SCR_FLEETENTER`,
  Settings → "Fleet client…" (shows "Leave fleet…" in fleet mode), fleet glyph in the status bar, forced bg scanning,
  SoftAP download disabled.
- **Mini** (`src/xiao_s3/main.cpp`): leader or client starts after `phone::begin()`.
- **Shared code:**
  - `webshare` uses AP_STA on the fleet channel, retunes on `stop()` and allows 1 station while the fleet is up.
  - Serial `fleet` commands (below).
  - `logFleetEvent()`.
  - `phone::loadTz()` split out of `phone::begin()`.
  - `pushStatus()` returns early when no phone is authed.
  - **FW 0.20.0**.
- **`platformio.ini`:** only two bench envs added, `cyd_fleetdbg` and `xiao_s3_rgb_fleetdbg` (`-DFLEET_DEBUG`).
  Normal envs send no fleet frames in P1.
- **Builds (2026-10-31, all OK):**

  | Env | RAM (B) | Flash (B) |
  |---|---|---|
  | `cyd` | 114,016 | 1,489,961 (+144 RAM, +19 KB flash vs 0.19.0) |
  | `cyd_fleetdbg` | 114,032 | 1,494,377 |
  | `xiao_s3` | 103,332 | 1,331,238 |
  | `xiao_s3_rgb` | 103,476 | 1,341,525 |
  | `xiao_s3_rgb_fleetdbg` | 103,484 | 1,345,445 |

- **Reviewed:** no critical findings. All 6 code-fix items rated important plus all minor items fixed, notably:
  - the SoftAP stop stranding the fleet on ch 1;
  - `fleet leader on` killing a live AP;
  - a partial-alloc retry panic;
  - `fleet ch` not retuning;
  - the unused 9.6 KB fg dedup table on a client is now skipped;
  - `Sync.selfMacs` resized to 9 (`Sync` = 91 B);
  - the boot guard now counts consecutive crash boots only.
- **Encrypted-peer limit:** confirmed **7** at runtime (sdkconfig `CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM`; the header's
  `ESP_NOW_MAX_ENCRYPT_PEER_NUM 6` is only a constant).

**Serial console (USB):** `fleet` (status) · `fleet client` (saves role + reboots) · `fleet leave` (reboots to normal) ·
`fleet leader on|off` (minis only) · `fleet ch <1-13>` · `fleet remac` (re-randomize the STA MAC, prints the time taken).
Debug builds only: `fleet rate 1|12` · `fleet pad 250|1300|0`.

### P1 bench test checklist (owner, next session)
**Flash** with `PYTHONIOENCODING=utf-8 PYTHONUTF8=1 "$HOME/.platformio/penv/Scripts/pio.exe" run -e <env> -t upload
--upload-port <COM>`, and read serial with pyserial (memory: serial-capture-windows-pyserial).
- **Mini #2:** env `xiao_s3_rgb_fleetdbg`. Its S3 and C5 both enumerate as `303A:1001`, so pick the S3 by MAC
  `10:b4:1d:eb:96:04` (`esptool chip_id`). Last seen on COM10, but ports move on replug.
- **CYD:** env `cyd_fleetdbg`, CH340 `1A86:7523` (was COM14). The first upload attempt often fails with "Wrong boot
  mode"; just retry.

**Steps:**
1. **Mini as leader:** type `fleet leader on`. Expect `[FLT] leader up: ch 1 mac … (set direct|via stop/start)
   maxPayload … txpwr 8.00 dBm rate 12M pool …(psram|internal)`, then heap lines and 5 s ping stats.
   - Note which MAC-set path was used ("direct" vs "via stop/start") and whether the 12 Mbps rate was accepted (a
     one-time warning prints if not).
2. **CYD as client:** type `fleet client`, or use Settings → Fleet client… → confirm. Expect `BT memory release -> 0`
   and `fleet client up, free heap N`.
   - The screen shows FLEET CLIENT. Once a ping is heard it changes from "Searching… ch 1" to "Leader xx:xx -NN dBm",
     and the glyph turns green.
3. **Memory soak:** 30 min with scanning running.
   - **CYD free heap ≥ 95 KB steady**, min-free and largest block logged.
   - C5 link dot stays green, no aborts.
   - Mini internal + PSRAM headroom logged.
4. **Delivery:** compare the pong ratio at `fleet rate 1` vs `12` and `fleet pad 250` vs `1300`.
5. **MAC re-randomize:** run `fleet remac` on the CYD and read "re-randomized in X ms". Then reboot each side in turn and
   confirm pings resume.
   - If a rejoin after a reboot fails, fall back to boot-only randomization (owner-approved).
6. **Mini under load:** with the phone connected, measure delivery while idle, during DETS pushes, and during a BLE log
   download. Also start a SoftAP download while the fleet is up.
   - The fleet must stay on its channel after the AP stops. Try `fleet ch 6` first to prove the retune fix.
7. **Leaving fleet mode:** test "Leave fleet" on the CYD (Cancel is the default; works from touch and BOOT; respects the
   touch lock) and `fleet leave`. The CYD must reboot to normal mode with the phone link back.
8. **Crash fallback:** force 3 consecutive crash boots and check that `evt:"fleet","op":"fallback"` is logged and the
   unit boots normally.
9. **Optional (sniffer):** each frame's addr2 must not equal `scannerIdHex()` or the base MAC.
10. **Existing-risk measurement:** free heap on a **normal-mode CYD during a Wi-Fi (SoftAP) log download** with NimBLE up.
    AP mode likely costs the same ~51 KB as STA, so this existing feature may be near OOM. If it is, gate it behind a
    heap check or `NimBLEDevice::deinit()` it first (separate fix).

**After P1 passes:**
- Commit P1 on `fleet-mode`. Commit message on request; don't push until the owner says.
- Then **P2**: enrollment + auth, meaning the `M:` CMDs, Probe/Hello/Join/JoinAck, LMK peers, the seq window, release
  and unjoin, and the web `fleet.js` + `fleetcrypto.js`.
- Later phases: P3 time/position push · P4 link **v12** ScanPlan (flash ALL boards together) · P5 forwarding/merge +
  DETS v9 · **P5b** LogRows store-and-forward · P6 scan-analyzer contracts via REMOTE.md (`fleet`/`devpos` evts, meta
  `fleet_id`/`fleet_role`/`fleet_leader`, row `scanner`/`ofile`/`orow`).

**Side note, existing and NOT fleet-related:** about every 30 s the CYD's 2.4 GHz and 5 GHz counts dip to 0 and recover
over a few polls. This is the C5's 30 s TTL expiring Wi-Fi entries before the next AP scan refreshes them. It is
cosmetic on the live list and DETS; the log is unaffected. Worth its own look.

## Latest — 2026-10-29 (eve): fw 0.19.0 = DETS v8 + richer live-POST rows (scan-analyzer "Live-POST v-next", owner-approved)
DETS rows append `cid` (4-hex) / `uuid` (4-hex short or 32-hex) / `sig` (`host_status.cpp` `pushDetections`); the web app
(`features/remote.js` `liveRow`) forwards them plus `ie`, and drops fallback names (`realName()`). **Flashed: CYD + mini #1
(`xiao_s3`) + mini #2 (`xiao_s3_rgb`) — all hosts on 0.19.0.** Web app pushed (`?v=20261029f`). Server already
expands short UUIDs on ingest. **Verified end-to-end 2026-10-29** by scan-analyzer (s42–s45 + odid-tx: live sig/tier/flags match the file log). Done.

## Earlier — 2026-10-29: fw 0.18.0 (explorer + BLE auth Phase A) on ALL THREE HOSTS; explorer upload fixes (webapp, pushed)
**RESUME HERE.** Explorer + auth Phase A committed and pushed as `6145c18` (web app live on Pages). **Every host is now on
0.18.0** (owner's requests, for hardware testing — the no-flash-until-auth rule is satisfied):

| Host    | Env           | Port / MAC                    | Name               | sid            | Key gen | Pairing QR                                  |
|---------|---------------|-------------------------------|--------------------|----------------|---------|---------------------------------------------|
| CYD     | `cyd`         | COM14 (CH340)                 | (CYD Settings → Phone key) | —      | 1       | on the CYD screen                           |
| Mini #1 | `xiao_s3`     | COM16, 10:b4:1d:e7:ed:a0      | CFLO-MINI-GABAME   | `a0ede71db410` | 1       | `qr-codes/CFLO-MINI-GABAME-pairing-qr.svg`  |
| Mini #2 | `xiao_s3_rgb` | COM10, 10:b4:1d:eb:96:04      | CFLO-MINI-BEMOPU   | `0496eb1db410` | 1       | `qr-codes/CFLO-MINI-BEMOPU-pairing-qr.svg`  |

(COM numbers move on replug — identify the S3s by MAC with `esptool chip_id`.) **`qr-codes/` is git-ignored: each QR holds
that device's secret key; never commit it.** The mini QRs encode the serial `key` link (read over USB-CDC with DTR high,
RTS low so the board doesn't reset), rendered to SVG by the vendored `webapp/js/vendor/qrcode.js` under Node, with the name
+ sid as a caption. A rekey invalidates the QR — regenerate it the same way.

**Tested by the owner so far:** T3 on the CYD (pairing via the Phone-key QR, authenticated connect); the file explorer on
the CYD (browse / download / upload / rename / mkdir / delete; a 293 KB JPG upload completes); **T2 PASSED** (the current
web app works with a 0.17 device — mini #2 before its flash; "Open file explorer" just shows the version error). Pairing
the minis from their new QRs not yet confirmed.

Web-app fixes this session (webapp-only, all pushed; tokens now `?v=20261029e`, `#uiver` `ble-auth e`):
- `d8f819b` — upload confirmation added; first attempt at the upload failure (pausing GPS) — superseded.
- `822544b` — **uploads failed with "GATT operation failed for unknown reason"** (25–35 % in, later instantly): the ~5 s
  phone-GPS write collided with a chunk write; Android runs ONE GATT op per connection and rejects a concurrent one.
  New `webapp/js/ble/gattq.js` `gattWrite(ch, data)` serializes writes; `gps.js` (`sendGps`) and `features/files.js`
  (`fsReq`, `F:` download) go through it. **Any future write that can run during a long transfer must use `gattWrite`**
  (other one-off button writes in `commands.js` etc. still call `writeValue` directly — fine while the explorer dialog is
  modal, but convert them if that changes).
- `cb207c9` — upload prompt is now the shared in-app `askConfirm` dialog (auth.js `#secDlg`): title **"Confirm Upload"**,
  "Upload selected to `<folder>/`?"; an existing name gets a separate red **"Replace file?"** prompt. (Owner feedback: the
  old native prompt listed the file name twice and read like an overwrite warning; native `confirm()` titles can't be
  changed.) The explorer's **delete** prompt is still native `confirm()` — offered to convert, not asked for yet.

**Next:** the remaining Phase A hardware tests from `docs/ble-auth.md` §"Hardware test plan" — pair each mini from its QR
(T3/T4 on a screenless host), T4 deep link combined with `#server=`, T5 attacker with nRF Connect (no notifications, writes
ignored, kicks, lockout), T6 replay rejected, T7 rekey (CYD NEW KEY + serial `rekey confirm`; then regenerate that QR), T8
20 connect/disconnect cycles incl. mid-transfer, T9 explorer items (upload speed still unmeasured; `.upl` sweep after a
mid-upload reboot; `max_files` handle budget). Optional: convert the explorer's delete prompt to `askConfirm`. Open issues
list below still applies.

## Earlier — 2026-10-28 (late): SD card file explorer, fw 0.18.0 (now COMMITTED `6145c18` + pushed; all hosts flashed 2026-10-29)
Owner's rule was: don't flash 0.18.0 until BLE link security is handled — Phase A (below) is that fix; it is built, reviewed
and now on all three hosts for hardware testing. The status text in this section is as of 2026-10-28/29 morning (the
"Blocker" and "NOT flashed" notes are historical); builds pass for `cyd`, `xiao_s3`, `xiao_s3_rgb`.

**What it is.** Owner-chosen scope: the WHOLE card, with browse / download / delete / upload / rename / new folder,
from a Data-page card ("SD card files" → `#filesDlg`).
- Device: new `src/host_common/host_files.{h,cpp}` (both hosts; `files::service()` runs from
  `core::servicePhoneCommands`). CMD **`Z<seq>:<op>:<arg>`** — `l` list (paged ~4 KB, `MORE\t<skip>`, request
  `l:<skip>:<dir>`), `m` mkdir, `r` rename `from\tto`, `d` delete file / empty folder, `u` upload `<size>:<path>`,
  `a` abort — plus **`U<seq byte><bytes>`** upload chunks (≤ `phone::UPLOAD_CHUNK_MAX` 500; the web app sends 240).
  Every op answers with ONE `FS=<n>:<seq>` LOGDATA payload (`OK…` / `DONE…` / `ERR\t<why>`). The NimBLE callback
  (`phone.cpp`) only fills one slot each (`g_fsCmd`, `g_upBuf`); all SD work is on loop(). **`F:<path>`** download
  widened from `/logs/` to any `files::safePath` path (`g_dlFile`/`g_streamingPath` 48 → 100 B).
- Uploads stage to `<path>.upl`, then old → `<path>.bak`, `.upl` → path, delete `.bak` (rolls back on failure);
  8 MB cap; 30 s idle / disconnect abort; size must be digits; zero-byte uploads finish at once.
- Guards (after the security + reviewer passes): `safePath` = absolute, < 96 chars, no control / `\:*?"<>|`, no
  empty / `.` / `..` parts, no part ending in `.` or space (FAT aliases `/logs.` → `/logs`). `logPathProtected`
  (now public, host_log.cpp) + the CYD `exploreHolds` are **case-insensitive** and include the in-progress `.upl`
  (`files::uploadHolds`). `/` and `/logs` can't be renamed/deleted; no rename onto a protected path or into
  itself; `.upl/.tmp/.bak` names refused as targets; `/logs/` names ≤ 47 chars (the log code's 48-B buffers —
  a longer one would wedge `enforceLogCap`). Case-only renames allowed.
- Web: `webapp/js/features/files.js` (NEW file, so it updates with the page) keeps its OWN LOGDATA listener for
  FS= framing (download.js untouched — it ignores FS= headers); drops replies whose `<seq>` isn't the pending one;
  downloads go through download.js's "save" sink and only count as saved when every byte arrived; gated on
  STATUS `fw` ≥ 0.18.0. Wiring in `main.js`, markup in `index.html`, styles in `scss/_base.scss` (→ style.css
  built). Tokens bumped to `?v=20261028b`, `#uiver` = `sd-files` (not deployed). CLAUDE.md CMD list updated.
- `FW_VERSION` 0.18.0.
- Note: an automation added `.gitignore` entries for `host_files.h` + `files.js` (false positive: the comment text
  `F:/Z:` looked like a drive path). Comments reworded and the entries removed with the owner's OK — check
  `git check-ignore` before committing so the new files aren't left out.

**2026-10-29 update — SEC-H3 Phase A firmware WRITTEN (builds: cyd, xiao_s3, xiao_s3_rgb; NOT flashed, NOT committed).**
Design/decisions in `docs/ble-auth.md`. New: `src/host_common/devkey.*` (NVS key + generation, pairing URL, HMAC-SHA256,
ct-compare), AUTH characteristic `…0007` + auth state machine + notify-to-authed-handle-only gating in `phone.*`
(`authed()` replaces `connected()` for transfers/DETS/phone-GPS freshness; `connected()` kept for UI), `host_serial.*`
(`help`/`key`/`rekey [confirm]`), CYD Settings → Phone key (`SCR_PHONEKEY`/`SCR_REKEYCONFIRM`, QR v6 scale 5), grey/blue BLE
icon, `phonecfg` audit lines (`logPhoneCfg`). `PHONE_BLE_SEC` = 0; Phase B bonding not started. Still to do: reviewer +
security passes, hardware tests T2–T8, the web app (separate agent), then flash. The blocker below is what this fixes.
**Review fixes applied (same day):** peer MAC filter set only after auth (SEC-M1); notify snapshots the authed handle (NONE =
broadcast in NimBLE); `devkey::hmac`/`rekey` return status (HMAC failure rejects without a lockout count; failed NVS write
shows "NOT SAVED" on the CYD + serial warning); `phone::authEpoch()`/`transferAlive()` abort transfers when the peer changes and
run `service()` inside chunk loops; AUTH onWrite stops after max tries / with no nonce, rekey refreshes the nonce; bounded
AUTH builder + static buffer (`PHONE_DEBUG_STACK` prints the NimBLE task stack high-water after auth); kick queue (4);
`uploadOverrun` no longer clears `g_upReq` (consumer eats the stale chunk); QR byte-capacity check; Phone-key screen redraws
on gen change and times out after 60 s; legacy `D:` uses `safePath` + `logPathProtected` and audits only real deletes;
SoftAP PSK no longer logged; `safePath` rejects bytes >= 0x7F.

**⛔ Blocker — BLE link has no pairing/auth (security review SEC-H3).** Any central in range that connects while
the owner's phone isn't connected can now read, overwrite, rename and delete anything on the card — e.g. replace
`/signatures.csv` (drop camera rules / raise thresholds) or `/whitelist.csv` (mute a tracker) to blind the scanner.
Before 0.18.0 the worst case was deleting logs (`D:*`). The same gap already exposes `D:*`, `A:` whitelist adds,
and the SoftAP password in STATUS (`appsk`). Planned fix to design first thing: **BLE bonding with LE Secure
Connections, passkey shown on the device** (CYD screen; the screenless mini needs another way: LED/serial,
just-works + allow-list, or a button), then `READ_ENC`/`WRITE_ENC` (or authenticated) on CMD / LOGDATA / STATUS,
and decide what (if anything) stays readable unauthenticated. Must keep Web Bluetooth on Android Chrome working
(it triggers the OS pairing dialog on an encrypted characteristic). Lighter interim idea also on the table: an
on-device notice / `evt` log line whenever sigdb/whitelist is changed by the phone.

**Other open issues (smaller, not fixed):**
- SEC-L4: an open upload holds an extra SD handle; not verified the worst case fits `max_files` = 5 (live log
  appends + Explore `g_viewFile` + webshare + streaming + upload).
- REV-008: `.upl` files left by a reboot/power loss mid-upload aren't swept at boot (deletable from the explorer).
- SEC-L2: the slot flags are `volatile`, not `std::atomic`/portMUX — fine on these chips in practice.
- SEC-L5: no free-space floor; uploads into `/logs/` count toward the 512 MB cap. SEC-L7: fuzz the log readers
  with a non-NDJSON file planted in `/logs/`.
- REV-011: a 0-byte download also shows download.js's "Nothing to download — no rows in that range" toast.
- REV-012: a listing chunk that happens to start with `SIZE=`/`SESS=`/`WL=` (a file named so, on a 180-B
  boundary) would confuse download.js until its 20 s watchdog — very unlikely.
- REV-014: lowercase `z`/`u` (and Z/U sent to pre-0.18 firmware) fall to phone.cpp's final `else` → `g_download=0`.
- `enforceLogCap` still `break`s on a failed remove (a too-long name put on the card by a PC could still wedge it).
- CYD file dates show blank (its system clock is never set; FAT stamps read 1980); mini dates are real.
- Upload speed untested (estimate ~1–5 KB/s: one ack round trip per 240-B chunk).
- Nothing tested on hardware yet.

## Earlier — 2026-10-28: status-LED brightness setting, fw 0.17.0 (DONE; mini #2 FLASHED, pushed `80bf283` + `cac0de7`)
The mini #2 RGB LED was glaring at night, so its overall brightness is now a web-app setting.
- **CMD `I:<0-100>`** (`phone.cpp` → `ledBrightnessRequested()`; `'L'` was taken by the log download) → core
  `servicePhoneCommands` calls the new **`hostboard::setLedBrightness(pct)`** (board clamps 0–100, re-applies the current
  colour, saves NVS **`cydui/ledbri`**; skips a no-op write) then `pushStatus()`.
- **STATUS part 2 `led=<pct>`** from the new `hostboard::ledBrightness()`; **−1 = no dimmable LED** (CYD, mini #1 /
  plain `xiao_s3`), which keeps the web card hidden.
- `src/xiao_s3/host_board.cpp`: `writeRgb()` scales `kLedDuty × kLedLevel × s_ledPct`; LEDC moved **8 → 12-bit** so the
  dim G/B channels keep their colour balance when scaled (a lit channel never rounds to 0). **Default `kLedDefaultPct` = 5**
  (first shipped as 25 in `80bf283`; the owner found 25 still too bright in the dark, 5 still visible in daylight →
  `cac0de7`). The boot R→G→B self-test runs at the saved level. A value already in NVS wins over the default.
- Web app: Settings **"Status LED"** card (`#ledCard`, slider `#ledSlider` 0–100 step 5), shown only when STATUS
  `led ≥ 0` (`ble/status.js`); the `I:` send lives in **`main.js`** (entry-module rule, so a stale cached `commands.js`
  can't break the import). Token `?v=20261028a`, `#uiver` = `led-bri` (Pages confirmed live). The 5% default change only
  touched the slider's placeholder value, so the token was not re-bumped.
- `FW_VERSION` 0.17.0; CLAUDE.md CMD list + STATUS keys updated. Builds: cyd, xiao_s3, xiao_s3_rgb all green.
- **Device state:** mini #2's S3 (`10:b4:1d:eb:96:04`, COM10 this session) runs `cac0de7` `xiao_s3_rgb`, boot-verified
  (link up, scanning). **Owner confirmed the slider works.** CYD NOT reflashed (its change is only `led=-1` + stubs).
- The untracked `hardware/diagrams/mini-build_led_wiring_diagram.svg` was **deleted** (owner, 2026-10-31): the LED stays
  as built; `hardware/PINOUT.md` §6 is the reference (100 Ω blue fitted).

## Earlier — 2026-10-27: MINI #2 built + `[env:xiao_s3_rgb]` RGB status LED (DONE; committed + pushed `4fddba0`)
The owner built a **second mini** ("mini #2", NOT the unit used on the 10-24 drives): new perfboard/case
footprint, **2000 mAh** cell, **power LED removed** (redundant), and a **5 mm RGB status LED** on the S3. Wiring diagram:
the owner's Cirkit export (deleted 2026-10-31; it said 68 Ω for blue but **100 Ω is
fitted**).

**Hardware (mini #2):** S3 `10:b4:1d:eb:96:04` (BLE `CFLO-MINI-BEMOPU`), C5 `10:bd:a3:cf:a3:c8`. The USB serial number = the
MAC, so ports are identifiable without esptool. LED: R → D0/GPIO1 via 470 Ω, G → D2/GPIO3 via 100 Ω, B → D3/GPIO4 via
100 Ω, common → GND.

**Firmware (built, NOT committed):**
- New env **`[env:xiao_s3_rgb]`** = `extends env:xiao_s3` + `-D MINI_RGB_LED=1`. Mini #1 keeps plain `xiao_s3` (no LED,
  unchanged). Flash mini #2's S3 with `xiao_s3_rgb` only.
- `src/xiao_s3/pins.h`: `LED_R/G/B_PIN` 1/3/4 under `#ifdef MINI_RGB_LED`. `src/xiao_s3/host_board.cpp`: `writeRgb()`,
  LEDC PWM 5 kHz/8-bit, `kLedDuty` = {255,40,15} (tuned by eye, see below), **active HIGH (common-cathode)**, R→G→B self-test at boot.
  `hostboard::setScanLed()` → same semantics as the CYD via host_core `applyScanLed()` (green link up / red down / red
  blink alert / magenta follower). `src/xiao_s3/main.cpp`: also sets the link LED after the boot ping and on the
  idle tick (no-op on plain xiao_s3).
- Docs updated: PINOUT.md §6a (LED rows + note), mini-build.md (wiring-table rows, mini #2 / 2000 mAh note).
- Builds: xiao_s3_rgb 41.7% flash / 30.8% RAM; xiao_s3 + xiao_c5 still green.

**Bench results (2026-10-27):** C5 flashed `xiao_c5`, S3 flashed `xiao_s3_rgb`. Link up, 2.4/5G/BLE/PRB streaming,
RTC present (time not set yet), GPS NMEA clean (no fix indoors), bat ~3.9–4.0 V. The first SD mount failed with "no valid
FAT volume" (fresh card, likely exFAT); on a later boot it mounted (`sd=ok`, sessions logging), so the owner presumably
reformatted it to FAT32.

**LED problem:** the LED stayed **dark**. A pad test proved D0/D2/D3 drive correctly (read-back follows the drive). A 7-step
pattern (each pin high alone, all high, each pin low alone) stayed **dark throughout**. Every leg arrangement of a working
CC LED would light on at least one step, so the cause is (a) a **common-ANODE part** with its common on GND (owner thinks
this is likely; they own both types), (b) an open common/GND joint, or (c) a dead LED. **Owner is replacing it with a
common-cathode part.**

**Resolved:** the owner swapped in a **common-cathode** LED (the first one was very likely common-anode). The 7-step test
then showed every colour correctly (pin mapping R/G/B = D0/D2/D3 confirmed). At full duty magenta looked purple and yellow
looked green: red (470 Ω) is far weaker than G/B. A PWM tuning test picked **B=15** for true magenta and **G=40** for true
yellow → `kLedDuty` {255,40,15}. (Pure blue is only used in the boot self-test, so it's dim there; that's expected.)

**Threat-category LED colours (added same day, shared core → CYD too on its next flash; CYD NOT flashed):**
`hostboard::setScanLed(bool,bool,bool)` → `setScanLed(LedColor)` (`enum class LedColor` in host_board.h; CYD maps to its on/off
LED, Amber → yellow). host_core keeps a two-colour pattern: `applyScanLed()` picks it per cycle, new `tickScanLed()` (called at
the top of both loops) alternates every `LED_ALT_MS` = **700 ms** (owner's choice), `showLinkLed()` resets to the link colour on
scan stop/idle (replaced the CYD's 4 direct `setLed(link)` calls + host_phone_cmds). Priority: **Axon = red/blue alternating** >
**Flock/ALPR = amber** > **Ring/Cam/Glass = cyan** > follower magenta > link green/red; a deauth/evil-twin alert alternates red with
that base (now at 700 ms, was once per ~2 s cycle). Categories = `countCategories()` (non-whitelisted, ≥ Suspect, same as the HOME
tiles), only while the link is up. Mini levels: `kLedLevel[]` in xiao_s3/host_board.cpp (Amber = R255 + 100/255 of the green duty).
Owner approved the colours on a temporary demo build (removed). All of cyd / xiao_s3 / xiao_s3_rgb build.

**Current device state:** mini #2's S3 runs the real **`xiao_s3_rgb`** with the tuned duties: link up, SD logging,
scanning; LED solid green. The C5 runs `xiao_c5` (v11).

**Next:**
1. Owner eyeballs the live states: boot R→G→B, solid green (link up), red when the C5 is unpowered. Magenta (follower)
   only shows if `FOLLOW_ALERTS_ENABLED` is flipped on; red blink needs a deauth/evil-twin event.
2. ~~Commit~~ — done (`4fddba0`, pushed 2026-10-28). The diagram SVG is still untracked (see Latest).
- Power rule reminder: **only one XIAO USB at a time**, switch ON.

## Earlier — 2026-10-24: position/time source handling fw 0.12 → 0.16 + webapp polish (ALL DONE, pushed)
**State at end of session:** CYD (COM14) + mini S3 (COM16) both run **fw 0.16.2** (items 9–10); both C5s unchanged (link v11, no
reflash needed all day). Webapp `#uiver` = `gps-sent`, `style.css?v=` + `main.js?v=` = `20261024e`. Bridge is
current (scan-analyzer ACKed `fix_age` on file + live rows; their server sides are built, commits pending the owner).
**Open / next:**
- Mini **DS3231 not detected** ("RTC not found" at boot with the switch on) — check its I2C / power wiring. Without it
  the purple clock dot depends on the GPS alone and the mini has no clock after a reboot until GPS/phone sync.
- Owner phone-check of: the source dots + Settings "Time & location sources" (incl. Prefer device / Device only),
  the Home summary card + modal (needs the `js-ver` page; open `…/webapp/?r=1` to bypass the cache once).
- A drive where Chrome keeps posting with the screen off (like drive 1) would exercise live rows on the onboard GPS.
- Still open from before: webapp Explore/Map don't surface `fix_age`; async BLE all-logs export; CYD field test with
  SW2 on. (The reviewer pass on today's firmware is DONE, see item 9.)

### Items (chronological)
1. ~~Bump `FW_VERSION` `"0.11.0"` → `"0.12.0"`~~ — **DONE 2026-10-24** ([host_log.h:85](src/host_common/host_log.h#L85)).
   Flashed **CYD** (COM14: boot OK, `link=up`, detections flowing; its C5 not reflashed — FW_VERSION is host-only and
   the C5 was already v11) and **mini S3** (COM16, identified by esptool MAC `10:b4:1d:e7:ed:a0` = scanner_id
   `a0ede71db410`; boots OK, `CFLO-MINI-GABAME`; `link=down`/`RTC not found` only because the xiao_c5 was unplugged,
   switch off and battery flat — expected per owner). xiao_c5 needs no reflash (already v11).
2. ~~Owner uploads an SD log from the reflashed CYD~~ — not needed: both 2026-10-24 mini drives (sessions 29, 34) carry
   v11 `bssid`/`data`/`dl`; verified (every `data` row has a `bssid`, none elsewhere; most APs also logged as AP rows).
3. ~~Reply on the bridge~~ — done (fw bumps + v11 verification posted). Their server side is built (schema v9) but not committed yet,
   pending the owner's OK.
4. **Phone pocketed → GPS fallback — BUILT 2026-10-24, fw 0.13.0 (committed + pushed after scan-analyzer ACKed
   `fix_age` on the bridge; mini S3 + CYD flashed, CYD link + scanning verified).** Bug: phone GPS never expired on the host (`g_haveGps` set once, never cleared, not even on
   disconnect), so a pocketed phone (screen-off Chrome stops `watchPosition`) pinned every later row to its last fix
   and the onboard NEO-6M never took over. Time was always fine (phone epoch free-runs on `millis()`).
   Fix (owner-approved; owner chose "hold the last fix when no GPS, and mark it"):
   - `phone::hasGps()` = connected AND written within `PHONE_GPS_STALE_MS` 90 s; `hadGps()`/`gpsAtMs()` added.
     Onboard is live only while a Status carried a fix within 10 s (`g_onbAtMs`; a dropped link no longer leaves it live).
   - Order: live phone > live onboard > HELD most-recent fix of either (`posSource()`=3). `posLive()` +
     `posFixAgeS()` in host_core.
   - **Log: `"fix_age":<s>` right after `lon` on EVERY line carrying lat/lon, only when held** (new `appendPos()`
     helper in host_log.cpp replaced 6 copies). Absent = live.
   - Follow scoring uses `posLive()` only. STATUS `gps=` = live only; `gpssrc=3` = held. CYD: orange GPS pin,
     modal "Last known · No live fix - held Ns". Webapp: icon tooltip + status-info text for gpssrc 3; `#uiver`
     = `gps-held` (no CSS change).
   - scan-analyzer ACK (bridge 2026-10-24): name/shape fine; keep it absent for a live fix (never `"fix_age":0` for
     live) — already the case. Their column + down-weighting of held rows is planned, not built.
   - **Field-checked (mini drive, scan-analyzer session 29, log in the bridge folder):** phone fixes every ~6 s,
     screen off at ~13:38:34, position FROZEN 86–90 s with no marker, then onboard every 2 s; 0 `fix_age` rows. The
     phone did stop sending; the 90 s phone window was too long → fixed in 0.14.0 below.
5. **fw 0.14.0 — 10 s phone preference + web source dots (2026-10-24, CYD FLASHED COM14; mini S3 NOT yet — unplugged).**
   - `posSource()`: phone if `phone::hasGps()` (connected, < 90 s) AND (its fix < 10 s old `PHONE_FIX_FRESH_MS`, or
     onboard not live); else live onboard; else held (3). `posAged()` = held OR fix in use > 10 s → log `fix_age`
     (so a phone-only walk between sparse 30 s keep-alives gets `fix_age` 10–30). Follow scoring: fixes ≤ 90 s old.
     Today's drive would have handed over ~10 s after screen-off instead of 90 s.
   - STATUS part 2 gains **`gav`/`tav`** (available sources, bit0 phone / bit1 device). Part 1 worst case was already
     ~252 B (the notify limit), hence part 2. `timeAvailMask()`: device = GPS clock or RTC latched.
   - **Webapp app bar (owner request):** clock + location icons each get two stacked dots (blue phone on top, purple
     device below, grey = unavailable) and the icon takes the colour of the source in use; orange GPS icon = held.
     Older firmware without gav/tav: availability inferred from the source in use. Colours `$color-src-phone`
     `#388bfd` / `$color-src-device` `#a371f7` / `$color-src-off`; old cyan `.onboard` rules removed.
     `style.css?v=20261024a`, `#uiver` = `src-dots`. Not yet seen on a phone.
   - **Pending:** flash the mini S3 (0.13.0 → 0.14.0); owner phone check of the dots; next drive should show
     `fix_age` only on stretches with no onboard fix. Webapp Explore/Map don't surface `fix_age` yet.
6. **fw 0.15.0 — time/GPS source preferences + device-clock dot rule (2026-10-24, CYD FLASHED COM14 + boot OK; mini S3 FLASHED COM16 (link up; boot says "RTC not found" — check the DS3231 wiring);
   ).** Owner requests:
   - **Purple clock dot** = "on-device fallback ready": onboard GPS supplying time now (gpsEpoch within 10 s) OR
     the RTC holds a valid time. A free-running latched GPS clock alone reads grey (still used as last resort).
   - **Settings → "Time & location sources"**: two selects (Prefer phone / Prefer device / Device only) → CMD
     `P:<t><g>`, NVS `cydui/tpref`,`gpref` (loaded in `loadSrcMask()`), echoed in STATUS part 2 `tpref`/`gpref`;
     selects enabled only once STATUS reports them (older fw never gets `P:`). host_core: `timeSource()` /
     `posSource()` branch on the pref; device-only also skips the phone in the held fallback + RTC discipline and
     greys the phone dot (`phoneGpsOk()`/`phoneTimeOk()`). `#uiver` = `src-prefs` (no CSS change).
   - Not yet phone-checked; not reviewed by an agent.
   - **Webapp GPS cadence fix (same day):** owner saw the GPS icon flap phone/device every ~30 s while standing
     still on "Prefer phone". Cause: the webapp sent a fix only after ~10 m of movement, else a 30 s keep-alive,
     but 0.14.0 treats a phone fix > 10 s old as stale. `js/gps.js` now sends every real `watchPosition` update
     at most every 5 s, moving or not, skips any position the phone reports as > 10 s old, and no longer logs each
     send. Fresher positions, not hidden markers: rows only lose `fix_age` because the fix is actually current.
     `#uiver` = `gps-5s`.
7. **fw 0.16.0 — live-POST rows use the SCANNER's position (2026-10-24, CYD + mini S3 FLASHED, links up).**
   scan-analyzer found the mini drive's live rows frozen 13:38:39→13:49:21 (~10.7 min, 84 rows, no marker): the
   webapp stamped live rows with the phone's last fix (`state.lastGps`), and screen-off stopped geolocation but not
   the page (BLE + POSTs kept going). Fix: STATUS part 2 gains `pos=lat,lon` + `pfa=<fix_age|-1>` (what the file log
   stamps now); `scannerFix()` in `js/gps.js` → live rows (`remote.js`, + `fix_age` when ≥ 0, owner-approved
   contract addition, bridge note posted) and the sighting trail (`sightings.js`, live fixes only). Older fw: phone
   fix only while < 10 s old. No C5 change. `#uiver` = `live-pos`.
   **Field-tested (mini drive 2, sessions 33 live / 34 file):** file log phone ~6 s → onboard 2 s (15:30–15:32,
   screen off) → phone again, no freeze > 15 s, 0 `fix_age` (onboard was live). Live rows match the file (median 0 m,
   max 14 m). This time Chrome sent NO live rows while the screen was off (15:29:49–15:32:41), so the live-row
   onboard path wasn't exercised; the file log covers that stretch.
8. **Webapp polish (2026-10-24, webapp-only).**
   - `692cef1` COINTEL-FLO wordmark 1.3rem/.2em → **1.2rem/.1em** (margin-right −.1em): the source dots + sat count
     had wrapped the title to two lines.
   - `b0d6610` Home summary card: **unit name moved from the app bar into the card** (same `#unitName` id); second
     row labels **PRB · UNIQ · SUSP · LKLY · CONF** (`white-space:nowrap`); **tap the card → `#countsDlg`** "Scan
     summary", every count spelled out (`features/counts.js`, live-refreshed from `ble/status.js`, disabled sources
     read "off"). Verified in headless Chromium over CDP (real pointer click → dialog opens, 9 rows, no errors).
   - `e5bc851` Owner's phone showed the new page but the card did nothing: it had the new `index.html` with a cached
     OLD `main.js` (no handler; closing the tab doesn't clear the HTTP cache). **`js/main.js?v=<token>` now carries
     the same token as `style.css`, bumped every deploy** (see the cache section below).
   - `54ea88b` removed the outdated `mini-build_wiring_diagram.png` (showed the 3V3 C5 feed); mini-build.md note updated.
9. **fw 0.16.1 — reviewer-pass fixes on 0.13–0.16 (2026-10-24, CYD + mini S3 FLASHED, links up; webapp `gps-sent`).**
   - **Race: `fix_age` ≈ 4294967.** A phone GPS write (NimBLE task, core 0) landing between `millis()` and the
     `g_gpsMs` read in loop() wrapped the age negative. Phone fix now lives under `g_gpsMux`; `phone::gpsFix()`
     returns lat/lon/stamp together, `gpsAgeMs()` reads the stamp first and clamps; `hasGps()` uses it.
   - **Mixed lat/lon/fix_age.** Every `posLat()`/`posLon()`/`posAged()` call re-chose the source. New
     `PosFix posSnapshot()` (host_core.h) decides once; `appendPos`, STATUS `pos`/`pfa`, follow scoring and the
     CYD GPS modal each take ONE snapshot. `posLat`/`posLon`/`posAged` removed; the single-field wrappers remain.
   - `fix_age` threshold compared in ms (`FIX_AGE_MARK_MS`), so 10.0–10.99 s fixes are marked too.
   - **Time ranking (owner choice):** device clocks rank fresh GPS > RTC > latched GPS under every pref; prefer-phone
     puts the phone first, prefer-device puts it LAST (no GPS↔phone clock flapping on a 10 s Status gap). RTC
     discipline in prefer-device takes fresh GPS first. Quirk: prefer-device with only a stale latched GPS shows the
     device as `tsrc` while the purple dot is grey (dot = "fresh fallback ready").
   - `P:` must be exactly 4 chars, both digits packed in one byte (no torn pair), NVS written only on change.
   - Webapp `gps.js`: `lastGps` advances only after a successful BLE write (5 s throttle kept on attempts); the
     first skipped stale fix is logged (a skewed phone clock would otherwise drop every update silently).
   - Checked, no change: rows with `epoch:0` after a timesync anchor are backfilled by both `parse.js` and
     `logfilter` (device-only with no device clock is safe). Ignored: 49.7-day `millis()` wrap.
10. **fw 0.16.2 — CYD GPS modal refreshes live (2026-10-24, CYD + mini S3 FLASHED).** Owner: the modal was drawn
   once, so connecting the phone with it open still read "No fix" until closed + reopened. `serviceGpsModal()` now
   re-checks ~2×/s and repaints only the body when source / lat-lon / sats / held age changed (`GpsModalView`).
   On the live Scan screen the scan cycle now keeps running under the modal (it used to pause, which would have let
   the onboard fix go stale and flip the live modal to "Last known"); `onScanCycle()` skips the list repaint while
   it's up and keeps the status bar live. Not yet re-checked on-device by the owner.

## Earlier — Wi-Fi AP/CLIENT role pill + data-frame client flag + AP BSSID, link **v11** (2026-10-23, ALL FOUR BOARDS FLASHED, pushed `c4af4d6`)
Requested by scan-analyzer over the bridge, approved by the owner. Schema agreed with scan-analyzer before commit.
Webapp `#uiver` = `wifi-role+data`, `style.css?v=20261023a`. `FW_VERSION` NOT bumped (still 0.11.0).
- **Webapp role pill:** `wifiRoles(tags)` in `js/util/format.js` (2.4/5G → `AP`, PRB → `CLIENT`, both when a MAC
  is heard both ways). Outlined `.sp.prole` pill after the band pills on live + Explore rows (`renderDetList`);
  "Wi-Fi role" line in both detail modals (`openDetail`, `openLiveDetail`); PRB friendly name is now "Wi-Fi client
  (probe / data frame)". No CYD UI change.
- **Link v10 → v11:** `Detection.bssid[6]` appended (`sizeof(Detection)` 75 → **81**) + `FLAG_WIFI_DATA_CLIENT`
  (bit 8) / `FLAG_WIFI_DATA_UPLINK` (bit 9). C5 `handleData` records the AP (addr1 on ToDS uplink, addr2 on
  FromDS downlink); `mergeDetection` keeps a prior bssid when a later probe from the same MAC has none (flags were
  already sticky).
- **Log (PRB rows):** `flags` gains `data`, plus `dl` when no uplink frame was seen (the RSSI is then the AP's);
  `"bssid":"AA:BB:…"` sits right after `mac` whenever `data` is set. Host: a no-label, no-tier `kFlagRules`
  entry + the `dl` rule in `flagTokens()`; new `FLAGS_DATA` (host_core.h) is carried in `DevGroup.flags`, so the
  DETS `flags` field and live-POST see 256/512, but it is NOT in `FLAGS_NOTABLE` (no tier floor, no follow-guard
  exemption). Live-POST `flagStr()` emits `data`/`dl`; NO bssid in live rows (reduced shape). Explore detail
  shows "Connected to AP" (`parse.js` reads `bssid`). `lastseen` lines unchanged.
- **Flashed + checked:** CYD (COM14) + its C5 (COM18): link healthy, C5 console `dataClient=2`/`1` per window. Mini
  xiao_c5 (COM23) + xiao_s3 (COM16; confirmed by console, not the ODID bench S3): link healthy on v11.
- **Not yet verified:** an actual SD row carrying `bssid`/`data` (download a session, search for `"bssid"`). No
  reviewer pass run on the firmware diff.
- **Also landed just before (`382ad34`, firmware):** STATUS part 1 never reached the phone. A bare
  `NimBLECharacteristic::notify()` only schedules a send and reads the value at transmit time, so setting part 1
  then part 2 back to back sent part 2 twice (grey status icons, "Session: -", Start shown while scanning).
  `pushStatus()` now sends each part with `notify(data, len)`.
- **Recurrence-level rename (scan-analyzer side only, coordinated over the bridge):** their per-device recurrence
  levels reused threat-tier words. The owner asked for distinct systems, and we kept OUR threat tiers
  (suspect/likely/confirmed, in the log contract). scan-analyzer is renaming: `single-day`→`one-day`,
  `possible`→`occasional`, `likely`→`frequent`, `confirmed`→`resident`, `mobile`→`roaming`, `no-fix` stays.
  These levels are internal to their DB/API/UI, so there is no change in cyd-scanner.
- **Next:** check a real log row; consider showing the bssid / "joined" state on the CYD detail screen + webapp
  CLIENT pill (e.g. `CLIENT·joined`); still open from before: mini battery sense, async BLE all-logs export, CYD
  field test with SW2 on.

## Earlier — one scan = one file + never-stop logging; all-logs range export; grouped session picker; unit names; lastseen lines; fw 0.11.0 (2026-10-22, BOTH HOSTS FLASHED, all pushed)
Commits `5308308` (logging + export + picker), `ac1ffd8` (unit names), `777ae4f` (file-name display + range-export
naming), `926c197` (`FW_VERSION` 0.9.0 → **0.10.0**). Flashed: **CYD** (COM14, `CFLO-CYD-TOLUBA`) and **mini S3**
(COM16, `CFLO-MINI-GABAME`). C5 firmware unchanged (no protocol bump). Webapp live (`#uiver` = `range-names`,
`style.css?v=20261022c`), owner phone-checked it ("looks good").
- **Trigger:** an owner field drive produced **13 session files** (~72 KB each). Root cause: the per-stream dedup set
  (`MAX_SEEN` 400) filled in ~1–2 min of driving → `logNewDetections()` SKIPPED new MACs until the 5-min
  `SESS_ROTATE_MIN_MS` guard let it rotate (real data gaps), and the bg stream rotated at `MAX_SEEN-32`.
- **Fix — LRU seen table, no rotation:** `SeenKey` (now packed, 17 B) gained `lastSeen`/`lastLogged`; when full the
  least-recently-SEEN entry is evicted (`seenInsert`), so logging never stops; both rotations + `lastRotateMs` /
  `SESS_ROTATE_MIN_MS` removed. A file changes only on boot / New Session. **Re-log every `RELOG_MS` = 5 min**
  while a device stays (or returns) in range; `lastLogged` is stamped only after a successful write.
  `uniqCount`/`rowCount` replace `seenCount` (STATUS `uniq`, CYD `U:`). The live view was never capped (C5 table
  evicts its stalest entry); only logging was.
- **All-logs time-range export:** `T:<mode>[:min]:*` (BLE) / `/dlf?f=all` (Wi-Fi) → `logfilter::MultiExport`
  joins the window from EVERY `/logs/*.jsonl` (cheap 1 KB head/tail reads pick files; per-file timesync backfill
  REWRITES `epoch:0` rows; one fresh meta line; cap 64 files). `thisBootFiles()` = live fg/bg + a ring of 8
  files ended this boot (they get a timesync anchor on sync too). Torn-last-line-safe tail epochs; a short BLE send
  is newline-padded to `SIZE=`. Exports are named **`YYYYMMDD-HHMMSS-YYYYMMDD-HHMMSS.jsonl`** from the first/last
  dated row (owner wants name-sortable files; the on-card log names are UNCHANGED).
- **Session list** lines gained `\tfirstEpoch\tlastEpoch`. **Webapp Session logs** = checkbox multi-select of
  SCANS (`features/sessions.js` groups same-stream files ≤ 6 min apart); rows show the full file name(s), then the
  human date span, then kind/parts/size. Download/Map/range join parts (`ble/transfer.js` `bleFetch` promise queue +
  `parse.js` `joinLogParts`); **Upload = one `/api/v1/uploads` body per scan**; Delete queues `D:`. Explore picker
  lists the same groups and collapses re-logged rows to one device with a sighting trail. 20 s idle watchdog for
  stalled BLE transfers. CYD on-device Explore now indexes one entry per device (up to 512 devices).
- **Unit names:** BLE name `CFLO-<CYD|MINI>-<WORD>` (`scannerName()`, 3 CV syllables from an FNV hash of the eFuse
  MAC; stable across reflash/NVS wipe) — also STATUS `name=`, meta `scanner_name`, the CYD Phone Link QR screen and
  the web app bar. `hostboard::boardTag()` added.
- **Reviewer pass** done (REV-001…012); fixes applied. **Open:** the BLE all-logs export blocks `loop()` while it
  streams (~12 KB/s → no logging for minutes on a big range; use Wi-Fi) — async export backlogged.
- **scan-analyzer (bridge 2026-10-22):** no server change needed; `fw 0.10.0` marks relog-era sessions.
- **Next:** field-test the CYD (SW2 on → C5 powered) — one file per scan, `uniq` climbs past 400, "Past 24 h"
  spans every file; owner uploads the old 13-part drive as one scan. Old 0.9.0 files on the cards still show
  grouped in the picker. Mini already walk-tested (below).

### Same day, later — `lastseen` departure lines (fw 0.11.0), join-meta fix, dedup toast (BOTH HOSTS FLASHED, pushed)
Commits `cc01e93` (lastseen + `FW_VERSION` **0.11.0**), `15f5e2e` (join-meta fix), `763d6ea` (dedup toast).
Webapp `#uiver` = `dedup-toast` (no `style.css` change since `?v=20261022c`). Both units flashed + boot-checked
(CYD reported `link=up` — C5 powered).
- **`{"evt":"lastseen"}` departure line** (schema owner-approved directly to this session, spec from scan-analyzer's
  bridge entry): `src,mac,[pan],first_ms,last_ms,rssi_max` per device presence period, ALL device types; written
  after `DEPART_MS` 60 s unheard (both fg + bg streams swept each cycle), on LRU eviction of a present entry
  (bounded `g_evicted[16]` queue, flushed after the cycle's file closes), and flushed for every present entry on
  New Session (`beginSessionNamed` → `writeDepartures(st, …, true)` into the OUTGOING file). File log only (not
  DETS/live-POST). `SeenKey` = 24 B packed (+`firstSeen`/`rssiMax`/`srcType`/`present`). **The seen tables no
  longer fit the CYD's static DRAM (ld overflow by 7.4 KB) → now heap-allocated (`calloc`) on the first
  `beginSessionNamed`**; a failed alloc disables logging for that stream rather than spamming. Web Map skips the
  line (`parseScanEvents`). Documented in CLAUDE.md.
- **Field test (owner walk, mini):** ingested fine — 1723 lastseen lines, **35 chains / 77 rotating addresses
  linked** (6/12 without). Handoff gaps cluster at −33…−8 s = the C5 30 s TTL inflation (scan-analyzer's 35 s overlap
  allowance is right).
- **Bug found + fixed (webapp):** that joined upload (`bg-20261022-144217+8p.jsonl`) carried meta `fw 0.9.0`, no
  `scanner_name`: `joinLogParts` kept only the FIRST part's meta, and the first part predated the mini's reflash
  (groups can span reflashes). Now each DISTINCT meta line is kept in part order (server: last meta wins).
  scan-analyzer patched session 22 server-side.
- **Server dedup is now ROW-level** (scan-analyzer, owner request): re-joined/regrouped re-uploads store only new
  rows; all-duplicate → `duplicate:true`, `session_id:null`, counts 0. So **re-uploads are safe** (files with a
  `scanner_id` meta). The upload toast now says "Already on the server — nothing new in X" or "N new sighting(s),
  M new event(s); K row(s) already on the server" (K = rows sent − stored, a client-side estimate).
- **Owner-side, parked:** NordVPN on the server PC breaks the phone→PC Tailscale path ("Failed to fetch" on upload).

### Same day, last — STATUS split fix + webapp polish (BOTH HOSTS FLASHED; webapp `#uiver` = `map-credit`, `style.css?v=20261022h`)
- **STATUS truncation fix (`c25a81b`, firmware, no fw bump):** STATUS had grown to ~273 B with `name=`, past the
  BLE notify limit (MTU−3 ≈ 252 B; the web app only gets STATUS by notify, never a long read), so the tail was
  silently cut — the phone never learned `name=` (live-POST meta had no `scanner_name`), and the Wi-Fi AP creds
  (appended while a download AP is up) were likely cut too (AP-QR path NOT re-tested). `pushStatus()` now sends
  TWO notifications: part 1 live counts/state (~215 B), part 2 `dl;sid;fw;dbgen;name[;apssid;appsk;apurl]`
  (`dl` + creds stay together — `status.js` reads them from one notification). One-shot serial WARNING if a part
  passes 240 B. Web app unchanged (it merges keys per notification). CLAUDE.md STATUS line documents it.
- **13-part drive uploaded** as one scan (`20261022-111401+13p.jsonl`, 5200 sightings). A first attempt timed out
  fetching part 1; a console capture showed the `F:` never reached the mini (stale GATT link right after the
  reflash). The retry streamed every part with `SIZE=` == bytes sent. Not reproduced; if it recurs, add a single
  auto-retry in `bleFetch` when nothing at all arrives.
- **Webapp polish (owner, several relayed by scan-analyzer and confirmed with the owner):**
  - "Refresh session list" feedback (`8303c7c`): button → "Refreshing…" + disabled, list placeholder, working
    toast until `SESS=` lands (30 s timeout → error toast). The list is slower now that the device reads every
    file's head/tail for its span — cache finished files' spans on the device if it gets too slow.
  - COINTEL-FLO wordmark (`24d1958`, `bfc2ea7`): Oswald ExtraLight 200 from Google Fonts, `letter-spacing:.2em`,
    1.3rem, title only (`.wordmark`) — scan-analyzer's map + admin pages use the same 0.2em.
  - Scanner threat strip → **Threats card** (`bd104bb`): header + chevron, one labelled `CAT_COL` pill per ACTIVE
    category (most first, wrapping), "No active threats" when none; tap → the full breakdown modal. Owner
    approved ("looks okay"); the first view showed the old dots only because of the Pages cache.
  - Grayscale basemap (`339cdb7`): `className:"basemap"` on the tile layer in the shared `newMap()` +
    `.basemap{filter:grayscale(1) contrast(.85) brightness(1.08)}` — tiles only, so markers keep their colours.
  - Minimal attribution (`991f5cc`): `attributionControl.setPrefix(false)`; the required "© OpenStreetMap
    contributors" credit is kept (linked to osm.org/copyright), small and faint (`_map.scss`).
- **Open (unchanged):** ~~mini battery sense reads wrong~~ RESOLVED 2026-10-23 (moved to D1, see RESUME HERE); BLE all-logs export
  blocks the loop (async backlogged); field-test the CYD with SW2 on.

## Earlier — web C5-link icon fix + status-icon info modal; mini-build cell now 1000 mAh (2026-10-21 late, webapp-only, pushed)
Commits: `4f2b2f5` (modal + link fix + last session's S3 battery-sense fw + redrawn mini-build diagrams), `2d51a7d`
(owner's link colours), `cd591d1` (green background removed). No firmware flashed this session (S3 already ran the
battery-sense build; cyd + xiao_s3 both re-built green before committing). **Not yet browser-tested** — live on
Pages once the footer `#uiver` reads `link-nobg` (`style.css?v=20261021c`).
- **C5 link icon showed "down" while scanning worked: a WEB-APP bug, not the link.** The S3 console showed every
  cycle succeeding (`g_linkOk` true → STATUS `link=1`). `status.js` set `el.style.color` on the outer `#linkState`
  span, but `.si .material-symbols-outlined` gives the inner glyph its own muted colour, so it stayed grey.
  **Fix:** classes styled on BOTH spans (`_base.scss`), cleared on disconnect.
- **Owner-chosen link colours (icon colour only, NO background):** gray = down (the `.si` default), white = up / not
  scanning (`.linkup`), light blue `$color-scanning` #79c0ff = up + scanning (`.linkscan`). Computed from the merged
  `state.status` `link` + `scan` keys. **Gotcha:** don't name these classes `.up`/`.down`/`.wait` — `_base.scss` has
  generic pill rules (`.up { background:$color-pill-up }`) that painted a green box behind the icon (fixed in `cd591d1`).
- **New: tap any app-bar status icon → "Status icons" modal** (`features/statusinfo.js`, `#statusDlg`, wired in
  `main.js` via a click on `#appbarStatus`): one row per icon (clock, GPS, DB, C5 link, battery, Bluetooth) with a
  clone of the live icon, its name, current state (toned text: ok green / warn amber / bad red; the link row uses the
  icon's own gray/white/light-blue), and a one-line description. Tapped row highlighted; re-renders live on each
  STATUS notify + BT state change (MutationObserver on `#conn`); Connect button when disconnected. STATUS keys merge
  into `state.status` (cleared on disconnect). Clock title now names `tsrc=3` (RTC). New SCSS tokens
  `$color-ok`/`$color-bad`/`$color-scanning`. CYD on-screen status bar unchanged (it already has a GPS modal).
- **Cell swap:** the mini build's LP103450 2000 mAh LiPo → a **1000 mAh** LiPo (case fit, model not recorded). Noted in
  [docs/mini-build.md](docs/mini-build.md) charging section: runtime ≈ halves; the TP4056's 1 A default is now 1C →
  consider `Rprog` ≈ 2.4 kΩ (0.5 A) unless the cell's datasheet allows 1C.
- **Wiring diagrams** `hardware/diagrams/mini-build_wiring_diagram{.png,.svg,_labeled.png}` are now COMMITTED
  (owner's 10-21 redraw; supersedes the "stale, don't commit" notes below).
- **Next:** owner checks the web app on the phone (icon colours + modal); battery sense read wrong (RESOLVED 2026-10-23, moved to D1; open item
  below — console `[S3] bat 344–358 mV (-1%)` on 10-21 evening; meter the D3 pad next).

## RESUME HERE — mini build bring-up COMPLETE (link + RTC + GPS all PASS on battery); S3 back on `xiao_s3` (2026-10-21)
The owner finished the §2.6 rewire (C5 fed from its BAT pads, no S3 3V3 → C5 3V3 wire).
- **UPDATE (later 10-21): LINK PASS on battery.** Cell fitted; with the switch in circuit the probe still showed the C5
  brownout-looping (identical to no-cell) → the **switch was faulty**. Switch removed, both XIAOs wired straight to
  TP4056 OUT±: `xiao_s3_bringup` reports LINK PASS (0 miss, 134/134 bursts, C5 up 1449 s, resets 0). RTC still
  `i2c: none` (open item). GPS: Status arrives, no fix yet (indoors). S3 currently runs `xiao_s3_bringup` → reflash
  `xiao_s3` when done. **Switch since replaced + re-fitted by the owner (DONE, 10-21).**
- **UPDATE (10-21 evening): RTC FIXED.** Owner swapped in a NEW DS3231 module (old one presumed faulty; all pins
  had continuity). Bring-up now shows `[RTC ] PASS | i2c: 0x68` (no 0x57 EEPROM on this module, fine), temp 30.5 C.
  Clock set via `s <epoch>` -> `2026-10-21 16:56:51 UTC osf=0`. Link still PASS. **Next: persistence test** (cut all
  power ~30 s, re-check time + `osf=0`), then `x` stress with the RTC on the rail, then Stage 3 GPS.
  **Persistence PASS (17:02 UTC):** all power cut 30+ s (LiPo removed too) -> RTC still correct to the second, `osf=0`,
  so the coin cell holds. Coin-cell type (CR2032 vs LIR2032) still UNKNOWN - owner to check later re: the §2.4
  charge-resistor note. Owner's multimeter read ~3.0-3.3 V (shaky probes) -> most likely a CR2032; this module has
  no 0x57 EEPROM so it may not be a ZS-042 - check it for a charge resistor/diode before leaving a CR2032 in long-term.
  **Stage 1 `x` stress with RTC on rail (LiPo in): PASS** - 75 pongs, 0 miss, 0 corrupt, all bursts ok, C5 resets 0.
  **Stage 3 Test 1: PASS** - C5 console (COM23) `gps: bytes=67864 nmea_ok=1996 bad=0` climbing (clean 9600 NMEA,
  GPS powered from S3 3V3 on battery). **Test 2: PASS** - 3D fix, 6 sats via the link (got it after the rig sat in the
  window on battery; USB cable too short to reach). **ALL BRING-UP STAGES PASS.** S3 reflashed to production `xiao_s3`:
  boots with RTC time (session `bg-20261021-135657` named at boot, pre-phone), logs `RTC disciplined from GPS`, bg scan
  cycling (34 devices). Open items: coin cell likely CR2032 -> check module for a charge resistor; redraw wiring
  diagrams; RTC persist test via the production path (phone/GPS sync -> power-cycle).
- **S3 battery sense — FIXED 2026-10-23 by moving it to D1 (GPIO2).** Every meter check on D3 passed, and a `[DIAG]`
  print proved the firmware read GPIO4 correctly (`bus=ADC_ONESHOT`, raw and calibrated mV agreed): the pin really sat at
  ~0.1–0.4 V, and a resolder made it worse. Bad pad or pin, cause unknown. Wire moved to **D1**, `BAT_ADC_PIN 2`: now
  `gpio2 ≈ 2020 mV` → `[S3] bat 4041 mV (89%)`. D3 is marked a suspect spare. Also fixed in shared `power.cpp`:
  `analogSetPinAttenuation()` was a silent no-op before the pin's first read (harmless here; 11 dB is the core default).
  Original notes below.
- **S3 battery sense — FLASHED, but READING WRONG (open, owner debugging later; 10-21 night):** owner soldered the
  100k/100k divider (switch output -> **S3 D3 = GPIO4**) + the power LED (3V3 -> 1k -> LED -> GND; works, no
  firmware). `power.cpp` moved `src/cyd/` -> `src/host_common/` (one impl, `power::init(BAT_ADC_PIN)` per board; S3
  stub deleted; CYD unchanged). Flashed `xiao_s3` (COM16; RAM 31.0% / Flash 41.0%) with two extra S3 fixes: the
  **idle loop now calls `hostboard::sample()`** (before, with scan off, the reading never refreshed -> stale STATUS
  `bat=`), `begin()` seeds a sample, and **`[S3] bat <mV> (<pct>%)` prints at boot + every ~30 s** (console check vs a
  meter). **Symptom:** console reads Vbat ~0.56–0.79 V (≈0.3–0.4 V at the pin) -> `bat=-1`, drifting steadily DOWN
  (794 -> 724 -> 706 -> 620 -> 561 mV) straight through the switch going off -> ON (switch on did power the C5:
  link + scan fine, 80 uniq). Looks like a floating ADC input, yet the **owner's meter says the divider reads
  correctly**. No GPIO4 conflict in `pins.h` / code. **Next:** meter **directly on the D3 pad** (not just the
  divider midpoint) vs GND with the switch on; confirm the wire lands on D3 (4th pad, USB side: D0..D3) and not
  D2; then if the pad is truly ~1.9–2.1 V, add a temp `[DIAG]` print of `analogRead(4)` raw +
  `analogReadMilliVolts(4)` to rule out the ADC/attenuation path (revert after). Then check STATUS `bat=` in the
  web app vs the meter. Owner's diagram shows the D3 wiring (still untracked).
- **Flashed tonight, both production firmware:** S3 = `xiao_s3` (COM16, MAC 10:b4:1d:e7:ed:a0),
  C5 = `xiao_c5` (COM23 that session, base MAC 10:bd:a3:cf:02:dc; identify with `esptool chip_id`, both
  are 303A:1001). **Background scan is OFF** on this unit (no `[S3] cycle` prints) → press Start in the
  web app to get detections.
- **C5 alone on its own USB: boots + scans fine** (`reset=11`, usual non-fatal PSRAM cacheline error),
  but `gps: bytes=0` (expected then: the GPS is powered from the S3's 3V3 and the S3 was off).
- **S3 on USB: `link=down` + `RTC not found`.** The bring-up probe reported the C5 **REBOOTING** (D7
  driven 10% / floating 89%). **Cause: there is NO battery fitted yet.** The two BAT pads are tied
  together, so with the S3 on USB the C5's only "supply" is the S3's onboard charger (current-limited,
  hunts with no cell) → C5 brownout loop. Same in reverse: the S3 rebooted ~45 times (session counter
  sess-00013 → 00059) while the C5 was on USB. **Not a wiring fault; the design needs the cell.**
  (The bring-up probe's "shared 3V3 sags" wording predates the rewire; it now means "BAT net can't supply".)
- **Testing without a battery:** (a) bench supply 3.8–4.0 V on TP4056 `B+/B−` (best stand-in for the
  cell), switch ON, TP4056 USB out; or (b) the temporary S3 5V → C5 5V jumper (gave link PASS on 10-20;
  remove it before the cell goes in). TP4056 USB with no cell = a charger, not a supply: may half-work,
  inconclusive. The owner may try (TP4056 only, both XIAO USBs out, check via the web app link icon).
- **I2C is dead, separate from the power issue:** the bring-up scan saw **nothing on the bus** (not even
  the ZS-042's 0x57 EEPROM) with the S3 on USB → the module is unpowered or both lines are open. Checklist
  given to the owner: (1) ~3.3 V at DS3231 VCC (S3 3V3 joint, which the GPS shares too); (2) the RTC is on the
  **S3's** D4/D5 (GPIO5/6), NOT the C5's D4/D5 (silk SDA/SCL = the C5 link UART); (3) continuity +
  no bridges D4–D5–D6; (4) SDA/SCL swapped.
- **Next:** owner fixes I2C + gets a cell (or a bench supply) → re-run [docs/mini-build-bringup.md](docs/mini-build-bringup.md)
  Stage 1 (flash `xiao_s3_bringup`, `p` probe, `x` stress, battery run), Stage 2 (RTC), Stage 3 (GPS: expect
  `gps: bytes>0` on the C5 console once the S3 powers it). Reflash `xiao_s3` afterwards.
- Wiring diagrams: the `.svg` + `_labeled.png` were redrawn and committed 2026-10-23 (current wiring, battery sense on D1); the plain `.png` is an older render.

## Earlier — mini build soldered bring-up: C5 POWER REWIRE (2026-10-20)
The owner moved the mini build from a breadboard (each XIAO on its own USB) to a soldered rig with the
TP4056 + switch. **Finding:** fed from the **S3's 3V3 pin into the C5's 3V3 pad** (the original
mini-build.md §2.5 plan), the **C5 never starts its firmware**. Its pad reads 3.3 V and every wire
passes continuity, but the new bring-up pin probe saw its link TX **100% floating, with no reboot
loop** (even after the C5 was changed to start its link UART first). A temporary **S3 5V → C5 5V
jumper** (C5 on its OWN regulator, S3 USB) gave `[LINK] PASS | pong 68 miss 0 corrupt 0 | burst 3/3
(28/28)`. Root cause unconfirmed (no XIAO-C5 schematic in the repo): either the S3's regulator can't
start the C5, or the C5's 3V3 pad isn't a usable supply input.
- **New power topology (docs updated: mini-build.md §2.5/§2.6, PINOUT.md §6e, mini-build-bringup.md):**
  switched TP4056 `OUT+` → **both** S3 `BAT+` and **C5 `BAT+`** (OUT− → both BAT−/GND). **No S3 3V3 →
  C5 3V3 wire.** RTC + GPS stay on the S3's 3V3. Rules: never plug in both XIAOs' USB at once; **keep
  the switch ON whenever any USB is in** (the C5 has no other supply).
- **Owner is desoldering the C5** to reach its underside BAT pads (in progress). Next: rewire per
  §2.6, then re-run [docs/mini-build-bringup.md](docs/mini-build-bringup.md) Stage 1 (incl. the `x`
  stress + a battery run), then Stage 2 (RTC) and Stage 3 (GPS).
- **Wiring diagrams are STALE:** `hardware/diagrams/mini-build_wiring_diagram{.png,.svg,_labeled.png}`
  are still untracked and show the old 3V3 feed. The owner is redrawing them, so don't commit the old
  ones.
- **New: bring-up test firmware `[env:xiao_s3_bringup]`** (`src/xiao_s3_bringup/main.cpp`, never
  shipped). Flash it to the S3 in place of `xiao_s3`; the C5 keeps `xiao_c5`. Every 3 s it prints
  `[LINK]` (pong/miss/corrupt, detection-burst completeness, C5 reset detection from uptime going
  backwards), `[RTC ]` (I2C scan, time, OSF, temperature), `[GPS ]` (fix via Status) and `[PWR ]` (S3
  reset reason, BROWNOUT flagged). `[PROBE]` reads the link RX pin with pull-down/up over 3 s:
  driven / floating / low / **REBOOTING** / D6-D7 bridged. Commands: `p` probe, `x` S3 Wi-Fi stress,
  `s <epoch>` set RTC, `i` I2C rescan, `c` clear.
- **C5 firmware changes (`src/c5/`, both `c5` + `xiao_c5`; no protocol bump):** the link UART now
  starts at the **top of `setup()`, before Wi-Fi/BLE** (so a radio-start-up brownout shows as a reboot
  loop on the host's RX line, not a dead wire); the boot banner prints `reset=<esp_reset_reason>`;
  `gps.cpp` prints `[C5] gps: bytes= nmea_ok= bad= fix= sats=` every 10 s (proves the NEO-6M wire
  indoors without a fix). All builds green (xiao_s3_bringup, xiao_c5, c5). **The DevKit `c5` was NOT
  reflashed**; the change is harmless for the CYD link, but the CYD rig still runs the old C5 binary.
- Earlier in the session: the owner's first soldered S3 had bad joints (resoldered all S3 pins, fixed).
  28 AWG stranded wire is fine electrically; tin the ends (stray strands bridge the 3V3/GND pads).

## New direction — scan-analyzer companion + remote-server pairing (PLANNED, 2026-10-15)
A separate **private** companion project — **scan-analyzer** — is being started: a self-hosted
server + web app that ingests this device's NDJSON logs into a database and **learns over time**
(estimate stationary-emitter locations from many GPS/time-stamped sightings; enrich OUI→vendor and
**BLE SIG company-ID**→manufacturer + likely-device research; wardriving map with GPS/time/source/
threat filters). Code public, **data + server private** (captured RF = bystander identifiers + GPS,
never committed anywhere). **Proposal + synthetic test fixture are in this repo's git-ignored
`scan-analyzer/` folder** (`PROPOSAL.md`, `sample-synthetic.jsonl`) for transfer to the new repo —
feed `PROPOSAL.md` into a fresh session there. The user will copy `.claude/hooks/` over; the new
session must review/extend `gitignore-sensitive.py` for the server's data/DB/.env paths.

**Status (2026-10-15):** the CYD↔scan-analyzer **API contract is APPROVED at DRAFT v0** by the owner;
**both sessions are holding the build** until usage resets (token-heavy). Full shapes captured in
outstanding **#11** + scan-analyzer `docs/api-contract.md`.

**Pending work in THIS repo (web app, build deferred):** a **"Remote server"** connection so the phone
web app can upload to scan-analyzer **without a hard-coded server address** — scan-analyzer shows a
**QR** deep-linking into this web app with `?server=<url>&token=<pairing-token>` (store in
`localStorage`), plus a **Settings "Remote server"** paste field as the fallback. Later: **live POST**
of detections once paired (Bearer = per-device key from the token); a standalone phone app is the
likely durable path for background live upload. **HTTPS mandatory** (mixed-content: this app is HTTPS,
so the server URL must be too — Tailscale / Cloudflare Tunnel / LAN cert). See CLAUDE.md "Companion
project + remote-server upload". Near-term upload is just download-a-log-then-upload-the-file (no
THIS-repo change). [[aggregate-triangulation-db-idea]]

**Hardware/modality expansion (exploratory, not started):** beyond RF, add boards or sibling setups
feeding the same scan-analyzer — a **camera/optical ALPR sub-board** à la **Flock-Noir** (MIT, now in
[docs/references.md](docs/references.md): XIAO ESP32-S3 Sense + OV2640/**OV3660** + OPT101; detects via
IR-flash pulse timing + brightness + RF, not CV); **geotagged image upload** + **on-device vision**
(XIAO S3 can run light ML); **per-device notes** (authored from the phone web app); and **Bluetooth
Classic** scanning (needs a new board — C5 has no BR/EDR, the CYD's ESP32 is held by NimBLE). Owner has
the Flock-Noir hardware (OV3660) and is testing viability. scan-analyzer PROPOSAL.md §11 + CLAUDE.md
"Hardware / modality expansion" carry the details; keep scan-analyzer's data model source-agnostic.

## Done — mini build (screenless XIAO sibling): Phases 0–4 + bring-up validated (2026-10-18, BOTH XIAO BOARDS FLASHED; committed)
Built the **screenless XIAO sibling** of the rig per [docs/mini-build.md](docs/mini-build.md): a **XIAO
ESP32-S3 Sense** host (replaces the CYD; no TFT/touch/BOOT — driven only via the BLE web app) + a **XIAO
ESP32-C5** scanner (replaces the DevKit C5) + a **DS3231 RTC** for a persistent clock. One repo, new
PlatformIO envs; the CYD-only `main.cpp` core was carved into a board-agnostic `src/host_common/` shared
by both hosts. **No `PROTOCOL_VERSION` bump** — all four firmwares stay link-compatible (v10); `src/c5/`
+ `lib/link_protocol/` untouched except the C5 `pins.h` board branch. New mini-build pinout: **[hardware/PINOUT.md](hardware/PINOUT.md) §6**.
Memory: [[mini-build-xiao-sibling]].
- **Phase 0 — scaffolding.** `src/c5/pins.h` branches on `ARDUINO_XIAO_ESP32C5` (XIAO link GPIO23/24,
  GPS RX GPIO1; DevKit pins in `#else`). Added `[env:xiao_s3]` (`seeed_xiao_esp32s3`) + `[env:xiao_c5]`
  (`seeed_xiao_esp32c5`); CYD filter → `+<cyd/> +<host_common/> -<host_common/rtc.cpp>`. Risk #1 cleared:
  the pinned stable platform (`55.3.312`) ships `seeed_xiao_esp32c5.json` — no local board def needed.
- **Phase 1 — host-core refactor (reviewed CLEAN).** 5011-line `src/cyd/main.cpp` → ~3250 lines +
  `src/host_common/` (`host_core`/`host_log`/`host_status`/`host_link`/`host_phone_cmds` + `host_board.h`/
  `host_ui.h`; sigdb/phone/webshare/whitelist/logfilter/power.h moved). `setup()`/`loop()`/the `SCR_*`
  state machine stay per-board; only the functions they call moved. Reviewer diffed vs HEAD → **CYD
  byte-for-behavior identical** (STATUS/DETS/NDJSON/on-screen flow), only sub-100 ms repaint-latency shift.
  CYD **Flash 45.9% / RAM 34.2% unchanged**.
- **Phase 2 — headless XIAO-S3 host.** `src/xiao_s3/` (`main.cpp` tiny setup/loop, `host_board.cpp`,
  `host_ui.cpp` serial-only hooks, `power.cpp` stub, `pins.h`). `foregroundScan()` uses a dedicated
  `g_fgScan` flag (not `g_scanActive`) to keep the fg/bg stream split. Builds 40.7%/30.9% (~197 KB < CYD).
- **Phase 3 — DS3231 RTC (host, I2C GPIO5/6).** `src/host_common/rtc.*` direct-I2C BCD read (no RTClib),
  probes only 0x68 (ignores AT24C32 0x57). Time **source priority 3** + hourly **write-back sink**
  (`disciplineRtc`). CYD stays byte-identical (`rtc.cpp` excluded from its filter; core reaches RTC via a
  null `g_rtcWrite` fn-ptr — including `<Wire.h>` would link ~32 KB). tz sign is `− tzOffsetMin*60`
  (matches the GPS branch). `STATUS tsrc=3` = RTC. CYD Flash +356 B only.
- **Phase 4 — headless Wi-Fi-download QR.** STATUS appends `apssid/appsk/apurl` while `webshare::active()`
  (buffer 512→640); web app `features/wifiap.js` renders a `WIFI:T:WPA;S:;P:;;` join QR + an open-index
  link. The web app had **no QR renderer** → **vendored MIT `qrcode-generator`** to
  `webapp/js/vendor/qrcode.js` (NOT a CDN dep; verified MIT, no network/eval; attributed in
  [docs/references.md](docs/references.md)).
- **Web-app C5-link indicator (NEW, this session).** The headless host has no screen, so the CYD's link
  dot had no home — added a `#linkState` app-bar icon (`cable`, green up / red down from STATUS `link=`)
  in `webapp/index.html` + `js/ble/status.js` + reset in `connection.js`. JS/HTML-only (no SCSS recompile);
  `#uiver → c5-link-icon`.
- **Bring-up (2026-10-18, on hardware).** Both XIAO flashed (S3 native `303A:1001` MAC `10:b4:1d:e7:ed:a0`;
  C5 native `303A:1001` base MAC `10:bd:a3:cf:02:dc`, confirmed ESP32-C5 by `esptool chip-id`). S3 boots
  clean (PSRAM ok, **SD mounts on GPIO21**, sigdb loads, BLE advertises `COINTEL-FLO`), **DS3231 detected**
  (`[S3] RTC present, time not set`). Initial link was DOWN — **root cause: link wiring not crossed** (the
  D-pad↔GPIO map: C5 GPIO23=D4 / GPIO24=D5); after the user fixed it, the link is **up and streaming
  dual-band detections** (`[S3] cycle: 2.4:6 5G:2 BLE:9 PRB:10` — **5 GHz confirmed**, risk #3 antenna ok).
  The C5 prints a non-fatal `BOARD_HAS_PSRAM` cacheline error at boot (risk #2) but runs fine.
- **On-device TODO (non-blocking):** RTC **persist** test (sync time via phone → power-cycle → boot log
  should read `RTC valid (latched)` + logs timestamped from row 1); web-app in-browser check of the new
  link icon + the Wi-Fi-download QR; a threat/5 GHz walk. Optional cleanup: drop forced `BOARD_HAS_PSRAM`
  on `xiao_c5` to silence the boot error. Camera (XIAO-S3 Sense) stays **deferred** (pins reserved).
- **Builds:** all four envs SUCCESS (cyd 45.9%/34.2% unchanged; xiao_s3 40.7%/30.9%; xiao_c5 45.4%/24.0%;
  c5 untouched). Webapp JS `node --check` clean.

## Done — follower count DISABLED + OUI/company-ID maker labels (2026-10-15, CYD FLASHED COM14 + webapp; pushed)
Context: a field drive to test the follow alert showed the count inflating (6→10→2→1). Root-caused: the
follow gate is an **anchor-radial GPS span** (`computeFollowScores`, `maxSpanM` = the max straight-line
distance from a device's first-seen anchor to the current position, a running max) — it can't distinguish
"a device followed me 400 m" from "I drove 400 m away from a fixed device," so it over-fires on roadside
infrastructure. The remaining "1" the user took for their car was actually their **own phone** (a Samsung
BLE RPA the `peerMac` self-filter misses, since a phone's advertised RPA ≠ its GATT address). Three changes
(CYD-only + webapp; **no `PROTOCOL_VERSION` bump**, `src/c5/` + `lib/link_protocol/` untouched, **C5 NOT
reflashed**):
- **(A) Follower count/alerts DISABLED, code preserved.** New `FOLLOW_ALERTS_ENABLED = false` flag +
  `followAlertN()` accessor (`src/cyd/main.cpp`). The follow DETECTION still runs
  (`updateFollowState`/`computeFollowScores`/`isFollowing`), so the follow-gated **TRACKER** category is
  UNCHANGED (user's choice). Only the surfaces are suppressed: CYD scan + HOME banners, magenta LED,
  STATUS `fol` (→0, hides the web follower banner), DETS `ftier`/`fscore` (→0, hides web follow markers),
  the `evt:follow` log line, and the drill-down (now unreachable — its only callers are gated). Flip the
  flag to `true` to restore the full UX, no other edits. A co-travel rework is the planned fix before
  re-enabling. [[phase5-followme-direction]]
- **(B) OUI vendor display label.** New display-only `vendor,<OUI>,<label>` sigdb rule + `vendorFor(mac)`
  (never scored; parsed before the 5-field split so labels may contain commas). `groupName()` now shows
  `<vendor> AA:BB` for a nameless device with a real globally-administered MAC that isn't a BLE RPA (else
  the raw MAC) — propagates to list/detail/DETS/web. Seeded Espressif + Raspberry Pi; user-expandable
  (one line, reload DB, no reflash). **RPAs are excluded by design** (random addresses can't be a vendor).
- **(C) BLE company-ID maker label.** New display-only `cidname,<hex>,<label>` sigdb rule + `cidName(cid)`
  (falls back to any matching `blecid,` threat label, so Apple/Tile/Flipper/etc. resolve for free). Shown
  **IN ADDITION to the MAC**: CYD **detail** screen → "BLE company: Samsung (0075)"; webapp **live list**
  r2 line (`.mk` accent span) + device modal. Carried to the phone via a new trailing **DETS v7** field
  (CYD→phone stream; append-only, older parsers ignore). `DevGroup` gained `companyId` (first across
  members, set in `buildGroups`). This is the stable identifier for rotating-RPA devices (phones,
  watches, car head units) where no OUI exists. **Seed expanded to ~32 common Bluetooth SIG makers**
  (Apple/Tile/etc. also via the `blecid` fallback), each **verified against the SIG assignment list** —
  which caught a decimal/hex bug: Garmin is `0x0057` (87 dec), not `0x0087`. Second CYD flash for this.
- **`DB_GEN` 3→4→5** so the card re-seeds `/signatures.csv` on next boot (⚠ DISCARDS on-card DB edits).
- Builds: **CYD SUCCESS** (RAM 34.2% / Flash 45.8%). Webapp: SCSS recompiled (`style.css?v=20261015a`,
  `#uiver → maker-labels`), JS `node --check` clean. **CYD FLASHED COM14** (`1A86:7523`). Pushed to `main`.
- **On-device TODO (non-blocking):** eyeball the CYD detail "BLE company" line + the webapp live-list
  maker tag; the phone's own Samsung RPA should now read "Samsung" next to its (rotating) MAC. The car
  **stereo itself is still invisible** — its named side is Bluetooth Classic, which the BLE-only scanner
  can't see; **BT Classic inquiry** (needs a NimBLE→Bluedroid rework) and **IRK/RPA resolution** (to
  filter the own phone) remain backlogged, both declined this round.

## Done — follower logging + HOME-page alerts (2026-10-14, CYD FLASHED COM14 + pushed `1aaebea`; webapp Pages-deploying)
Motivated by a field drive: the magenta follow LED lit while driving, but the user was on the CYD HOME
screen + the webapp **Home** page — and the follower banner is **Scan-page-only on both platforms**, so
there was nowhere it could show, and the FOLLOWING device's MAC/flags were only live (5-min TTL), making
"was it my car or my own phone?" unanswerable after the fact. Two features (CYD-only firmware + webapp;
**no PROTOCOL_VERSION bump**, `src/c5/` + `lib/link_protocol/` untouched, nothing crosses the link):
- **(A) Log followers (CYD, `src/cyd/main.cpp`).** New `logFollowEvents()` (modeled on `logRidEvents()`,
  per-MAC ring sized `MAX_DET`, rate-limited once per `EVENT_LOG_GAP_MS`), called right after
  `logRidEvents()` in `runScanCycle()`. One NDJSON line per device at FOLLOWING (`ftier==2`):
  `{"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"follow","mac":"AA:BB:..","score":N,"ftier":2[,"name":..][,"src":..][,"flags":..]}`
  — `src` via `srcTag()` (strongest member), `flags` via the existing `flagTokens()` (so a **Find-My
  iPhone** reads `findmy` — the key tell of own-phone vs. a car device), `name` JSON-escaped; overflow-safe
  `lnAppend` with a closing-brace guard. `parseScanNdjson` already skips `evt` lines (not mis-read as a
  detection). **This is how to answer car-vs-phone after a drive: download the session + read the
  `evt:follow` lines' `mac`/`flags`** (RPA MAC `4x–7x` + `findmy` ⇒ phone; stable vendor MAC ⇒ car/other).
- **(B) HOME-page alerts.** CYD: new `drawHomeAlerts()` band at **y=120** (`HOME_ALERT_Y`, free strip
  between the Scan button ~y109 and the "Background threats" caption y176), same magenta `! FOLLOWING: N
  (tap)` / red `! DEAUTH:/EVIL:` labels + stacking as the scan banner; drawn from `drawMenu()` + the ~2 s
  SCR_MENU idle refresh; clears itself when the condition drops; **tappable** → `openFollowList()` (new
  `g_followFromHome` flag so Back returns to HOME, not Scan). Webapp: `#folBanner`/`#alertBanner` **moved
  out of `<section id="page-scanner">` to just above `<main id="pages">`** so the one element each is
  visible on EVERY page (Home included) with no duplicate ids — `status.js`/`connection.js`/`main.js`
  wiring unchanged (same ids). `map.js` also gives `evt:follow` a magenta `F` pin (score/name/MAC/src/
  flags popup). SCSS banner margin flipped to `0 0 .5em` in `_banners.scss` → recompiled `style.css`;
  `style.css?v=20261014f`, `#uiver → home-alerts`.
- **Builds:** CYD SUCCESS (RAM 33.8% / Flash 45.9%); C5 not rebuilt (untouched). Webapp: SCSS recompiled,
  `parse.js` `node --check` clean.
- **Shipped:** CYD **FLASHED COM14** (`1A86:7523`, ESP32-D0WD-V3 MAC `d4:e9:f4:f4:17:b8`, first-try
  SUCCESS). **Committed + pushed to `main` `1aaebea`** (8 files); **GitHub Pages building** for
  `1aaebea` at push time (the ~10-min cache then applies — footer `#uiver → home-alerts` = live).
- **On-device TODO (not blocking):** confirm the HOME alert band y=120 doesn't collide with the Scan
  button / threat tiles + the tap opens the follow list (needs a live FOLLOWING or an event to render);
  webapp not yet in-browser tested. Then a drive to capture `evt:follow` lines and read `flags`/`mac`
  to settle car-vs-phone (#6).

## Done — COINTEL-FLO rebrand + GPS time + UI polish (2026-10-14, BOTH BOARDS FLASHED v10, all pushed to `main`)
A run of CYD/webapp changes this session (newest first). Device state at the end: **both boards on
protocol v10**, interoperating (link verified `table=55` dual-band). Ports this session: CYD = CH340
`1A86:7523` = COM14; C5 native = `303A:1001` = COM18 (confirmed ESP32-C5 by `esptool chip-id`, base
MAC `d0:cf:13:e4:6a:74`); spare S3 odid-tx = `303A:1001` = COM12 (MAC `a4:cb:8f:da:45:b0`). Identify
by chip, not COM.

- **Evil-twin event names the suspect SSID/BSSIDs + click-to-detail (`60db663`, CYD FLASHED + webapp
  pushed).** The map's `evt:eviltwin` pin was count+time only; the suspect addresses lived only on the
  individual AP detections. Now the CYD's `evt:eviltwin` NDJSON line also carries `"ssid"` + `"macs"`
  (suspect SSID + up to 8 comma-joined BSSIDs, from the live detections carrying `FLAG_WIFI_EVILTWIN`;
  `logEvent` buffer widened to `LOG_LINE`). Deauth stays count-only (the C5 reports a frame count, no
  per-AP address). Web app: `parse.js` reads `ssid`/`macs`; the Map event-pin popup lists the SSID +
  each BSSID, and **each BSSID is clickable → opens that device's full detail modal** (map builds a
  MAC→row lookup; click handlers attach on `popupopen` because Leaflet stops click propagation;
  `openDetail` via a deferred `import('./detail.js')` to avoid a static map↔detail cycle). New
  `.evmac`/`.evhint` SCSS; `style.css?v=20261014e`, `#uiver → eviltwin-addr`. CYD-only, no protocol
  bump. **Applies to NEWLY-logged events** (enrichment is at log time); old sessions keep count-only
  lines. Context: the owner hit an evil-twin alert likely from their own router's **guest Wi-Fi**
  (same/similar SSID, open vs WPA2 = the open-vs-secured mismatch the detector flags; benign).
- **HOME touch-lock readout fix (`be50c4a`, CYD FLASHED).** The bag-safe touch lock early-returns from
  `loop()` before the per-screen switch, so the HOME threat tiles (repainted only in the `SCR_MENU`
  idle branch) **froze on screen while locked** — scanning/logging kept running (bg cycle is before
  the lock gate), only the *display* stalled. The Scan screen was fine (the locked block re-runs
  `runScanCycle()`→`updateScan()`). Fix: in the locked branch, when on `SCR_MENU`, repaint status bar
  + `drawMenuThreats()` on the ~2 s idle cadence. CYD-only, no protocol bump.
- **STATUS `tsrc` + webapp time-source indicator (`d2f6de4`, CYD FLASHED + webapp pushed).** STATUS
  gained **`tsrc`** = `timeSource()` (0 none / 1 phone / 2 onboard GPS). The web app tints the app-bar
  **time** icon cyan (`.si#timeState.onboard`) when `tsrc===2`, with a "Time: onboard GPS / phone-synced
  / not set" tooltip — mirrors the existing `gpssrc` handling. `style.css?v=20261014d`, `#uiver →
  time-source`. CYD→phone only, no protocol bump.
- **Onboard GPS time fallback clock — protocol v9 → v10 (`712fd9f`, BOTH BOARDS FLASHED v10).** The
  NEO-6M's RMC **date+time** now feeds the CYD wall-clock so it timestamps logs with **no phone**, as
  long as it has a fix. `Status` gained `uint32_t gpsEpoch` (UTC secs; `sizeof` 24→28; `static_assert`
  + `PROTOCOL_VERSION` 10). C5 `gps.cpp` parses RMC `hhmmss`+`ddmmyy` via a clean-room days-from-civil
  epoch calc, set only on a valid (`A`) fix (GGA is time-only → can't set it). CYD: unified clock
  `timeHasSync()/epochLocal()/timeSource()` prefers the phone, else the latched GPS UTC (free-runs on
  `millis()`, refreshed on newer epochs); **every** clock consumer routes through it (NDJSON epoch,
  event/rid lines, session rename + timesync anchor, time-filtered export, STATUS `time=`, status-bar
  clock, Explore detail). **Timezone:** GPS is UTC-only, so `phone.cpp` **persists the last phone tz
  offset in NVS** (`cydscan/tzoff`, exposed via `tzOffsetMin()`) → a phone-less GPS boot reads LOCAL
  time after the first-ever phone sync (UTC until then). Status-bar clock is **cyan when GPS keeps
  time**, white when phone-synced. **Needs an outdoor fix (phone disconnected) to field-validate**;
  no sky view at the bench = `gpsEpoch` 0 = identical to before.
- **GPS status-bar glyph + tap-to-open modal (`9b8cd1c`, CYD FLASHED).** Replaced the middle GPS
  coordinate TEXT with a **location-pin glyph** in the right-hand icon cluster (left of the battery).
  Source badge: a small **phone handset** when the fix is from the linked phone, a little **chip/IC**
  when it's the onboard NEO-6M; dim-grey pin / no badge when no fix (colour also white=phone /
  cyan=module). **Tapping it opens a GPS modal** (source + lat/lon + module sat count); tap/BOOT to
  close, `redrawScreen()` restores the screen under it. Not offered on the stateful device-detail
  screens (no clean no-arg repaint); the scan cycle pauses while the modal is up (its list repaint
  would paint over the popup) and resumes on dismiss.
- **Status-bar icon spacing (`71c75de`, CYD FLASHED).** The lock↔phone gap was ~17 px vs ~7 px
  elsewhere; `lockCx()` moved `W-52 → W-42` (the battery glyph tracks it at `lockCx-30`) so the GPS /
  battery / lock / phone / link-dot cluster is evenly spaced.
- **Magenta `#AA00AA` UI accent (`4585770`, CYD FLASHED + webapp pushed).** New CYD `UI_ACCENT`
  (RGB565 `0xA815`) replaces `TFT_NAVY` for selected menu-item/button fills + the "< BACK" bar (left
  `TFT_BLUE` for the RESUME state + BLE icon). Web app `$color-accent` `#238636` (green) → `#aa00aa`
  (primary buttons, scan/pause record, range + nav-active chip). `style.css?v=20261014c`.
- **COINTEL-FLO rebrand (`0ad9ec7`, `9014133`, CYD FLASHED + webapp pushed).** HOME menu title → logo
  magenta **COINTEL-FLO**; **boot splash** of the logo (`resources/logo/logo.png` baked to
  `src/cyd/logo.h` as a 240×32 RGB565 `pushImage` blit — the TFT has no PNG decoder; generator kept at
  `resources/logo/gen_logo.py`), held **3 s** (was 2 s, bumped `fab...`→ this session). BLE device
  name + on-device Wi-Fi log page `CYD-Scanner`/`CYD Scanner` → **COINTEL-FLO** (web discovery is by
  service UUID, so the advert rename is safe). Web app `<title>` + app-bar → **COINTEL-FLO**.
  **Can the logo HTML/Python/PNG be used directly on the CYD?** No — no browser/terminal/PNG-decoder;
  the PNG was baked to an RGB565 array (preferred over ASCII art on a graphical TFT). The logo.html is
  the directly-usable one for the web.

### Resolved this session
- **C5 GPS pins 15/23 — CONFIRMED.** The owner wired the NEO-6M as-is and it works; RX=GPIO15 proven
  by a real fix, TX=GPIO23 is documentation-only (code passes `TX=-1`, receive-only). No action.
- **Battery charge-detect — NOT FEASIBLE (dropped).** The owner inspected the TP4056 module: the two
  extra pads are the DC **input +/−**, not a CHRG/STDBY status pad. So there's no software charge-detect
  on this module; the **voltage-only** battery indicator is the final form and **GPIO26 stays free**.
- **Remote-ID web-app map (increment 4) — VERIFIED working.** The owner saw the drone/operator plotted
  on the web map against the `tools/odid-tx` bench S3. (The marker sits in NYC because the S3 broadcasts
  **hardcoded** coords — drone 40.7128,−74.0060 / operator 40.7130,−74.0062, Lower Manhattan — NOT an
  internet/VPN location; the S3 isn't networked and the web app plots strictly `rid.olat/olon`.) The
  full Remote-ID chain (C5 decode → link → CYD detail/log → DETS → web map) is now end-to-end verified.


## Done — onboard GPS (protocol v9) — 2026-10-14
- **What:** the GY-NEO6MV2 NEO-6M wired to the **C5** is now read in firmware and its fix is forwarded
  to the CYD. The C5 reads NMEA on **UART0** (console is USB-CDC → UART0 free; RX = GPIO15, TX = -1
  **receive-only**, 9600 8N1, XTAL clock, 512 B RX buf) with a **clean-room, checksum-verified**
  `$GxRMC`/`$GxGGA` parser (`src/c5/gps.{h,cpp}`, no library), holds the latest fix mutex-guarded, and
  `sendStatus()` folds it into the `Status` frame.
- **Protocol v8 → v9.** `Status` gained `gpsLat`/`gpsLon` (int32 deg·1e7, 0 = no fix) + `gpsAltM`
  (int16, −1000 unknown) + `gpsSats` + `gpsFix` (0/1/2); `sizeof(Status)` = 24 (`static_assert` updated).
  FrameParser rejects a version mismatch, so **BOTH boards must be flashed together** (v9). ⚠️ Flashing
  needs the power switches set per PINOUT.md §4 (SW2 off to flash the C5; SW1 off to flash the CYD) and
  `PYTHONIOENCODING=utf-8`.
- **CYD: phone GPS PREFERRED, onboard FALLBACK.** A unified `posHasFix()/posLat()/posLon()` accessor
  (`src/cyd/main.cpp`) returns the phone fix when `phone::hasGps()`, else the onboard C5 fix (stored in
  the `Reply::Status` branch, valid when `gpsFix!=0`), else none. All six position consumers were
  switched to it (follow scoring, NDJSON log lat/lon, event + rid log lines, STATUS `gps=`, status-bar
  text). With no module present the Status carries `gpsFix=0`, so behaviour is byte-identical to the old
  phone-only path. Web app needs **no change** (it reads `gps=` / logged lat-lon as before).
- **Receive-only / in scope:** security pass PASS — TX=-1, no writes to the module, bounded parser, no
  new data-egress (fix flows only over the existing link + into the same device-local SD log).
- **BOTH BOARDS FLASHED v9 (2026-10-14) + VALIDATED ON-DEVICE.** C5 on COM18 (`303A:1001`, confirmed
  ESP32-C5 by `esptool chip-id` — COM12 `303A:1001` is the odid-tx S3, don't flash it), CYD on COM14
  (`1A86:7523`); both power switches OFF during flashing. User confirmed a **real onboard fix** with
  the **phone disconnected** and the coordinates correct — so the full path (NEO-6M → C5 UART0 parse →
  Status v9 → link → CYD status bar) works end to end. Link verified up post-flash (`table=60`, dual-band).
- **Polish applied + `gpssrc`/`sats` readout (2026-10-14, CYD + webapp, reflashed):** reviewer nits done
  (onboard lat/lon now divided in `double` not `1e7f`; `#include <math.h>` in `gps.cpp`). Added a source
  indicator: CYD `posSource()` (0 none/1 phone/2 onboard) + `g_onbSats`; STATUS gained **`gpssrc`** +
  **`sats`**; the CYD status-bar GPS text is **cyan when the onboard fix is active** (white = phone); the
  web app tints the GPS app-bar icon + shows the sat count when onboard is driving (`#gpsSats`,
  `style.css?v=20261014b`, `#uiver → gps-source`). CYD-only STATUS additions — **no further protocol
  bump** (still v9). README privacy note added (self-geolocation with no phone).
- **Files:** `lib/link_protocol/link_protocol.h`, `src/c5/gps.{h,cpp}` (new), `src/c5/pins.h`
  (`GPS_RX_PIN 15`/`GPS_TX_PIN 23`), `src/c5/main.cpp`, `src/cyd/main.cpp`; webapp `index.html`,
  `js/ble/status.js`, `scss/_base.scss`→`style.css`; docs: `hardware/PINOUT.md` §5/§5b,
  `hardware/addon-wiring.md` §3b/§4b (DONE), `README.md`, `CLAUDE.md`.
- **Builds:** c5 SUCCESS Flash 45.9% / RAM 24.0%; cyd SUCCESS Flash 45.2% / RAM 33.4%. Reviewer CLEAN,
  security PASS. **Committed + pushed to `main` 2026-10-14** (same commit carries the `resources/logo/*`
  edits per the owner — logo is "Updated logo. Still TBD").
- **Open / next session (all RESOLVED 2026-10-14 — see "Resolved this session" above):**
  - ~~Sanity-check C5 GPS pins 15/23~~ — CONFIRMED working (RX=15 proven by a real fix, TX=23 unused).
  - ~~Battery charge-detect~~ — NOT FEASIBLE on this TP4056 (extra pads are DC input +/−, no CHRG pad);
    voltage-only is final, GPIO26 stays free.
  - **`resources/logo/*`** — now wired into the boot splash + rebrand; artwork still "Still TBD" per
    the owner (finalize the logo PNG later, then re-run `resources/logo/gen_logo.py` to regenerate
    `src/cyd/logo.h`).

## Branches / deploy
- **2026-10-14: `main` is the only branch and the trunk; working tree clean.** The Remote-ID feature
  branches (`remoteid-decode`/`remoteid-ui`) all merged to `main` and the whole 2026-10-14 session
  (rebrand, GPS glyph/modal, GPS-time v10, accent, lock fix) was committed straight to `main`. Start
  the next dev round with a fresh `git checkout -b <name>` only if you want isolation.
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

## Done — battery level indicator (CYD status bar + web app) + v2-build docs (2026-10-14, CYD built; NOT flashed/pushed yet)
User updated the physical rig to match `hardware/diagrams/cyd-scanner_wiring_diagram_v2.png` (adds the
on-C5 **GY-NEO6MV2 GPS**, the **GPIO35 battery-sense divider**, and a **second switch SW2** on the
boost-OUT+ → C5 5V leg so the CYD's USB 5 V can't back-feed the C5 while flashing). Then asked for a
battery indicator on both the CYD status bar and the web app. Confirmed with the user before building:
**two switches** (SW1 master + SW2 C5-5V), **100 kΩ/100 kΩ** divider (×2), **voltage-only** (TP4056
CHRG is **not** wired → no charge-detect; GPIO26 stays free).
- **Docs (done):** `hardware/PINOUT.md` — diagram link → v2; §1 GPIO note (35 = battery, 26 free);
  §2 C5 GPS note (15/23); §4 power rewritten for **two switches** (SW1 master on battery→boost, SW2 on
  the C5 5V leg; CYD VIN direct) incl. ASCII + per-board flashing rules; §5 promoted PROPOSED→**BUILT**
  (battery firmware live; GPS wired but firmware pending, still protocol-v9-gated). `hardware/addon-wiring.md`
  status → BUILT (Build C minus CHRG). `README.md` Power note (SW1/SW2 + battery indicator + GPS).
  `CLAUDE.md` key-pin-facts. `src/cyd/pins.h` (`BAT_ADC_PIN 35`, GPIO26-free note).
- **Firmware (CYD, built OK):** new `src/cyd/power.{h,cpp}` — `analogReadMilliVolts(35)×2`, 8-sample
  avg + EMA, LiPo resting-voltage→% curve; `percent()` −1 when Vbat<2.5 V (no cell / on USB). `main.cpp`:
  `power::init()`+seed in `setup()`, `power::sample()` at the top of `runScanCycle()` (every ~2 s,
  C5-independent), a **battery glyph** in `drawStatusBar()` left of the padlock (fill + colour: green
  >50 / yellow 20–50 / red <20; hollow grey when absent), and `bat=%d` appended to the STATUS string
  (`s[360]`→`s[384]`). **No `PROTOCOL_VERSION` bump** (STATUS is CYD→phone, not the C5 link; C5 untouched).
  Build SUCCESS (RAM 33.4 % / Flash 45.2 %).
- **Web app (static-verified):** app-bar battery indicator (`#battState`/`#battIcon`/`#battPct`,
  Material battery icons by level); `ble/status.js` parses `bat=` (−1/absent → `.off`, <20 % → `.lowbat`
  red); reset on disconnect (`ble/connection.js`); `scss/_base.scss` `.si.batt` styles → recompiled
  `style.css`, `style.css?v=20261014a`, `#uiver → battery-indicator`.
- **NOT yet:** CYD not flashed; nothing committed/pushed; web app not in-browser tested. On-device check:
  battery glyph reads a plausible level (SW1 on), goes hollow-grey on USB-only (SW1 off), and the phone
  app shows the % — then flash (SW1 off while flashing the CYD), commit, push (Pages redeploys).

## Done — pre-sync time backfill (timesync anchor + reader backfill) (2026-10-11, CYD FLASHED COM14 + webapp, pushed)
User request: rows logged by the boot/background scan BEFORE a phone connects are written with
`epoch:0` (only `ms`-since-boot is meaningful); make their real time recoverable without rewriting the
append-only log. Chose the **sync-marker + reader-backfill** approach (option A), not on-device rewrite.
- **CYD** (`renameStreamOnSync`, `src/cyd/main.cpp`): when the phone sets the clock, append ONE
  `{"epoch":<syncEpoch>,"ms":<syncMs>,"evt":"timesync"}` anchor line to each stream that already holds
  pre-sync rows (the `started` branch), THEN rename the file. Pre-sync rows are left untouched
  (append-only preserved). CYD-only, no protocol/C5 change. Built + **flashed COM14**.
- **Webapp** (`data/parse.js`): new `backfillEpoch(items, sync)`; `parseScanNdjson` + `parseScanEvents`
  capture the `timesync` anchor in-pass and, after parsing, set `epoch = sync.epoch + round((ms −
  sync.ms)/1000)` for any `epoch:0` row/event. So Explore/Map show real clock times for boot-scan rows.
  `timesync` is excluded from map event pins. Legacy CSV unaffected (no anchor). `#uiver → timesync-backfill`
  (JS-only, `style.css?v=` unchanged). **Verified** with a node harness: pre-sync rows/events backfill to
  the right epochs, post-sync rows unchanged, timesync excluded. Static-verified; in-browser pending.
- **GPS is NOT backfilled** — no position existed before the phone's GPS stream started (onboard GPS is
  the backlogged hardware item); only time is reconstructable (linear ms→epoch offset).

## Done — webapp Scanner PAUSE button (2026-10-11, webapp-only, pushed)
User request: a pause control on the Scanner page matching the CYD's PAUSE (`g_viewFrozen`). Added
`#pauseBtn` to the `#scanPanel` icon row, right after Start/Stop (`pause`↔`play_arrow` icon, accent
highlight while paused). It **freezes only the live detection list** on the snapshot at pause time; the
threat counts/strip, the sighting trail, and scanning/logging all keep running — identical to the CYD,
where the view freezes on `g_frozenRows` while the scan continues. **Client-side only, no CMD/firmware**
(unlike Start/Stop's `G`/`X`). `state.paused` gates the list render in `renderDets()`; `togglePause()`
+ `resetPause()` (detections.js); wired in `main.js`; added to the connect/disconnect enable lists +
reset on disconnect (connection.js). SCSS `#pauseBtn.paused` highlight → recompiled `style.css`,
`style.css?v=20261011h`, `#uiver → scan-pause`. **Confirmed working in-browser (user, 2026-10-11).**
First-load "greyed out, can't use it" report was the usual **JS cache skew** — the phone had the new
`index.html` (button visible) but still-cached old `connection.js`/`main.js` (never enabled/wired it,
so it stayed `disabled`). Resolved by clearing the site's cached data / a fresh JS fetch after the
~10-min Pages revalidation (server was verified correct via `curl` the whole time). Reminder: a
new-HTML-but-stale-JS gap is expected for ~10 min after any webapp deploy — see "Webapp cache /
deploy-propagation".

## Done — webapp Home Threats: 3/row × 4 rows + full category labels (2026-10-11, webapp-only, pushed)
Cosmetic alignment with the CYD HOME readout (user request), on `main` after the Remote-ID merge:
- **3 cards/row × 4 rows** (was 4/row × 3): `_detections.scss` `.tgrid` `repeat(4,1fr)`→`repeat(3,1fr)`,
  recompiled `style.css`, `style.css?v=20261011g`. There was vertical room for the 4th row, no scroll.
- **Full category labels restored** now the cards are wider: removed `CAT_CARD_LABEL` (Tracker→Track /
  Flipper→Flip) in `features/categories.js` so every card shows its full name like the CYD (JS-only).
- `#uiver` bumped (`threats-3col` → `cat-fullnames`). No firmware change. Static-verified; the Pages
  ~10-min cache applies (footer flip = live).

## Done — Remote-ID increment 4: DETS RID block + web-app map (2026-10-11, branch `remoteid-ui`, CYD FLASHED COM14; webapp static-verified)
Final Remote-ID increment — carries the CYD's decoded RID (`g_rids[]`) to the phone and plots it.
**No link `PROTOCOL_VERSION` bump, no C5 change** (`src/c5/` + `lib/link_protocol/` untouched, still v8);
DETS is the CYD→phone BLE stream, versioned separately (now **v6**).
- **CYD** (`pushDetections`, `src/cyd/main.cpp`): after the `n` detection rows, when `g_ridStoreCount≥1`,
  appends a `R:<ridCount>` header + one 11-col tab row per emitter
  `mac\tuas\tdlat\tdlon\tolat\tolon\th\talt\tspd\thdg\toid` (coords `%.6f` empty at sentinel, h/alt m,
  spd m/s, hdg deg; `uas`/`oid` via `sanitizeField`; `row[256]`, `detsNotify`+`delay(6)` pacing). Block
  is appended (old parsers read `n` rows then wait for the next `D:` → unaffected).
- **Webapp** (JS-only, no CSS/SCSS change): `state.rids` (by lowercase MAC, atomic per-snapshot swap);
  `detections.js` parses the `R:` block + skips unrecognized lines; `detail.js`
  `renderDetailMap(points,msg,rid)` adds a drone (`CAT_COL[9]` green) + operator marker + connecting
  line; `categories.js openLiveDetail` adds a Remote-ID kv section; `data/parse.js` extracts `evt:"rid"`
  fields (+ a latent fix: `parseScanNdjson` now skips `evt` lines, which carry a `mac` and were being
  mis-parsed as detections — reviewer confirmed no real detections dropped); `map.js plotRid()` plots
  logged + live RID (separate `mapRidEvents` list so the drone/operator GPS bypasses the phone-fix
  filter), gated by the events filter. **`#uiver` → `ui 2026-10-11 · rid-map`** (JS-only, so `style.css?v=`
  untouched — note JS rides the 10-min Pages revalidation; the `#uiver` flip signals the new build).
- **Builds** SUCCESS both envs (CYD Flash 44.9% / RAM 33.4%; c5 sanity SUCCESS). Reviewer: no blockers
  (row buffer safe, backward-compat both directions, atomic RID update/self-heal, single `#detMap`
  Leaflet instance, clean import DAG). **CYD FLASHED COM14.** **Webapp static-verified only — in-browser
  (Android Chrome + a live RID over the phone link, or a loaded `evt:rid` log) still TODO.**

## Done — HOME threat tiles: per-category colours + threat-score badge (2026-10-11, branch `remoteid-ui`, CYD FLASHED COM14)
User-requested consistency fix: the CYD HOME category tiles were coloured by **tier** while the web app
colours the same categories by **category hue** (`CAT_COL`). Aligned the CYD to the web app:
- New **`kCatColor[CAT_N]`** RGB565 palette (`src/cyd/main.cpp`) approximating `webapp/js/config.js`
  `CAT_COL` (TFT 16-bit gamut can't match sRGB exactly → nearest RGB565, documented per entry). Tiles
  now use the category hue for outline + label (dim grey at 0, cyan when BOOT-selected).
- Because the hue no longer encodes severity, each hot tile gets a **tier-coloured number badge** on the
  right (`drawMenuThreats`): the **sigdb score**, or the **tier level 1-3** when the DB gave no numeric
  score (user chose "score, fall back to tier" — so a behavioral-flag-only threat like the ODID test
  drone reads its tier `1`, not `0`). Added `int score` to `DevGroup` (max member sigdb score, captured
  in `buildGroups` like `tier`); `countCategories()` now also returns `scoreMax[CAT_N]`.
- CYD-only, no protocol/webapp change (the web app already uses `CAT_COL`). Build SUCCESS, **flashed
  COM14**. **Needs a quick on-device eyeball** (label/badge fit on the 80px tiles, hue contrast on the
  TFT) — flag any tweak. Row tier colours (suspect/likely/confirmed) were intentionally left as-is.

## Done — Remote-ID increment 3: CYD detail UI + NDJSON `rid` line (2026-10-11, branch `remoteid-ui`, CYD FLASHED COM14)
Surfaces the decoded Remote-ID the CYD already stores in `g_rids[]` (from increment 2). **CYD-only, no
protocol/C5/DETS/web change** (`git diff --stat` = only `src/cyd/main.cpp`; PROTOCOL_VERSION still 8).
- **Detail UI** (`buildDeviceDetail()`): after "More UUIDs", when a stored `RemoteId` matches the MAC
  (`ridByMac()`), an orange `-- Remote-ID --` block — Drone ID, **Drone lat / Drone lon** (split rows:
  a combined "lat, lon" overflows `addField`'s 19-char line — my follow-up tweak), Height, Geo alt,
  Speed, Heading, Operator, **Oper lat / Oper lon**; each shown only when known (value sentinels:
  lat/lon 0, alt/height -1000, heading 361, empty string — NOT the `have` bits, which live in the
  C5-only `remoteid.h`). Reused by the scanner detail AND the category drill-down.
- **NDJSON `rid` line** (`logRidEvents()`, called after `logNewDetections()`): per-emitter,
  rate-limited ~once/30 s (reuses `EVENT_LOG_GAP_MS` + a MAX_RID-slot ring):
  `{"epoch","ms"[,"lat","lon"],"evt":"rid","mac"[,"uas"][,"dlat","dlon"][,"h"][,"alt"][,"spd","hdg"][,"olat","olon"][,"oid"]}`
  — overflow-safe `lnAppend` into a 512 B line; `uas`/`oid` JSON-escaped; unknown fields omitted.
- **Removed** the temporary `Serial.printf("[RID]…")` from `requestScan()` (store branch intact).
- **Build** SUCCESS (CYD Flash 44.8% / RAM 33.3%; c5 sanity build also SUCCESS). Reviewer: no blockers
  (bounds/format/escape/ring/sentinel all pass; CYD-loop-task only, no race). **CYD FLASHED COM14.**
- **On-device verify still TODO** (I can't drive the touchscreen or read the SD remotely): with the
  `tools/odid-tx` S3 still transmitting, open the drone's detail on the CYD to see the `-- Remote-ID --`
  block, and download a session (web app) to confirm `"evt":"rid"` lines. **Not committed/pushed yet.**
- **Next:** increment 4 — DETS stream RID fields + web-app Map drone/operator plotting.

## Done — Remote-ID increment 2: link frame + CYD store (2026-10-11, branch `remoteid-decode`, BOTH BOARDS FLASHED v8 + VALIDATED END-TO-END)
Increment 2 of the ASTM F3411 decode (HANDOFF outstanding #5; plan in
[docs/remoteid-decode.md](docs/remoteid-decode.md)). The decoded Remote-ID now crosses the link.
**Validated 2026-10-11 against a live OTA broadcast** from the new `tools/odid-tx/` bench transmitter
(a spare ESP32-S3 impersonating a drone): the C5 prints `[RID] … LOC lat=40.712800 lon=-74.006000
h=50m alt=120m spd=500cm/s hdg=45` (increment-1 decode) AND the **CYD** prints
`[RID] A4:CB:8F:DA:45:B1 uas=TEST-DRONE-0001 lat=40.712800 lon=-74.006000` — proving the whole chain
(S3 broadcast → C5 decode → `Reply::RemoteId` v8 frame → CYD store keyed by MAC), with the `uasId`
string + int32 coords round-tripping intact.
- **Protocol → v8** (`lib/link_protocol/link_protocol.h`): `Reply::RemoteId = 0x91` + a packed
  **75-byte `RemoteId` struct** (mac/have/idType/uaType, drone + operator lat-lon int32·1e7,
  height/altGeo int16 m, speed cm/s + heading deg, uasId[21], operatorId[21]) with a
  `static_assert(sizeof==75)`; `PROTOCOL_VERSION` 7→8, history comment updated. **Both boards must
  flash together — the FrameParser rejects a version mismatch, so a v8 board won't talk to a v7 board.**
- **C5** (`src/c5/remoteid.*`, `src/c5/main.cpp`): `toLinkStruct(RidEntry&, RemoteId&)` maps the decode
  table → wire struct in one place; `streamTable()` emits one `Reply::RemoteId` per active emitter
  **inside the existing burst** (between the opening `Status(scanning=1)` and closing `Status(0)`,
  same `delay(2)` pacing as the Detection loop) — the synchronous request/response contract is
  preserved (RID frames are non-terminating; the CYD still ends on the closing Status or the 250 ms
  idle-gap backstop).
- **CYD** (`src/cyd/main.cpp`): bounded `g_rids[MAX_RID=8]` store keyed by MAC (replace-on-match /
  append / evict-index-0), reset alongside `g_detCount=0` each poll; new length-checked
  `Reply::RemoteId` parse branch in `requestScan()`. Ends with a **temporary, clearly-commented serial
  `[RID]` readout** (mac + uasId + drone lat/lon) to verify frames cross the link — **increment 3
  removes it** when the real detail UI lands.
- **Race fixed (introduced by this increment):** the emit loop read the C5 RID table from the loop
  task while the NimBLE task mutates it via `ingest()`/`expire()` (the detection table has `g_mux`;
  the RID table was "single-task"). Could emit a torn RID frame — benign serial-only now, a landmine
  for increment 3's UI/log, so closed now (C5-only): replaced raw-pointer `count()`/`at()` with
  `int snapshot(RemoteId* out, int maxOut)` that copies under a **FreeRTOS mutex** (not a spinlock —
  `ingest()` does `Serial.printf`); `ingest()`/`expire()`/`snapshot()` share the lazily-created lock;
  `streamTable()` snapshots into a local `ridBuf[8]` then streams outside the lock.
- **Builds clean both envs** (tester): CYD Flash 44.7% / RAM 33.2%; C5 Flash 45.8% / RAM 24.0%
  (+~1.6 KB flash, +24 B RAM, +~600 B transient loop stack for `ridBuf[8]`). Reviewer: mergeable, no
  blockers (packing/assert, version consistency, 0x91 no collision, sync contract, store bounds-safe;
  worst-case burst ~<1 s, inside the CYD's 5 s timeout). **BOTH BOARDS FLASHED v8 2026-10-11** (C5
  COM18 `303A:1001`, CYD COM14 `1A86:7523`, battery/5V off during flash) and validated end-to-end
  (above). The temporary CYD `[RID]` serial line stays until increment 3's real UI replaces it.
- **Bench transmitter added — `tools/odid-tx/` (new, committed).** A standalone PlatformIO project
  (own env, NOT in the root `platformio.ini`) that makes a **spare ESP32-S3** broadcast BLE4 legacy
  ODID adverts (`0xFFFA` service data, `[0x0D][counter][25-byte msg]` framing, one message/advert,
  rotating BasicID→Location→System→OperatorID under one stable MAC). Clean-room; no attribution
  needed. Flash the S3 via its NATIVE USB (`303A:1001`) — **identify by chip type first** (shares
  VID:PID with the C5 native port; `esptool chip-id` → ESP32-S3; the test rig's S3 was COM12).
  `pio run -d tools/odid-tx -t upload --upload-port COMxx`. See its README.
- **Next:** increment 3 (CYD detail UI + NDJSON `rid` line — can consume the snapshot with no further
  locking) then increment 4 (web Map drone + operator plotting).

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

## Webapp cache / deploy-propagation (updated 2026-10-24)
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
- **`style.css?v=<token>` AND `js/main.js?v=<token>`** (one shared token, since 2026-10-24) — **bump it on
  every webapp deploy** so a refreshed HTML pulls a fresh stylesheet AND entry module. Without the main.js
  token, the 2026-10-24 Home-card change shipped a new page with a cached old main.js (dead click handler).
  Modules that main.js imports still can't carry a token (static-import URLs are fixed, no build step) and
  ride the 10-min revalidation — so new DOM wiring that must work immediately belongs in main.js or a NEW file.
- To force-fresh on the phone now: open the app with a throwaway query (`…/webapp/?r=1`): a new HTML URL
  skips the cached page, and its versioned main.js/css come fresh. Reopening the tab does NOT clear the
  cache; otherwise wait ~10 min after the Pages build.

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

Device state: **both boards FLASHED to protocol v10** (2026-10-14, onboard-GPS time → `Status.gpsEpoch`;
`sizeof(Status)`=28) and link-verified up (`table=55` dual-band). They interoperate on v10; a v10 board
rejects any older peer, so keep them in lockstep on future protocol bumps. The spare ESP32-S3 (COM12,
MAC `a4:cb:8f:da:45:b0`) runs the ODID bench transmitter.
**Latest flashes (2026-10-14 this session, all on `main`):** **BOTH boards flashed to protocol v10**
(onboard-GPS time fallback clock — C5 COM18 `303A:1001` chip-id-confirmed ESP32-C5, CYD COM14
`1A86:7523`). The **CYD was reflashed several more times** afterward (all CYD-only, protocol stays
v10): STATUS `tsrc`, the HOME touch-lock readout fix, and the evil-twin SSID/BSSID event enrichment.
Earlier in the same session (CYD-only, no
protocol change): the COINTEL-FLO rebrand + boot splash, the GPS status-bar glyph + tap modal, the
status-bar icon spacing, and the magenta `#AA00AA` accent. History: v8 (Remote-ID inc 2, 2026-10-11),
v7 (multi-UUID), earlier CYD camera OUIs + C5 promiscuous fixes (2026-10-10). Ports THIS session: CYD
= CH340 `1A86:7523` = COM14; C5 native = `303A:1001` = COM18 (flash); C5 UART bridge `2E3C:5740` =
COM8 (does NOT flash); spare S3 native = `303A:1001` = COM12 (shares VID:PID with the C5 native —
disambiguate by `esptool chip-id`). Ports drift; always identify by VID:PID + chip type.
**Pending field validation:** (a) ~~Wi-Fi client/camera capture~~ — DONE (#0); (b) the follower RPA
guard / follower banner — **STILL UNTESTED in the field (see #6).** A drive on 2026-10-14 did NOT
exercise it: the device was on the CYD HOME screen and the phone on the webapp **Home** page, and the
follower banner renders **ONLY on the Scan/Scanner page on BOTH platforms** (CYD: drawn inside
`drawScanInfo()`; webapp: `#folBanner` lives inside `<section id="page-scanner">`, a hidden `.page`
when Home is shown). So there was nowhere for it to appear. Re-test from the **Scan page** (CYD) /
**Scanner page** (webapp). (c) ~~Remote-ID decode + web map~~ — DONE/VERIFIED (web map plots the bench
S3's hardcoded NYC operator fix, #5); (d) **onboard GPS time (v10)** — ~~DONE: user-confirmed working
(2026-10-14), status-bar clock populates + real epochs~~ (#10).

## PICK UP HERE — outstanding

0. **~~FIELD RE-TEST of Wi-Fi client / camera capture~~ — DONE (2026-10-11, user-confirmed).** On a
   walk the scanner picked up **most/all of the cameras walked past**, so the promiscuous-starvation
   fix + both-direction data-frame capture + 2.4 ch 1–11 + 5 GHz sweep are validated in the field.
   Residual gaps remain by design (not regressions): **DFS 5 GHz channels (52–144) not swept** and
   **dormant battery cameras RF-silent until motion**. If a specific known 5 GHz camera is ever missed,
   the next levers are adding DFS channels and/or lengthening the 5 GHz window.
   - **2026-10-14: Axon detection confirmed in the field.** Driving past police at a blocked-off road
     construction site, the scanner flagged Axon gear well (sigdb Axon category). Another positive
     real-world confirmation of the signature matching.

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
4. **~~Pages deploy + in-browser smoke test of the redesigned webapp~~ — effectively DONE
   (2026-10-11, user-confirmed).** The 2026-10-05 GitHub Actions `degraded_performance` backlog
   cleared, the live webapp serves current, and the user exercised the redesigned 5-page UI on the
   phone and reported it **working** (no unpushed webapp version outstanding). Treat the redesign as
   field-confirmed; file a fresh item only if a specific page/control misbehaves. Re-verify a deploy
   anytime with `gh api repos/negativeaffirmations/cyd-scanner/pages/builds/latest --jq .status`.
5. **~~Remote-ID DECODE (ASTM F3411)~~ — COMPLETE (increments 1–4, 2026-10-11).** Landed across
   branches `remoteid-decode` (inc 1–2, merged to `main` `487fd96`) and `remoteid-ui` (inc 3–4). Full
   end-to-end: C5 decodes → v8 link frame → CYD store + detail UI + NDJSON `rid` → DETS v6 → web-app
   map (drone + operator plotting). **FULLY VERIFIED incl. the web map (2026-10-14):** the owner saw the
   drone/operator plotted in-browser against the bench S3 (the marker sits in NYC = the S3's hardcoded
   test coords 40.7128,−74.0060 / 40.7130,−74.0062, NOT an internet/VPN location — the S3 isn't
   networked and the web app plots strictly `rid.olat/olon`). Only residual idea: **BT5 extended-scan
   (Long-Range ODID packs)** — a later, measured change (check the BLE/15.4 coexistence rule first).
   Design + increment log in [docs/remoteid-decode.md](docs/remoteid-decode.md).
   - **Increment 1 DONE (2026-10-11, C5 FLASHED COM18, no protocol change):** `src/c5/remoteid.*`
     clean-room decodes the OpenDroneID messages (Basic ID / Location / System / Operator ID, incl.
     Message Pack + the BLE `0x0D` legacy framing) from the `0xFFFA` BLE service data already captured
     in the scan callback; accumulates per emitter MAC in a bounded TTL table and **prints decoded
     UAS ID + drone/operator lat-lon + height/speed to the C5 USB console** (`[RID] …`). Link stays
     **v7** (both boards interoperate) — bench-test with an ODID transmitter/sim and watch the serial.
   - **Increment 2 DONE (2026-10-11, BOTH BOARDS FLASHED v8 + VALIDATED):** `Reply::RemoteId` (0x91) +
     a 75-byte `RemoteId` struct → **PROTOCOL_VERSION bumped 7→8 (both boards flashed together)**;
     the C5 emits one per emitter inside its scan-dump burst (mutex-guarded `remoteid::snapshot()` — a
     cross-task race introduced by the emit loop was found + fixed), the CYD stores it keyed to the
     Detection by MAC (`g_rids[8]`) with a temporary serial `[RID]` readout. Confirmed end-to-end
     against the new `tools/odid-tx/` S3 transmitter: C5 + CYD both print matching `[RID]` lines. See
     the "Done — Remote-ID increment 2" entry above.
   - **Increment 3 DONE (2026-10-11, CYD FLASHED, user-confirmed on-device):** `-- Remote-ID --` block
     on the device-detail screen + per-emitter `"evt":"rid"` NDJSON line; temp serial readout removed.
   - **Increment 4 DONE (2026-10-11, CYD FLASHED; webapp static-verified):** DETS v6 RID block
     (CYD→phone) + web-app Remote-ID section and drone/operator map plotting (live + logged). See the
     two "Done — Remote-ID increment 3/4" entries above.
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
   - **2026-10-14: STILL UNTESTED — the follower banner is Scan-page-only on BOTH platforms.** Verified
     in code (not a bug): the CYD banner is drawn inside `drawScanInfo()` (Scan screen only) and the
     webapp `#folBanner` sits inside `<section id="page-scanner">` (hidden on any other page). The
     background follow detector + the CYD **magenta LED** (`applyScanLed()`) run headless on any screen,
     but **no on-screen banner shows on either HOME page.** A user drive with the CYD on HOME + the
     phone on the webapp **Home** page therefore produced no banner (expected). Re-test must be done
     from the Scan/Scanner page. **Open follow-up (offered, not yet built): surface the follower +
     deauth/evil-twin alerts on the HOME pages too** (CYD HOME banner/indicator + webapp Home follower
     indicator driven by STATUS `fol`/`deauth`/`evil`) so alerts are visible when not on the Scan page —
     which is exactly when you'd be driving/walking. Also still open: whether a given drive *should*
     flag anything depends on a stable non-RPA emitter moving with you ≥400 m — the own phone is
     excluded by design.
7. **~~P3 log follow-ups~~ — DONE (2026-10-11, CYD FLASHED).** (a) **SUPERSEDED 2026-10-22** by the LRU seen
   table (no skip, no rotation — see Latest; the skip caused data gaps + multi-file drives). Original: **Dense-area thrash guard:** when a
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
9. **~~Battery %/charge status-bar indicator~~ — BUILT (voltage) + charge-detect DROPPED.** The
   **voltage** indicator is live (`src/cyd/power.*`, GPIO35 100k/100k divider, status-bar glyph + web
   `bat=`; CYD-only, no protocol bump). **Charge-detect is NOT FEASIBLE** on this TP4056 module — the
   extra pads the owner found are the DC **input +/−**, not a CHRG/STDBY status pad — so there's no
   `chg=` key and **GPIO26 stays free**. See [[battery-indicator-plan]] / [[battery-power-subsystem]].
10. **~~Onboard GPS time (v10) — FIELD-VALIDATE~~ — DONE (2026-10-14, user-confirmed working).** The
   GPS-time fallback clock was tested outdoors and works: with a real fix the status-bar clock
   populates (+ turns cyan, `tsrc=2`) and logged rows carry real epochs instead of `0`. (Behaviour
   reminder: before any phone has *ever* synced the fallback reads **UTC** — no tz; after the
   first-ever phone sync the tz offset persists in NVS `cydscan/tzoff` and it reads LOCAL.)
11. **Remote-server upload to scan-analyzer — contract v0; WEB-APP CLIENT BUILT (2026-10-16).**
   **2026-10-23:** PRB rows now carry `flags` `data`/`dl` + `bssid` (link v11, see Latest); live-POST carries
   the `data`/`dl` flags only. Agreed with scan-analyzer; no server change needed.
   **2026-10-22:** "Upload selected" is now multi-select over grouped scans — one joined body per scan
   (`uploadSelectedSessions` in `features/sessions.js`, replaces `remote.js` `uploadSelectedSession`); meta now
   carries `scanner_name`; `fw` 0.10.0. scan-analyzer confirmed no server change.
   The CYD↔scan-analyzer API contract is locked at **DRAFT v0, owner-approved**. **Owner greenlit the
   build** (2026-10-16): scan-analyzer is implementing the server endpoints (bridge 11:19) and
   cyd-scanner's **web-app client is now built** (this session) against v0 — no shape changes.
   Authoritative full spec: scan-analyzer `docs/api-contract.md`.
   **LANDED (web app, `webapp/js/features/remote.js` + Settings "Remote server" card + Data "Upload
   selected to server"):** all three client paths, all to the user's OWN private server.
   - **Pairing:** `handlePairHash()` reads a QR deep link `…/webapp/#server=<enc>&token=<token>` (the
     `#` fragment, cleared from the URL right after via `history.replaceState`), `POST /api/v1/pair
     {token}` → stores `{key,device_id}` in `localStorage` (`rs.server`/`rs.key`/`rs.deviceId`); the
     Settings paste field (`pairFromForm`) is the fallback. Key/token are never written to the log.
   - **File upload:** `uploadSessionText()` POSTs the raw session NDJSON to `/api/v1/uploads`
     (`application/x-ndjson`, `text/csv` for legacy), Bearer key, `X-Session-Name`/`X-Client-Tz`
     headers. Wired via a new `state.dl.sink==="upload"` branch in `ble/download.js` — "Upload current
     session" (`L`) and "Upload selected to server" (`F:<path>`) both fetch over BLE then upload.
   - **Live-POST (opt-in, off by default):** `maybePostLive()` in `renderDets()` batch-POSTs each scan
     snapshot to `/api/v1/detections` as reduced log-shaped NDJSON (`live:1`; omits
     ch/ie/cid/uuid/pan/score/sig; `lat`/`lon` = the phone's own fix; `epoch` = LOCAL-encoded to match
     the file rows). Gated by `rs.live` localStorage toggle + paired + scanning + connected; in-flight
     guard + first-failure-only logging. (A background browser tab can't hold the link — hence opt-in.)
   - `uiver` bumped to `2026-10-16 · remote-server`. No `style.css` change (reused existing classes),
     so no `?v=` bump. **DEPLOYED + VALIDATED END-TO-END** (committed d32bdc5, pushed to Pages
     2026-10-16). scan-analyzer validated the server contract path over the live HTTPS tailnet by its
     own curl smoke (pair 200 / uploads 200 / detections accepted→deduped / revoke→401), AND the owner
     ran the real **phone live pass** on Chrome/Android + the CYD: QR pairing (#fragment token→key,
     cleared from URL) worked, BLE connected, and **live-POST streamed to /detections, all 200 OK**
     over the direct-LAN tailnet hop (829 sightings / 36 MACs / BLE+5G+2.4+PRB, all with the phone's
     GPS fix). Reduced-row schema confirmed on the server (ch/cid/uuid/pan/sig/score omitted, lat/lon =
     phone fix, local-encoded epoch + epoch-ms, session+ms dedup holding). **File upload also
     validated on real hardware (2026-10-16, upload-ux build):** "Upload selected to server" pushed two
     sessions, both fresh ingests (uploads #5/#6 = 235/236 sightings, duplicate:false), toasts showed
     the real counts + the "Fetching over BLE…" feedback. **All three endpoints (/pair, /uploads,
     /detections) + the client UX are now validated end-to-end — the integration is COMPLETE.**
   **Upload/download UX fix (2026-10-16, follow-up):** the first file-upload run landed 200 server-side
   but the owner saw no client feedback (the Activity log is Home-only; the buttons stayed on the
   Data/Settings page). Added a page-agnostic **toast** (`util/dom.js` `toast()` + `#toast` element +
   `.toast` SCSS) and wired optimistic "Fetching over BLE…/Uploading…" + result ("N sightings / M
   events / duplicate") + error toasts into both upload buttons (which also disable on tap via
   `setUploadBusy`) AND the plain Download-current/Download-selected paths (owner flagged the same
   lag). `style.css` rebuilt (`?v=20261016a`), `uiver` → `2026-10-16 · upload-ux`.
   **DONE + VALIDATED GREEN 2026-10-16 — scan-provenance `evt:meta` record (owner greenlit; CYD BUILT +
   FLASHED, webapp DEPLOYED to Pages, server side built by scan-analyzer-cb; end-to-end provenance pass
   passed on real hardware — see "Validation" below. Committed cyd-scanner `737649a`; scan-analyzer
   `8a14e34`/HANDOFF `92689fa`).** So a scan's origin
   survives whoever uploads it (today the server only
   records the uploader's bearer→device_id, i.e. the phone, not the scanner). The CYD firmware emits a
   per-session metadata line, one per session near the top of each log stream (fg + bg) mirroring the
   `timesync` anchor, AND the web app includes it in each live-POST batch:
   `{"evt":"meta","scanner_id":"<eFuse-MAC hex>","fw":"<FW_VERSION>","db_gen":<DB_GEN>,"scanner_name":"<opt>"}`
   - `scanner_id` = `ESP.getEfuseMac()` as 12-char lowercase hex (stable, unique, already used for the
     SoftAP). It's the **base** MAC — an opaque key, won't equal any row `mac` (BLE/Wi-Fi MACs derive
     from it with offsets).
   - `db_gen` = the existing `DB_GEN` constant (main.cpp, currently 5). `fw` = a short `FW_VERSION`
     string to ADD (none exists today — we have PROTOCOL_VERSION for the link + DB_GEN only).
     `scanner_name` = optional/blank for v1 (no per-unit label yet; make it an NVS-settable phone label
     later). Keep meta minimal — NO gps-source/hw-rev (point-in-time / untracked).
   - **Live-POST provenance:** CYD adds `sid=`/`fw=`/`dbgen=` to the **STATUS** characteristic
     (CYD→phone); the web app prepends a matching meta record per batch. **No CYD↔C5 link-protocol
     (PROTOCOL_VERSION) change** — all CYD-side + web-app.
   - **What shipped (2026-10-16):** CYD firmware — `FW_VERSION="0.9.0"` + `scannerIdHex()` (eFuse MAC,
     12 lc hex, computed once) + `writeSessionMeta()` writes the meta line as the FIRST line of every
     stream in `ensureLogFile()` (fg + bg; before the first row, like the timesync anchor), and
     `pushStatus()` now appends `sid=`/`fw=`/`dbgen=` (STATUS buffer widened 416→512). `scanner_name`
     OMITTED for v1 (no label yet; server column is nullable). NO PROTOCOL_VERSION bump (CYD-only; C5
     untouched, not reflashed). Build: RAM 34.2%, Flash 45.9%. Web app — `parse.js` skips `evt:meta`
     (both parsers); `ble/status.js` reads `sid/fw/dbgen` into `state.scanner{Id,Fw,DbGen}`;
     `features/remote.js` `metaLine()` prepends a meta record to each live-POST batch (omits unknown
     fields); `state.js` holds the three fields; `#uiver` → `scan-provenance`. **Webapp DEPLOYED to
     GitHub Pages** (both provenance paths live: file-upload carries the CYD's own meta line 1; live-POST
     prepends the meta record per batch).
   - Server columns are scan-analyzer's side, stored DISTINCT from uploader device_id. Their server was
     built to this shape (migration v5, meta parsed on upload + live paths w/ backfill,
     `GET /api/v1/sessions`) and is FORWARD-COMPATIBLE (ignores meta's absence). Privacy: first-party
     (owner's own device), no bystander PII.
   - **Validation (2026-10-16, real hardware, scan-analyzer-cb):** end-to-end GREEN on BOTH paths. A real
     phone upload landed server session 20 (file-upload) + session 19 (live-POST), both carrying
     `scanner_id=b817f4f4e9d4` / `fw=0.9.0` / `db_gen=5` (= this CYD's eFuse-derived id), `scanner_name`
     NULL, kept DISTINCT from the uploader `device_id=cyd-fb861c94`. Our per-batch live meta prepend AND
     the server's backfill both verified; `scanner_id` reads opaque (matches no scanned `mac`);
     pre-firmware sessions correctly stay NULL; integrity clean. **No shape changes — shipped +
     validated on both projects.** scan-analyzer's dev server is staying UP for continued testing.
   Original client-side spec (unchanged, for reference):
   - **Pairing:** scan-analyzer shows a QR → the web app opens with a URL **`#fragment`** (NOT a query
     string, so the token never reaches GitHub Pages logs): `…/webapp/#server=<url-enc>&token=<token>`.
     The web app reads `location.hash`, stores `server` + the exchanged key in `localStorage`; a
     **Settings "Remote server"** paste field is the fallback. Exchange: `POST <server>/api/v1/pair
     {token}` → `{key}`; thereafter `Authorization: Bearer <key>`.
   - **File upload:** `POST /api/v1/uploads`, raw `application/x-ndjson` body (`text/csv` legacy),
     metadata via `X-Session-Name` / `X-Client-Tz` headers, Bearer, sha256-idempotent.
   - **Live-POST (Phase 3):** `POST /api/v1/detections`, batched **reduced** NDJSON — the phone web app
     can't produce `ch`/`cid`/`uuid`/`pan`/`sig`/`score` (those optional), dedup by session+`ms`.
   - **Constraints:** server MUST send CORS + preflight `OPTIONS` for
     `https://negativeaffirmations.github.io`, and MUST be HTTPS (mixed content). Log-schema semantics
     are locked; **DETS is NOT ingested** by the analyzer. Reserved future `src` tags:
     `BTC`/`OPT`/`CAM`/`VISION`.
   - **Near-term (no build):** uploading is just download-a-log-then-upload-the-file via
     scan-analyzer's own UI.
   **Doc discipline while building (both projects):** update THIS item + the bridge incrementally each
   work session (what's built / what's next), so a fresh session can resume mid-build if one runs out
   of context/usage. See the top "New direction" + CLAUDE.md "Companion project + remote-server upload".

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

# Phone-side recording and threat analysis

Status: **phase 1 deployed, phase 2 built** (2026-11-23).

## Goal

Make the phone the place where scan data is recorded and analyzed, while every scanner keeps
working on its own:

1. **Record live.** The phone holds an up-to-date copy of every log the connected scanner is writing
   (on a fleet leader, the fleet file with the members' lines too). Download, Map and Upload of the
   current scan then need no transfer at all.
2. **Analyze on the phone.** The phone runs its own threat analysis over that data, with the memory,
   storage and time window the scanners don't have, and shows its verdict next to the scanner's.

The scanners keep their detection, alerts and SD logging exactly as they are. With no phone
connected nothing changes; with a phone connected the phone adds a second, better-informed opinion.

## What the phone gets today

| Path | What | Limits |
|---|---|---|
| DETS (notify, each ~2 s cycle) | the top 12 devices of the scanner's merged view | 12 rows; a subset of fields; no history |
| STATUS (notify) | counts, alert counts, position, session name | aggregates only |
| LOGDATA downloads (`L`, `F:`, `F:…@offset`, `T:`) | whole log files or their new tail | on request; ~13 KB/s |

So the phone currently sees *less* than a scanner does. A fleet leader already holds every member's
merged rows in RAM and every member's log lines in its fleet file (`/logs/fleet-*.jsonl`); the phone
only gets them by downloading.

## Bandwidth

A scanner's log grows by about 1-2 lines a second on a drive (a few thousand lines an hour,
~0.2-0.5 KB/s). LOGDATA moves ~13 KB/s. Streaming the log as it is written costs a few percent of the
link, including a leader's fleet file.

## Phases

### Phase 1: live mirror, phone only (no firmware change)

`webapp/js/features/livesync.js` (new file), with `webapp/js/ble/syncwait.js` for sharing the link. While a
scanner is connected and authenticated, and its firmware can send tails (fw >= 0.26.0):

- **Tails only.** Each pass fetches the **new bytes** of the scanner's current session file
  (`F:<path>@<offset>:<file_id>:<crc>`, the existing tail request) and appends them to the phone's log cache
  (`data/logcache.js`, IndexedDB). On a **fleet leader** it does the same for the live fleet file (every member's
  lines). Interval by file size: 20 s under 2 MB, 1 min under 10 MB, 5 min above.
- **Its own requests.** It does not use the general fetch path, which falls back to a whole-file download on any
  problem. A refusal or an odd reply ends the pass, and the next one waits longer (20 s doubling to 5 min).
- **Which file is which** (path, `file_id`, size) comes from a quiet session list (`Q`; the Data and Explore UI are
  not touched). `Q` blocks the scanner while it reads every log's head and tail, so it is asked only on a real
  change: the first pass, a current file name it hasn't seen (New Session, time-sync rename, fg/bg switch), a
  refused update, the fleet's member count changing. A file that doesn't exist yet is re-checked with backoff.
- **Whole-file copies are bounded.** A file is copied whole once, automatically only up to 1 MB (about 80 s over
  BLE; the size is re-checked right before, and only then). Bigger files show a "Copy now" button with the size and
  time; files over 20 MB are not kept live. A copy that failed, or was lost in this connection (Clear saved logs,
  eviction), is offered again rather than re-made automatically. The scanner can't abort a file once it is sending,
  so while a whole-file copy runs, a user action waits for it (with a toast) instead of failing.
- **It yields.** Before each request it stops if a user action is waiting (`syncWait()` counts waiters) or a user
  transfer is running; it never starts while LOGDATA is busy, never during the Wi-Fi download, and stops on
  disconnect. Requests that are answered with a list (session list, wipe-all, whitelist) claim LOGDATA through
  `claimWrite()`; when it is busy a session list / wipe refuses with a message, a whitelist add or remove is still
  sent (the scanner applies it after the transfer).
- **Fleet file choice:** the newest fleet file dated within the last 15 min, else the highest-named undated one (a
  new file before any member row with a time); re-checked only while a member is joined.
- **It stops** if the phone can't save the copy (storage full) and says so.
- Data page: a switch "Keep a live copy of the current scan on this phone" (default on, saved on the phone) with a
  line saying how current the copy is.
- Because the cache is the one Download / Upload / Map / Explore use, those actions find the current file saved
  and only top up the last few seconds.
- Works in the browser and in the Android app; in the app the stage-2 background service keeps the page running
  with the screen off.

Known costs: each pass re-saves the whole file Blob in IndexedDB (why the interval grows with size and the cap is
20 MB; phase 2 removes it), and a phone that is not connected records nothing (the scanner's SD log is still the
record).

### Phase 2: push stream (firmware 0.44.0, built)

The scanner pushes every log byte to the phone as it writes it, instead of the phone polling for tails:

- **Subscribe.** Once the phone's copies are current it sends CMD `J:<path>@<off>:<file_id>:<crc>[|<path>@…]` (the
  current session file, plus the live fleet file on a leader; at most 2). The scanner checks each exactly like a tail
  request (same `file_id` + prefix-CRC check) and answers one LOGDATA notify: `LLOK=<n>`, or `LLERR=parse|path|id|off|many|none`
  with nothing subscribed. A bare `J` stops; a new `J:` replaces the set.
- **Frames.** From then on, while no BLE transfer runs (never inside another reply), it sends single LOGDATA notifies
  `LL=<file_id>:<offset>:<bytes>`, up to 4 per file every 250 ms, each as big as the link's MTU allows. It reads the
  bytes from the card each round instead of queueing them in RAM, so a busy link or a full notify buffer only delays
  them. A file renamed on time sync is followed by its `file_id`. The subscription ends with the authenticated session.
- **Phone side** (`features/livesync.js`): the frames collect in RAM and are saved into the copy on each pass (or once
  256 KB are waiting), only the part past the copy's current size (a user download may have fetched a tail
  meanwhile). An offset gap, a lost copy or a refusal ends the push; the phase 1 tail updates take over, and it
  subscribes again on the next pass (after a minute, or 10 min when the scanner never answered: older firmware or a
  stale page module). New Session or a fleet file rotation changes the set of files: the phone stops the push, brings
  the new files up to date, and subscribes again.
- **For phase 3:** every pushed piece is announced as a window `cflo-livelog` event `{sid, path, fid, off, bytes}`.
- Old web apps never send `J`, so they never get frames.

Still to do: storing the copy in chunks instead of re-saving the whole file (the re-save cost is why the save interval
grows with size), and a STATUS key for the leader's live fleet file (STATUS part 2 is at its size limit, so the
session list still finds it, asked only on a real change).

### Phase 3: phone analysis engine

**Increment 3a (built):** `webapp/js/analysis/engine.js` keeps one record per device for the whole current scan and
every fleet unit, fed by the live copy's `cflo-livelog` pieces (pushed, tail updates and whole copies alike; the start
of a file is read from the saved copy). Whole-scan follow: sightings grouped into places 300 m apart; 3+ places spread
over 1+ km across 10+ min = "following"; "stayed in one place while you moved N km" when it didn't move with you
(movement counts only past 60 m steps, so GPS jitter while parked adds nothing). The device popup shows it as "Phone
analysis" (`features/phoneview.js`, reaching the engine through `state.engine`: a plain import would load a second,
empty instance next to main.js's `?v=` one). Checked offline on a real fleet drive (no follower, matching the scanners)
and a synthetic tracker (found).

**Increment 3b (built):** `webapp/js/analysis/history.js` reads every log saved on the phone (all scanners, fleet files
included) once per page load, in the background a file at a time, and keeps per device the days seen, places (300 m),
first / last seen, names, and the evil-twin / deauth events that named it. The popup's "Saved scans on this phone" shows
days seen, first seen, places, whether it was seen at its current place before ("seen here on 9 days"), and its past
alerts (both XNVV4 routers list the 2026-10-22 and 2026-11-23 evil-twin events). Reached through `state.history`.

**Increment 3c (built):** `webapp/js/analysis/alerts.js` raises the phone's own alerts every 15 s, through the existing
notification settings (`features/notify.js` exposes `state.notifyShow`: same switches, permission, style, quiet time):
- **Following you (phone analysis):** a device the whole-scan analysis calls following. Skipped when whitelisted, when it
  is a rotating BLE address with no signature or detector flag (phones / watches), or when the saved scans show it on
  3+ earlier days with no signature (it travels with you every day: your own car or phone).
- **Evil-twin / deauth alert seen here before:** a new alert line in this scan whose devices set off the same alert on
  an earlier day, "probably a fixed setup, not an attack". The scanner's own alert still fires as before.

**Increment 3d (built):** `tools/vendordb/build.py` builds `webapp/data/oui.tsv` (40,320 IEEE MA-L prefixes) and
`webapp/data/cid.tsv` (4,082 Bluetooth SIG company IDs); `webapp/js/analysis/vendors.js` fetches them the first time a
device popup needs them (~1.3 MB, not on page load) and the popup shows "Maker" and "BLE company". Random (locally
set) addresses have no maker. Sources and licences: docs/references.md.

Next: fleet-wide correlation views (which units heard a device, when, from where), and using the maker lists in the
follow / "your own device" decisions.


`webapp/js/analysis/` (new). Runs over the mirrored lines as they arrive (and over any loaded scan):

- **Same rules, bigger tables.** A JavaScript port of `sigdb` scoring that reads the scanner's own
  `/signatures.csv` (fetched once over the file explorer, so phone and scanner never drift), plus the
  complete IEEE OUI registry and the Bluetooth SIG company-ID list bundled with the app. The scanner
  is capped at 96 OUI + 64 name rules; the phone isn't.
- **Whole-drive memory.** Follow detection over the whole session instead of the scanner's last few
  minutes (a device seen at N places across T minutes), evil-twin and deauth context ("this pair has
  fired here before", "this router drops clients every drive"), using every past scan saved on the
  phone and, when paired, the server's read API.
- **Fleet-wide correlation.** Rows from every unit (`scanner` on fleet lines) in one place: devices
  heard by several units, positions from several viewpoints.
- **Output.** A per-device phone verdict (tier, category, reasons) shown beside the scanner's in the
  live list and device popup ("Scanner: Likely · Phone: benign, known router pair"), and fed to the
  existing notification engine (`features/notify.js`), so phone-only findings can alert.
- The scanner's verdict is never overwritten; disagreements are shown, not hidden.

### Phase 4: denser data where analysis needs it

The SD log holds a device's first sighting, a re-log every 5 min while in range, and a `lastseen`
line when it leaves. That is enough for signature matching but thin for following and position
estimates. If phase 3 shows the need, the phone stream (not the SD log) gets a compact per-cycle
change record (MAC, RSSI, position; only what changed), still well under the link's capacity.

## Decisions

- **Scanners stay self-sufficient.** No detection, alert or log moves off a scanner; the phone only
  adds.
- **One rule file.** The phone reads the scanner's `signatures.csv`; phone-only knowledge (full OUI /
  company-ID lists, history) adds to it and is labelled as the phone's.
- **The SD log remains the record of truth.** The phone copy is a cache of it (byte-identical, keyed by
  `file_id`), so anything built on the phone can always be checked against the card.

## Privacy

The mirrored logs contain bystanders' device identifiers with positions, as the log cache already
does. They stay in the phone's local storage (IndexedDB), are never sent anywhere except the paired
server the owner chose, and the Android app already excludes them from backup. "Clear saved logs"
removes them.

## Coordination with the analysis server

Uploads of a mirrored scan are the same bytes as today's uploads, so phase 1 needs no server change.
Phase 2/3 open a question for the server: whether live upload should carry the mirrored log lines
(full detail, every unit) instead of today's DETS-derived rows. That is a contract change and is
agreed with the server side before it is built.

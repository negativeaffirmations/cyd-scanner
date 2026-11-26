# GATT identification reads ("gattid")

Many BLE devices advertise no name, or only sometimes. The scanner can ask a nearby device what it is by connecting
briefly and reading the standard identification fields every BLE device exposes to any central. This is read-only
and rare per device; see the scope rules in CLAUDE.md ("Scope / ethics / privacy").

## What is read

Read By Type (ATT, one request per UUID, no service discovery), so nothing else is ever touched:

| UUID | Field | Notes |
|---|---|---|
| 0x2A01 | Appearance | read first |
| 0x2A00 | Device Name | skipped when Appearance is a personal category (phone, watch, wearable audio, hearing aid, medical) |
| 0x2A29 | Manufacturer Name | |
| 0x2A24 | Model Number | |
| 0x2A26 | Firmware Revision | |
| 0x2A27 | Hardware Revision | |

Values are cut to the struct sizes minus the NUL: name 31, maker 23, model 23, firmware 15, hardware 15 printable ASCII
characters (anything else becomes `?`).

Never read: Serial Number 0x2A25 or anything else. No writes, no notify/indicate subscriptions, no pairing or bonding:
the C5 build has the NimBLE Security Manager compiled out (`MYNEWT_VAL_BLE_SM_LEGACY=0`, `MYNEWT_VAL_BLE_SM_SC=0`),
so a "needs encryption" answer is recorded (`auth`) and never followed by a pairing attempt.

## Who does it

The C5 (src/c5/gattid.*), with the raw async NimBLE host API (`ble_gap_connect`, `ble_gattc_exchange_mtu`,
`ble_gattc_read_by_uuid`, `ble_gap_terminate`) driven by a non-blocking state machine in `loop()`; never
NimBLEClient (it waits forever on a silent peer).

- **When:** only while the Wi-Fi radio is idle and BLE is wanted by this unit; the BLE scan is paused for the slot
  (`blePaused()`) and the next AP scan waits while a slot runs or a link is open. In the full time-slice (BLE window
  before each AP scan) a slot opens after BLE has scanned >= 1.5 s of the window and only if the worst case
  (stop 0.5 s + 2.5 s hard deadline + 1.0 s disconnect wait + 0.5 s margin = 4.5 s, `gattid::worstSlotMs()`) still fits
  before the AP scan. Fleet plans and `blepause off` (BLE runs beside the AP scan, 3 s gap) have no such window: a slot
  opens >= 1 s after the last Wi-Fi phase ended and the AP scan waits for it; with one attempt per cycle that adds at
  most ~4.5 s once to a cycle of ~24 s (AP scan + both promiscuous windows), ~28.5 s, inside the 30 s table TTL.
- **Order:** connect, then the ATT MTU exchange (preferred MTU 64), then the reads below.
- **Limits:** one connection at a time; 1.0 s per operation; 2.5 s hard deadline from the connect, then terminate;
  1.0 s wait for the disconnect; at most 1 attempt per cycle (the counter also advances when the AP scan is off),
  >= 20 s apart, <= 60 per hour. A link that stays open is re-terminated every 3 s, no new attempt starts, and the C5
  restarts if it is still open 30 s after the first terminate. If a hard deadline cut the reads short with fields never
  tried, the device is retried after 15 min (once per boot) instead of 24 h.
- **Candidates:** a connectable, undirected advert (ADV_IND) heard within the last 1.5 s (no sticky "was connectable
  once"); public or random-static address only (resolvable / non-resolvable private addresses are skipped: phones,
  watches, our own hosts); RSSI >= -75 dBm (-85 for host priority devices); seen >= 2 adverts, first seen >= 6 s and
  >= 2 host polls ago, last seen within 1.5 s.
- **Never connected to:** our own units (CFLO service UUID); Apple (0x004C), Microsoft (0x0006), Samsung (0x0075),
  Garmin (0x0087) and Google (0x00E0) company IDs; Fast Pair 0xFE2C; hearing aids (0xFDF0, 0x1854); health, fitness and
  audio-stream services (0x1808 glucose, 0x181F CGM, 0x183A insulin, 0x1810 blood pressure, 0x180D heart rate, 0xFEBC
  Dexcom, 0x1822 pulse oximeter, 0x1809 thermometer, 0x181D weight, 0x181B body composition, 0x183E activity, 0x1814
  running, 0x1816 cycling speed, 0x1818 cycling power, 0x184E / 0x1850 / 0x184F / 0x1846 / 0x1844 / 0x1853 audio);
  advertised appearances in the phone, watch, thermometer, heart rate, blood pressure, glucose, running, cycling,
  wearable audio, hearing aid, medical (49-55) and outdoor sports categories plus wearable computer (0x0086) and tablet
  (0x0087); and devices the host lists as whitelisted / its own phone. If the Appearance read over GATT is personal or
  medical the link is closed at once, only the appearance (and `nw`) is kept, and the device is never tried again.
- **Whitelist skips:** the host sends private addresses nowhere (the C5 never reads them) and rotates its whitelisted
  BLE devices through 8 of the 10 hint slots, so every one reaches the C5 within a few polls; the C5 keeps each skip
  for 10 min. A host that says reads are off always wins, even over console `gatt on`.
- **Back-off:** a device that answered is not read again for 24 h (in practice once per boot); refused / timed out:
  15 min, 1 h, 4 h, then given up for the boot. History entries with a running back-off, a give-up, a deny or a live
  skip are never evicted; with no evictable slot no new attempt starts. `gatt try <mac>` is one attempt, pending for at
  most 60 s, allowed once per 15 min per MAC, and may clear a give-up (never a deny).
- **Names withheld:** a GATT name under a personal appearance, or containing an apostrophe form (' ` U+2019 U+02BC) +
  s / S followed by the end or a non-letter ("Bob's", "BOB'S-PC"), is not kept (`nw`).

The C5 also remembers the last advertised name of public / static addresses (128 entries) and fills it into later
adverts of the same device that carry none (rows then show it even when this advert had no name). Such a row carries
`FLAG_BLE_NAME_CACHED` (Detection.flags bit 13, not notable: no tier, no category); a real advertised name clears it again.
The detection log row gets `"nc":1` right after `name` (the name MAY come from the cache: the flag stays set until a real
advertised name arrives), and the DETS row says so in `nsrc` (below).

## Link

Additive, no `PROTOCOL_VERSION` change:

- C5 -> host `Reply::DevInfo` (0x93, 124 B): mac, address type, which fields were read (`have`) and which were
  attempted (`tried`, same bits), RSSI of the advert that triggered the connection, appearance, name, maker, model, fw, hw. Sent inside the scan-dump burst while the device is in the live table (first 3 polls, then every 60 s).
- Host -> C5 `GattHint` (64 B) appended to StartScan: enable flag, RSSI floor, up to 10 MACs to skip (own address,
  the phone, whitelisted devices) or to prefer (devices at Suspect tier or above). Without a hint (older host) the
  C5 does no reads.

C5 console: `gatt on|off|auto|save|stat|clear|rssi <dBm>|try <mac>` (`save` keeps auto / off only; `stat` says why reads are idle).
The per-cycle line ends `gatt <attempts>/<ok>/<last slot ms>ms`. Cached results are sent only while reads are on.

## Host and log

Hosts keep the results per MAC (`src/host_common/devinfo.*`), prefer a GATT name for display when no advertised name
exists, show a "Device" block (after Remote-ID) on the CYD detail screen, and log one line per device per log file:

```
{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"devinfo","src":"BLE","mac":"AA:…","at":"public|static"[,"name":"…"][,"app":N][,"mfr":"…"][,"model":"…"][,"fw":"…"][,"hw":"…"][,"nw":1][,"auth":1]}
```

with `"rssi":N` (the advert that triggered the connection) and `"rd":"n,a,m,o,f,h"` (the fields the C5 attempted, in that
order: name, appearance, maker, model, firmware, hardware; a name skipped for privacy counts as attempted, with `nw`)
right after `at`. The full order is
`…"evt":"devinfo","src":"BLE","mac","at","rssi":N,"rd":"…"[,"name"][,"app"][,"mfr"][,"model"][,"fw"][,"hw"][,"nw":1][,"auth":1][,"re":1,"age":S]`.
A line copied into a new log file (New Session, a new file) with values already logged in an earlier file carries
`"re":1` and `"age"` (seconds since the read, fw 0.47.1): a copy, not a new read. A result the C5 flags as already sent (`DI_TRIED_RESENT`, e.g. after a host reboot) is also logged `"re":1`, without `age` (fw 0.47.2).

Detection rows: `name` is still an advertised name; `"nc":1` marks one filled in from an earlier advert of the same device; readers join on `mac`. Phone CMD `C:<0|1>` turns the
reads on or off (NVS `cydui/gattrd`, default on; serial `gatt on|off`); STATUS part 3 `gatt=<0|1>`. DETS v11 appends `nsrc` to each row: `g` when the shown name came from a GATT read (the name, or "maker model"),
`c` when it came from the C5 name cache, empty otherwise; the web app forwards it as `nsrc` on live-POST rows.

## Build notes

`MYNEWT_VAL_BLE_SM_LEGACY=0` / `MYNEWT_VAL_BLE_SM_SC=0` are set in `[env:c5]` and `[env:xiao_c5]` (the map file has no
`ble_sm_pair_initiate`). `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` cannot be set from a build flag (the Arduino core's
sdkconfig.h redefines it to 3), so the single-connection rule is kept by the state machine, not the stack. The C5 sets
the preferred ATT MTU to 64 (it has no GATT server) for the exchange.

# Mini build — staged bring-up test

Wire the mini build **one module at a time** and prove each stage before adding the next. Wiring
reference: [mini-build.md §2.6](mini-build.md) and `hardware/diagrams/mini-build_wiring_diagram_labeled.png`.

**Tools:**
- **`[env:xiao_s3_bringup]`**: a test firmware for the S3 (`src/xiao_s3_bringup/main.cpp`). It
  replaces the host firmware during bring-up and prints a PASS / WAIT / FAIL line per stage every 3 s.
- **`[env:xiao_c5]`**: the C5 runs its **normal** scanner firmware, so the real link and real radio
  load get exercised. Its USB console prints `[C5] gps: bytes=… nmea_ok=… bad=… fix=… sats=…` every 10 s.
- A multimeter.

**Ground rules:**
- **Never plug in both XIAOs' USB cables at once.** Both XIAOs' chargers sit on the cell, so two at
  once means two chargers working the same cell. This also solves port identification: both XIAOs
  enumerate as `303A:1001`, so with only one plugged in there's no ambiguity.
- **Keep the battery switch ON whenever a USB cable is in** (cell attached). The C5 runs from the cell
  through its own BAT pads (mini-build.md §2.5), so with the switch off it's dark and the link is
  down. That's expected, not a fault.
- On Windows, set `PYTHONIOENCODING=utf-8` before `pio … -t upload`, or the upload can hang.

## Stage 0 — before every power-up (unpowered checks)

Run these after **every** soldering session, before you apply power:

| Measure (meter in Ω, unpowered) | Good | Bad |
|---|---|---|
| S3 `3V3` ↔ S3 `GND` | climbs into the kΩ range as the capacitors charge | stays near 0 Ω: **short, don't power** |
| S3 `BAT+` ↔ S3 `3V3` | hundreds of Ω or more, possibly drifting | near 0 Ω: **BAT+ bridged to 3V3** |
| Each new wire, pad to pad | about 0 Ω | open: cold joint |
| Each new pad ↔ its neighbours | open | anything low: stray strand or solder bridge |

Tin every stranded wire end before soldering, and inspect the 3V3/GND pads (adjacent on both XIAOs)
under magnification.

## Stage 1 — S3 ↔ C5 (power + UART link)

**Flash first:**
1. C5 USB only: `pio run -e xiao_c5 -t upload`
2. S3 USB only: `pio run -e xiao_s3_bringup -t upload`

**Wire:**
- Switched cell `+` (the same net as S3 `BAT+`) → C5 `BAT+`
- Cell `−` / GND → C5 `BAT−`
- S3 `GND` → C5 `GND`
- S3 `D6` → C5 `GPIO24` (pad D5)
- S3 `D7` ← C5 `GPIO23` (pad D4)

The link is a **cross** (TX → RX); TX → TX gives total silence. **Do not** wire S3 `3V3` → C5 `3V3`:
the C5 doesn't boot that way (see mini-build.md §2.5).

**Test** (S3 USB only, switch **on**, cell attached):
1. **Voltage:** C5 `3V3` ↔ C5 `GND` should read **about 3.3 V** (its own regulator's output). XIAOs
   have no power LED, so a dark C5 tells you nothing; the meter does. But 3.3 V at the pad alone
   doesn't prove the C5 is running (that was the 2026-10-20 trap): the `[PROBE]` / `[LINK]` lines do.
2. `pio device monitor -e xiao_s3_bringup` and watch the `[LINK]` line:
   ```
   [LINK] PASS | pong 112  miss 4  corrupt 0  rtt avg 3ms max 9ms | burst 5/5 ok (last 38/38) | C5 up 61s resets 0
   ```
   - **pong** climbing: the link works.
   - **miss**: a few are normal. The C5 is briefly busy during each Wi-Fi scan pass.
   - **corrupt** must stay **0**. Bytes that arrive garbled point to a loose or noisy wire.
   - **burst x/y**: each burst is a full dump of the C5's detection table (large, back-to-back
     frames). A short count means frames were lost on the wire, even if pings pass.
   - **resets** counts C5 reboots, detected from its uptime going backwards. **Must stay 0.** A
     rising count means the C5 is browning out on the shared 3V3.
   - PASS needs about 30 s (3 clean bursts).
3. **Stress:** type `x` + Enter. The S3 starts continuous Wi-Fi scanning on top of the C5's scanning,
   the worst-case draw on the S3's regulator. Leave it for **2–3 min**. Pass if `resets` stays 0 and
   the S3 never reboots (`[PWR] S3 boot reason` must never become `BROWNOUT`).
4. **Battery:** with the switch on, unplug USB and let it run about 5 min. Then plug the **S3's** USB
   back in, still with the switch on, so nothing loses power. That's safe: the S3's small charger just
   trickles the cell. Open the monitor. If opening the monitor resets the S3, the counters restart,
   but **`C5 up` still reports the C5's own uptime**: it should cover the whole battery run. A small
   number means the C5 reset on battery.

| Symptom | Likely cause |
|---|---|
| C5 3V3 reads 0 V | open 3V3 or GND joint (measure at the pads, not the wire) |
| C5 3V3 well below 3.3 V | short or overload on the rail: back to Stage 0 |
| `FAIL (no Pong at all)` but 3V3 is fine | TX/RX swapped, an open D6/D7 wire, or the C5 isn't flashed with `xiao_c5` |
| `corrupt` > 0 or `burst` short | marginal joint on D6/D7/GND: reflow it and keep the leads short |
| `resets` rising, worse under `x` | 3V3 sagging: check the GND path and joint quality |

**Pin probe:** while the link is down, the bring-up runs a `[PROBE]` every ~15 s (or type `p`). It
reads the S3's D7 line with pull-down and pull-up to see whether the C5's TX is actually driving it:
- **driven HIGH**: the C5 is running and the C5 → S3 wire is good.
- **FLOATING**: the C5 isn't running, or the wire lands on the wrong pad.
- **driven LOW**: the C5 is held in reset, or the line is shorted to GND.

It also detects D6 and D7 bridged to each other. Remember the C5's pads are labelled by D-number /
silk, not GPIO: **GPIO23 = pad D4 (silk "SDA")**, **GPIO24 = pad D5 (silk "SCL")**.

To tell which link direction is broken: power from the **C5's** USB instead, run
`pio device monitor -e xiao_c5`, and look for `[C5] streamed N detections` every 10 s. If it appears,
S3 → C5 works and the fault is on C5 → S3 (`GPIO23` → `D7`).

## Stage 2 — DS3231 RTC

**Wire:** S3 `3V3` → RTC `+`; `GND` → `−`; `D4` → `D` (SDA); `D5` → `C` (SCL). Do Stage 0 again first.

**Test** (S3 USB):
```
[RTC ] PASS | 2026-10-20 18:04:11 UTC  temp 24.50C  osf=0 | i2c: 0x68
```
- `i2c:` lists everything on the bus. Expect `0x68`; ZS-042-style modules also show `0x57` (EEPROM,
  ignored).
- `osf=1` / `(unset)` is normal on first power-up. Set the clock by getting the epoch from PowerShell
  (`[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()`) and typing `s <epoch>`.
- **Persistence:** after setting the time, cut **all** power for about 30 s, then power back up. The
  time should still be correct with `osf=0`. If it isn't, check the coin cell (and see the
  CR2032/LIR2032 charge-resistor note in mini-build.md §2.4).
- `i2c: none`: SDA/SCL open or swapped, or the RTC isn't powered. Devices on the bus but no 0x68:
  wrong module.
- Re-run Stage 1's `x` stress: the link must still pass with the RTC on the rail.

## Stage 3 — NEO-6M GPS

**Wire:** S3 `3V3` → GPS `VCC`; `GND` → `GND`; GPS `TX` → C5 `GPIO1` (D0). Leave GPS `RX` unconnected.
Do Stage 0 again first.

**Test 1: NMEA is reaching the C5** (works indoors, no fix needed). Switch off, S3 USB out, C5 USB
in, then `pio device monitor -e xiao_c5`:
```
[C5] gps: bytes=48210 nmea_ok=1205 bad=0 fix=0 sats=0
```
- `bytes` climbing: the TX → GPIO1 wire works.
- `nmea_ok` climbing with `bad` at about 0: clean NMEA at 9600 baud.
- `bytes=0`: open wire, GPS unpowered, or wired to the wrong pad.
- `bad` high: noise or a marginal joint.

**Test 2: fix through the link.** Back to S3 USB, monitor `xiao_s3_bringup`, near a window or outside:
```
[GPS ] PASS | 3D fix  sats 7  40.123456, -75.123456  alt 52m  2026-10-20 18:10:02 UTC
```
A cold start can take 1–5 min (longer indoors). The module's LED blinks once it has a fix.

Finish by re-running Stage 1's `x` stress with everything connected, then a battery run.

## When done

Flash the real host firmware back onto the S3: `pio run -e xiao_s3 -t upload` (S3 USB only).

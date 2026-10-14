# Add-on wiring — battery monitor + GPS (CYD)

**Status: BUILT (Build C, with one change).** Both add-ons are on the current-build hardware
([cyd-scanner_wiring_diagram_v2.png](diagrams/cyd-scanner_wiring_diagram_v2.png)):

- **Battery voltage sense (GPIO35)** — built, **firmware live** (`src/cyd/power.*`; status-bar glyph
  + web app; STATUS `bat=`). See §4a.
- **Charge status (GPIO26 ← TP4056 CHRG)** — **NOT wired.** The TP4056 CHRG pad isn't broken out on
  this build, so there's no USB-charge-detect input and the indicator is **voltage/% only**. GPIO26
  stays free. (This is Build C minus the CHRG pin — see the table in §0.)
- **GPS on the C5 (GPIO15/23)** — **wired, firmware pending** (protocol-v9 gated; see §4b).
- **Power:** a second switch (**SW2**) was added inline on the boost-OUT+ → **C5 5V** leg (CYD VIN is
  fed directly). It isolates the C5 so the CYD's USB 5 V can't back-feed it while flashing. See
  PINOUT.md §4.

Pin facts are from [PINOUT.md](PINOUT.md); power nodes are from PINOUT.md §4 (the LiPo → TP4056 →
SW1 → 5 V boost rig).

---

## 0. The pin budget (read this first)

On the CYD, after the display, touch, SD, RGB LED, backlight (GPIO21), and the **C5 UART link
(GPIO22 = TX, GPIO27 = RX)**, exactly **two** GPIO are free:

| Pin        | Type                              | Good for                              |
|------------|-----------------------------------|---------------------------------------|
| **GPIO35** | input-only, **ADC1_CH7** (P3/IO1) | analog read (battery voltage) OR a UART RX |
| **GPIO26** | full GPIO, DAC2/ADC2 (audio hdr)  | digital in/out OR a UART RX           |

Caveats that drive the whole design:

- **Battery voltage MUST go on GPIO35.** It's the only free **ADC1** pin, and ADC2 pins (26/27)
  **cannot be read while Wi-Fi is active** — which is always, on this scanner. So 35 = voltage, non-negotiable.
- **GPIO1/GPIO3** (P1 serial header) are the USB-serial console / flashing UART — using them for a
  sensor fights programming and the debug console, so treat them as off-limits.
- That leaves **one** pin (GPIO26) for "the other thing." So you can't have the battery **voltage +
  charge-status LED + an onboard GPS** all on the CYD at once. Pick a build below.

### Build options

| Build | GPIO35 | GPIO26 | You get | You give up |
|-------|--------|--------|---------|-------------|
| **A — battery only (full)** | battery voltage | TP4056 CHRG | % **and** a charging indicator | — |
| **B — battery + GPS (recommended if you want both)** | battery voltage | **GPS TX → RX** | % **and** onboard GPS | the CHRG pin (infer "charging" from voltage instead) |
| **C — GPS on the C5 instead** | battery voltage | TP4056 CHRG | %, charging, **and** GPS | nothing on the CYD, but needs **C5 firmware + a link-protocol GPS frame** (the C5 has plenty of free GPIO) |

The rest of this doc details the **battery monitor** (§1) and the **GPS module** (§2), then the
**chosen build — C: full battery on the CYD + GPS on the C5** (§3), and the **firmware notes** (§4).

---

## 1. Battery monitor

### 1a. Voltage sense (GPIO35) — required for any % readout

A 2:1 resistor divider off the **switched** battery rail (so it only draws current when the master
switch is ON), tapped into GPIO35.

```
 (switched Vbat)                      GPIO35 is input-only — perfect for ADC, can't back-drive.
 TP4056 OUT+ ──[switch]──┬── Boost + input        ← this node = "switched Vbat", 3.0–4.2 V
                         │
                        R1 100 kΩ
                         │
                         ├─────────────► CYD GPIO35   (+ optional 100 nF to GND for a quiet ADC)
                         │
                        R2 100 kΩ
                         │
                        GND  (common ground — same as the boost OUT− / link ground)
```

| From                                   | To              | Notes                                              |
|----------------------------------------|-----------------|----------------------------------------------------|
| Switched Vbat (TP4056 OUT+ after switch / boost + input) | R1 (100 kΩ) top | Tap the **switched** side so the divider doesn't drain the cell when off |
| R1 bottom = R2 top (the midpoint)      | **CYD GPIO35**  | This is the sense node (≈ Vbat ÷ 2)                |
| R2 (100 kΩ) bottom                     | **GND**         | Common ground                                      |
| (optional) 100 nF cap                  | GPIO35 ↔ GND    | Smooths ADC noise from the high-impedance divider  |

- **Divide ratio:** 100 k / 100 k halves it → 4.2 V reads 2.10 V, 3.0 V reads 1.50 V — comfortably
  inside GPIO35's ADC range (use 11 dB attenuation; read with `analogReadMilliVolts(35)` then ×2).
- **Drain:** 4.2 V ÷ 200 kΩ ≈ 21 µA, only while switched on — negligible.
- Use **1 % resistors** for accuracy, or trim a correction factor in firmware against a multimeter.

### 1b. Charge status (GPIO26) — optional (Build A only)

TP4056 **CHRG** pad → **CYD GPIO26**, read as `INPUT_PULLUP`. CHRG is open-drain: pulled **LOW while
charging**, released (reads **HIGH**) when done / not charging.

| From          | To            | Notes                                               |
|---------------|---------------|-----------------------------------------------------|
| TP4056 `CHRG` | **CYD GPIO26**| `digitalRead` with internal pull-up; LOW = charging |

> In **Build B** GPIO26 is used by the GPS, so you lose this pin. Charging can instead be inferred in
> firmware from the voltage (on USB the pack sits high / climbs toward ~4.2 V).

---

## 2. GPS — GY-NEO6MV2 (NEO-6M)

The NEO-6M streams **NMEA at 9600 baud, 8N1** out its TX, 1 Hz by default. For a position readout you
only need to **read** it, so a single wire (module TX → a CYD RX pin) carries all the data.

| GY-NEO6MV2 pin | To (CYD)                 | Notes                                                                 |
|----------------|--------------------------|-----------------------------------------------------------------------|
| **VCC**        | **5 V** (CYD VIN / boost 5 V) *(or 3V3)* | The breakout has its own 3.3 V LDO, so 5 V is fine and keeps the CYD's 3.3 V rail unloaded. 3V3 also works. |
| **GND**        | **GND**                  | Common ground                                                         |
| **TXD**        | **CYD GPIO26** (UART RX) | Module → CYD. **This is the data wire.** TXD idles at **3.3 V logic** (NEO-6M side), safe for GPIO26 even when VCC = 5 V. |
| **RXD**        | *leave unconnected*      | Only needed to **configure** the module (change baud/rate). Read-only needs no CYD TX pin — and none is free anyway. |

Notes:

- **Current:** ~45 mA tracking, up to ~67 mA during cold-acquisition. Powering from **5 V** (not the
  CYD 3.3 V) avoids loading the AMS1117 that feeds the ESP32 + display.
- **Antenna / placement (matters for your case):** the ceramic patch antenna needs **sky view** and
  should sit **away from the ESP32 2.4/5 GHz radios and any metal** — RF noise from the scanners next
  to it will hurt fix sensitivity. Face the patch up/outward.
- **No backup wiring needed:** the GY-NEO6MV2 has an onboard backup cell + EEPROM for warm/hot starts.
- **Why this replaces/augments phone GPS:** an onboard fix removes the "screen-off kills the phone's
  GPS" problem (the web-app GPS only flows while the phone page is foregrounded). This is the backlogged
  "onboard GPS" item — see HANDOFF.

---

## 3. Chosen configuration — Build C: full battery plan on the CYD, GPS on the C5

The CYD keeps **both** battery pins (voltage **and** charge status); the GPS moves to the **C5**, which
has plenty of free GPIO. Cost: the C5 must read the NMEA and forward the fix to the CYD over the link,
which is a **firmware + link-protocol** change (§4b) — not wiring-only.

### 3a. CYD side — battery plan (as built)

| Signal              | CYD pin   | Connection                                             |
|---------------------|-----------|--------------------------------------------------------|
| Battery voltage     | **GPIO35**| 100 k/100 k divider **midpoint** (off switched Vbat)   |
| Divider bottom (R2) | GND       | common ground                                          |
| ~~Charge status~~   | ~~GPIO26~~| **NOT wired** — CHRG pad not broken out; GPIO26 stays free |

C5 link (GPIO22 TX / GPIO27 RX ⇄ C5 GPIO4/5) is **unchanged**. Power feed gained **SW2** on the
C5 5V leg (PINOUT.md §4).

### 3b. C5 side — GY-NEO6MV2 GPS

Mounted on the C5's spare hardware UART (UART1). Read-only needs just the one data wire (module TXD →
C5 RX); the TX pin is optional, only to reconfigure the module. Pins chosen to avoid the link (4/5),
console (11/12), USB-JTAG (13/14), strapping (8/9), the RGB LED (27), BOOT (28), and the unverified
25/26 — **GPIO15** and **GPIO23** are plain, broken-out right-header pins.

| GY-NEO6MV2 pin | To (C5)                   | Notes                                                              |
|----------------|---------------------------|--------------------------------------------------------------------|
| **VCC**        | **5V0** *(or 3V3)*        | Breakout has its own 3.3 V LDO; 5 V keeps the C5's 3.3 V rail unloaded. |
| **GND**        | **GND**                   | Common ground (shared with the link / boost ground)               |
| **TXD**        | **GPIO15** (UART1 RX)     | Module → C5. **The data wire.** TXD is 3.3 V logic → safe on a C5 GPIO. |
| **RXD**        | *unconnected* (or GPIO23) | Only for configuring the module (baud/rate). Omit for read-only.   |

> Alternate free C5 pins if 15/23 are awkward on your layout: 6, 7, 10, 24 (all plain/broken-out).
> Antenna placement rules from §2 still apply — sky view, away from the ESP32 radios + metal.

### 3c. Combined pinout (Build C)

```
        ┌──────────────── CYD (host) ───────────────┐        ┌──────────── ESP32-C5 (co-proc) ────────────┐
 Vbat/2 ┤ GPIO35 (ADC1)  battery voltage (÷2)        │        │ GPIO15 (UART1 RX) ◄── GPS TXD  (NMEA 9600) │
 (free) ┤ GPIO26         unused (CHRG not wired)      │        │ GPIO23 (UART1 TX) ──► GPS RXD  (config)    │
        │ GPIO22 ──────── link TX ───────────────────┼───────►│ GPIO4  (LP_UART RX)                        │
        │ GPIO27 ◄─────── link RX ───────────────────┼◄───────┤ GPIO5  (LP_UART TX)                        │
        │ VIN/5V ─┐                                   │        │ 5V0 ─┬─ GPS VCC                             │
        │ GND ────┴─ divider R2 + CHRG ret + ground ──┼────────┤ GND ─┴─ GPS GND                             │
        └────────────────────────────────────────────┘        └────────────────────────────────────────────┘
                     (5 V boost feeds CYD VIN + C5 5V0 in parallel; all grounds common)
```

| Signal          | Board | Pin        | Connects to                          |
|-----------------|-------|------------|--------------------------------------|
| Battery voltage | CYD   | GPIO35     | 100 k/100 k divider midpoint          |
| ~~Charge status~~ | CYD | ~~GPIO26~~ | **not wired** (CHRG pad not broken out) |
| GPS data (RX)   | C5    | GPIO15     | GY-NEO6MV2 TXD                        |
| GPS config (TX) | C5    | GPIO23     | GY-NEO6MV2 RXD (config-only)           |
| GPS power       | C5    | 5V0 (3V3)  | GY-NEO6MV2 VCC                        |
| GPS ground      | C5    | GND        | GY-NEO6MV2 GND                        |

All grounds are common (already shared by the UART link + the boost output). No change to the link
wiring or the 5 V power feed.

---

## 4. Firmware (for when this is implemented — not done yet)

### 4a. CYD — battery (DONE — no protocol change, CYD-only)

**Implemented** in [`src/cyd/power.{h,cpp}`](../src/cyd/power.cpp) + `src/cyd/main.cpp`:

```cpp
#define BAT_ADC_PIN  35            // ADC1_CH7, battery voltage (×2 of the 100k/100k divider)
// Vbat(mV) = analogReadMilliVolts(BAT_ADC_PIN) * 2, averaged + EMA-smoothed, mapped to a LiPo % curve.
// No CHRG pin (GPIO26 unwired) → voltage/% only, no charging state.
```

The `power` module reads GPIO35 on the scan cadence; `drawStatusBar()` paints a battery glyph (fill +
colour by level, hollow grey when no cell / on USB); `pushStatus()` adds `bat=<pct>` (−1 = absent) to
the phone STATUS string → the web app's app-bar battery indicator. CYD-only, no `PROTOCOL_VERSION`
change. (The `chg=` key in the earlier plan is dropped — no CHRG pin.)

### 4b. C5 — GPS, then forward over the link (**protocol bump, both boards flash together**)

> 🚫 **HOLD — do NOT start this until the owner explicitly says to.** It bumps `PROTOCOL_VERSION`
> (8 → 9) and touches `lib/link_protocol/link_protocol.h` + both firmwares. Until then, leave the
> protocol alone; phone GPS over BLE stays the only position source.

Because the GPS is on the C5 but the logger/UI live on the CYD, the fix has to cross the link — so
this is **not** wiring-only. Plan (when approved):

```cpp
// C5: read-only NMEA on UART1, RX = GPIO15 (TX unused; -1).
Serial1.begin(9600, SERIAL_8N1, /*RX=*/15, /*TX=*/-1);
// parse NMEA (TinyGPS++ or a minimal RMC/GGA parse) → lat/lon + fix flag.
```

- Add a GPS field/frame to [lib/link_protocol/link_protocol.h](../lib/link_protocol/link_protocol.h)
  — e.g. a `Reply::Gps` (lat/lon as int32·1e7 + a fix/valid flag), or fold lat/lon into `Status`.
  Either way `PROTOCOL_VERSION` bumps (currently **8 → 9**) and **both boards flash together**.
- The C5 streams its latest fix in the scan-dump burst (or on `GetStatus`); the CYD consumes it and
  routes it into the **existing position path** (today fed by the phone over BLE), so logging + the map
  get an onboard fix. **Prefer the onboard fix when valid, fall back to phone GPS** — this also fixes
  the "screen-off kills the phone GPS" problem (backlogged onboard-GPS item).
- Keep the UART1 RX read cheap (buffered/interrupt-driven) so it doesn't disturb the C5's radio
  time-slicing; GPS at 9600/1 Hz is light.
```

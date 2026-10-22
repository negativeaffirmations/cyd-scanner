# mini-build — screenless XIAO sibling (XIAO ESP32-S3 host + XIAO ESP32-C5 scanner)

Design + migration plan for a smaller, **screenless** variant of cyd-scanner. The host is a
**Seeed Studio XIAO ESP32-S3 Sense** (replaces the CYD); the scanner co-processor is a **Seeed
Studio XIAO ESP32-C5** (replaces the ESP32-C5 DevKit). There is **no TFT, no touch, no BOOT-menu
UI** — the device is driven entirely through the existing BLE phone web app. A **DS3231 RTC** is
added on the host for a persistent wall clock. The camera on the S3 Sense is **deferred** (not in
this build) but its pins are kept reserved.

This document is implementation-ready and self-contained. It assumes you will validate with
`pio run -e cyd`, `pio run -e xiao_s3`, and `pio run -e xiao_c5` (and the existing `pio run -e c5`
must keep building unchanged).

> Scope reminder: this is a **defensive** detection/fingerprinting build. Nothing here adds or
> enables interference/attacks (no jamming/deauth/injection/spoofing/floods/DoS). The S3 and C5
> radios transmit only for the tool's own function (BLE phone link, Wi-Fi SoftAP log download,
> active scan requests) — identical to the current build.

## 1. Overview and the one-repo decision

**Decision: one repo, add new PlatformIO envs. Do NOT split the project.** The new variant shares
almost everything that matters with the existing build:

- the **web app** (`webapp/`) is the *primary and only* UI for the headless host — a split repo
  would fork or duplicate it;
- the **UART link protocol** (`lib/link_protocol/`) is identical and must stay in lockstep across
  all four firmwares (CYD, XIAO-S3, DevKit-C5, XIAO-C5);
- the **signature DB** (`/signatures.csv` seed in `src/cyd/sigdb.cpp`), the **NDJSON log schema**,
  the **DETS stream format**, and the **STATUS string** are shared contracts;
- `src/c5/` firmware is reused verbatim (only a pin branch differs);
- docs (`docs/`, `hardware/PINOUT.md`) describe both rigs together.

A monorepo keeps these synchronized by construction (the same reasoning that put the CYD and C5 in
one repo originally). The variant is expressed purely as: two new board envs + a new board source
folder + a shared `src/host_common/` core carved out of today's CYD-only `src/cyd/main.cpp`.

### Boards at a glance

| Role | Current build | Mini build | SoC / radios | Notes |
|------|---------------|------------|--------------|-------|
| Host | ESP32-2432S028R "CYD" | **XIAO ESP32-S3 Sense** | ESP32-S3, BLE 5.0 + 2.4 GHz Wi-Fi, 8 MB PSRAM + 8 MB flash | headless; onboard microSD on Sense board; camera deferred |
| Scanner | ESP32-C5 DevKit | **XIAO ESP32-C5** | ESP32-C5, Wi-Fi 6 2.4+5 GHz + BLE 5 + 802.15.4 | same SoC → `src/c5/` reused; pins differ; 5 GHz antenna caveat |
| Clock | (none; phone/GPS) | **DS3231 RTC** on host I2C | — | persistent wall clock, 3rd time source + sink |

Same-SoC swaps preserve the radios and therefore all radio-dependent features: the phone BLE GATT
link (`phone.*`, NimBLE), the Wi-Fi SoftAP log download (`webshare.*`), active Wi-Fi/BLE scanning,
and SD logging all carry over to the S3; dual-band Wi-Fi 6 + BLE + 802.15.4 all carry over to the
XIAO C5.

## 2. Hardware

### 2.1 XIAO ESP32-S3 Sense (host) — pin plan

Exposed breakout pads (fixed by the board; well-established):

| Pad | GPIO | Alt | Mini-build use |
|-----|------|-----|----------------|
| D0 | 1 | ADC | spare (alt link/host sensor) |
| D1 | 2 | ADC | spare |
| D2 | 3 | ADC | spare |
| D3 | 4 | ADC1_CH3 | **battery sense** (100k/100k divider off switched Vbat, added 2026-10-21) |
| D4 | 5 | **SDA** | **DS3231 SDA** (I2C) |
| D5 | 6 | **SCL** | **DS3231 SCL** (I2C) |
| D6 | 43 | U0TXD | **Link UART1 TX → XIAO-C5 RX** |
| D7 | 44 | U0RXD | **Link UART1 RX ← XIAO-C5 TX** |
| D8 | 7 | SCK | **microSD SCK** (Sense B2B, shared w/ camera bus) |
| D9 | 8 | MISO | **microSD MISO** |
| D10 | 9 | MOSI | **microSD MOSI** |
| (onboard LED) | 21 | — | status LED (active low) — see collision note |

Reserved for the **deferred camera** (OV2640/OV3660 on the Sense B2B) — **do not use**: GPIO
10–18 (DVP data + XCLK + PCLK) and GPIO 38–48 (VSYNC/HREF + SCCB SIOD/SIOC on 40/39). Because the
camera is deferred, nothing is wired to these now, but keep them free so the camera sub-board drops
in later with no re-layout.

Rationale / coexistence:
- **DS3231 on D4/D5 (GPIO5/6)** is the dedicated `Wire` I2C bus and is clear of both the SD SPI
  group (GPIO7/8/9) and the camera group (GPIO10–18/38–48). The camera's own SCCB I2C is on
  GPIO39/40 internally — a *separate* bus, so the RTC bus and a future camera never collide.
- **Link UART on D6/D7 (GPIO43/44).** Console is USB-CDC (`ARDUINO_USB_CDC_ON_BOOT=1`), so the
  UART0 peripheral/pads are free for the inter-board link via `HardwareSerial(1)` matrixed onto
  these pads — the same "console-is-USB, UART free for the link" pattern the C5 already uses.
  Alternates if needed: D0/D1 (GPIO1/2).
- **microSD on D8/D9/D10 (GPIO7/8/9)** is the Sense B2B SPI; treat SD + camera as one reserved bus
  group. Mirrors the CYD rule "SD on its own SPI, off the link/I2C."

> **VERIFY before wiring SD:** confirm the XIAO ESP32-S3 **Sense** microSD SCK/MISO/MOSI **and
> especially the CS pin** against Seeed's Sense schematic/wiki — the SD and camera are on one B2B
> bus and the exact CS is board-fixed, not a free choice. If SD CS turns out to be **GPIO21**, it
> collides with the onboard user LED: in that case move the status-LED function to a spare pad
> (D0/GPIO1) or drop the LED. The host is headless, so the LED is cosmetic — do not let it block SD.

> **Battery sense (added 2026-10-21):** the XIAO exposes no battery divider of its own, so a
> 100 kΩ/100 kΩ divider from the switched battery rail feeds **D3 (GPIO4, ADC1_CH3)**, the same
> design as the CYD's GPIO35. STATUS `bat=` reports 0–100 % (−1 below 2.5 V / no cell). See §2.5.

### 2.2 XIAO ESP32-C5 (scanner) — pin plan

The XIAO C5 only breaks out GPIO **0, 1, 7, 8, 9, 10, 11, 12, 23, 24, 25** (8/9 are strapping). The
DevKit's link pins (GPIO4/5) and GPS RX (GPIO15) are **not** broken out, so both move:

| Signal | DevKit C5 (today) | **XIAO C5 (new)** | Notes |
|--------|-------------------|-------------------|-------|
| Link UART TX → host RX | GPIO5 | **GPIO23** | plain broken-out GPIO, matrixed to `HardwareSerial(1)` |
| Link UART RX ← host TX | GPIO4 | **GPIO24** | plain broken-out GPIO |
| GPS RX ← NEO-6M TXD | GPIO15 (UART0) | **GPIO1** | receive-only (TX=-1); UART0, remapped |
| GPS TX (unused) | GPIO23 | **-1** | read-only NMEA, no TX needed |

Keep clear of the strapping pins (GPIO8/9), the USB-JTAG/console, and the DevKit's unverified
25/26 caveat. GPIO0/1 are LP pins that double as XTAL_32K — fine as plain GPIO when no 32 kHz
crystal is fitted (verify item below). Link UART keeps the existing `UART_CLK_SRC_XTAL` clocking so
Wi-Fi PLL activity can't shift its baud (unchanged from `src/c5/main.cpp`).

### 2.3 5 GHz antenna (hardware pre-req / risk)

The XIAO ESP32-C5 ships with a **2.4 GHz-only** antenna. The entire reason the C5 is in this
project is **5 GHz Wi-Fi 6** coverage (`PH_PROMISC5` sweep + dual-band AP scan). With the stock
antenna, 5 GHz sensitivity will be poor to nonexistent. **Pre-req:** fit a **dual-band 2.4/5 GHz
u.FL antenna** to the XIAO C5's antenna connector. Validate by confirming 5 GHz beacons appear in
the detection table (channels > 14) before trusting the build's 5 GHz claims. Flag in
`hardware/PINOUT.md`.

### 2.4 DS3231 RTC module

- I2C address **0x68**. ZS-042/"DS3231 AT24C32" modules also expose an **AT24C32 EEPROM at 0x57** —
  ignore it (don't probe/claim 0x57).
- **CR2032 trickle-charge hazard:** the common ZS-042 board has a charge resistor/diode intended
  for a rechargeable **LIR2032**. With a non-rechargeable **CR2032** this can overheat/leak. Either
  fit a **LIR2032**, or remove the charge resistor (commonly `R5`/the series diode) and keep a
  CR2032. Document whichever you do.
- Wire VCC→3V3, GND→GND, SDA→D4 (GPIO5), SCL→D5 (GPIO6). DS3231 is 3.3 V-safe.

### 2.5 Power & charging (supersedes the CYD TP4056 + boost subsystem)

The XIAO boards **run a single-cell LiPo directly** — no boost/step-up and (optionally) no external
charger, unlike the CYD build (`battery-power-subsystem`). The CYD needed a **5 V** rail, so it used
a TP4056 to charge and a boost converter to lift 3.7 V → 5 V. The XIAO is native **3.3 V**: the cell
(3.0–4.2 V) feeds the XIAO's onboard regulator directly, so **the boost/step-up module is simply
deleted**, and an onboard charge IC charges over USB-C.

**Topology (REVISED 2026-10-20): one cell, each XIAO on its own regulator:**
- The switched, protected cell output (TP4056 `OUT+` → switch) feeds **both XIAOs' BAT pads**:
  **XIAO-S3 `BAT+`/`BAT−`** *and* **XIAO-C5 `BAT+`/`BAT−`**. Each board's onboard regulator makes its
  own 3.3 V.
- The **NEO-6M GPS and DS3231** run from the **S3's `3V3` output pin**. All grounds are tied together.
- **Do NOT wire S3 `3V3` → C5 `3V3`.** That was the original plan, and **it does not work**. On
  hardware (2026-10-20) the C5's 3V3 pad read 3.3 V, but the C5 **never started its firmware**: the
  bring-up pin probe showed its link TX 100% undriven, with no reboot loop. The same C5 ran perfectly
  as soon as its **own** regulator was fed (a temporary S3 `5V` → C5 `5V` jumper on USB). The root
  cause is unconfirmed (no XIAO-C5 schematic in the repo): either the S3's regulator can't start the
  C5, or the C5's 3V3 pad isn't a usable supply input. Don't rely on the 3V3 pad.
- **Both XIAO chargers now sit on the cell**, but a XIAO's charger is only active while **that board's
  own USB** is plugged in. Rule: **never plug in both XIAOs' USB at once.** One XIAO USB at a time just
  trickles the cell, which is harmless (see the TP4056 notes below).
- **The C5 has no supply except the cell (or its own USB).** With the switch **off**, an S3-USB-only
  bench session leaves the C5 dark and the link down. **Keep the switch ON whenever any USB is
  plugged in.** Switch-off plus C5-USB would also feed the S3's BAT pad from the C5's charger with no
  cell attached.
- On battery the **5 V / VUSB pin floats** (live only on USB) — every peripheral must be 3.3 V-safe
  off-battery. DS3231 ✓; confirm the GY-NEO6MV2 runs at 3.3 V (most do via their onboard regulator).

> **CELL CHANGE (2026-10-21):** the LP103450 **2000 mAh** cell was swapped for a smaller **1000 mAh**
> LiPo (the 2000 mAh didn't fit the case; exact model not recorded). Same wiring/topology. What
> changes: **runtime roughly halves**, and the TP4056's default **1 A** charge current (`Rprog` 1.2 kΩ)
> is now **1C** — at or above the max charge rate of many small LiPos. Check the new cell's datasheet;
> if it says ≤ 0.5C (or nothing), swap `Rprog` to **~2.4 kΩ (≈ 0.5 A)**. The 2000 mAh figures below
> are kept as the original rationale. Battery-% mapping is voltage-only, so it needs no change.

**Charging the LP103450 (2000 mAh) — DECIDED: external TP4056 charge path (no boost).** The XIAO
onboard charger is small and fixed (~50–100 mA, IC is an SGM4056/TP4057 depending on rev, **not**
program-resistor adjustable), so a **2000 mAh** cell would take **~20–40 h** to charge off it —
impractically slow. The build therefore keeps a **TP4056 module** from the CYD BOM **as the charge
path only** (the boost/step-up is still deleted — the XIAO runs the cell natively at 3.3 V):

- Wire **LiPo → TP4056 `B+`/`B−`**, and **TP4056 `OUT+`/`OUT−` → XIAO-S3 `BAT+`/`BAT−`**. The XIAO
  runs from the TP4056's protected output; charging happens through the **TP4056's own USB** input.
- Use a TP4056 module **with battery protection** (the DW01+FS8205 variant with `OUT±` pads) so the
  cell gets over-discharge/over-current cutoff. If the LP103450 already has an inline protection PCB,
  that's fine (double protection just means a slightly earlier cutoff).
- Charge current: the common module's default `Rprog` = 1.2 kΩ ≈ **1 A** (≈ 0.5C for 2000 mAh →
  ~2–2.5 h full charge), which is appropriate. For a gentler 0.5 A, swap `Rprog` to ~2.4 kΩ.
- **Do not charge via the XIAO's USB and the TP4056 at the same time** — that's two active chargers
  on one cell. Keep the XIAO USB for flashing/console; charge through the TP4056's USB. (Plugging
  XIAO USB while running is harmless — its ~100 mA charger just trickles — but don't rely on it.)
- Net BOM change vs. CYD: **TP4056 kept, boost/step-up removed.**

**Battery sense (added 2026-10-21):** switch output (the rail feeding both XIAOs' B+) → 100 kΩ →
**S3 D3** → 100 kΩ → GND. Tapping the switched side means the divider draws nothing when off.
`Vbat = analogReadMilliVolts(D3) × 2`, mapped to % by the shared `src/host_common/power.cpp` curve.
Caveat: with the switch OFF but the S3 on USB, the S3's own charger drives its B+ pad (= this rail), so
the reading follows the charger, not the cell. (The XIAO-C5's BAT_VOLT sense is still unread: it would
need a link field.)

### 2.6 Complete device wiring (pinout)

All five components, net by net. The two XIAOs each run from the switched cell through their own
regulator; the RTC and GPS share the S3's 3V3; single common ground. Wire and verify them one module
at a time with the staged bring-up test: [mini-build-bringup.md](mini-build-bringup.md).

> ⚠️ `hardware/diagrams/mini-build_wiring_diagram*.{png,svg}` still show the OLD S3 3V3 → C5 3V3
> feed and are being redrawn. **This table is authoritative.**

| From | Pin | To | Pin | Net / purpose |
|------|-----|----|-----|---------------|
| LP103450 LiPo | **+** | TP4056 | **B +** | cell → charger battery terminal |
| LP103450 LiPo | **−** | TP4056 | **B −** | cell → charger battery terminal |
| TP4056 | **OUT +** | Switch | common | master on/off |
| Switch | throw | XIAO-S3 | **BAT +** | switched cell → host regulator |
| Switch | throw | XIAO-C5 | **BAT +** | switched cell → scanner's **own** regulator (same net as S3 BAT+) |
| TP4056 | **OUT −** | XIAO-S3 | **BAT −** | protected cell return |
| TP4056 | **OUT −** | XIAO-C5 | **BAT −** | protected cell return (same net as S3 BAT−/GND) |
| TP4056 | **IN + / IN −** | *(USB)* | — | charge input — charge via the TP4056's own USB only |
| ~~XIAO-S3~~ | ~~**3V3**~~ | ~~XIAO-C5~~ | ~~**3V3**~~ | **REMOVED 2026-10-20**: the C5 won't boot from its 3V3 pad (§2.5) |
| XIAO-S3 | **3V3** | DS3231 | **VCC** | RTC power |
| XIAO-S3 | **3V3** | NEO-6M | **VCC** | GPS power (verify 3.3 V) |
| XIAO-S3 | **GND** | XIAO-C5 | **GND** | common ground |
| XIAO-S3 | **GND** | DS3231 | **GND** | common ground |
| XIAO-S3 | **GND** | NEO-6M | **GND** | common ground |
| XIAO-S3 | **D4 / GPIO5 (SDA)** | DS3231 | **SDA** | I2C data |
| XIAO-S3 | **D5 / GPIO6 (SCL)** | DS3231 | **SCL** | I2C clock |
| XIAO-S3 | **D6 / GPIO43 (link TX)** | XIAO-C5 | **GPIO24 (link RX)** | UART link: host → C5 |
| XIAO-S3 | **D7 / GPIO44 (link RX)** | XIAO-C5 | **GPIO23 (link TX)** | UART link: C5 → host |
| XIAO-C5 | **GPIO1 (UART0 RX)** | NEO-6M | **TXD** | GPS NMEA → C5 (receive-only) |
| XIAO-C5 | *(none)* | NEO-6M | **RXD** | leave unconnected (C5 GPS TX = −1) |

Notes:
- **Link is a cross:** host TX→C5 RX and C5 TX→host RX (don't wire TX→TX). Keep the leads short and
  mechanically solid — the existing link-robustness notes (loose contacts drop the large detection
  burst) apply unchanged; `LINK_MONITOR` is still the tool for hunting bad contacts.
- **GPS lives on the C5** (not the host), exactly as in the current build — its fix/epoch ride back
  to the host in the `Status` frame (link v10). Receive-only: the C5's GPS TX is `-1`, so the NEO-6M
  `RXD` pin is left floating.
- **microSD** on the XIAO-S3 Sense is onboard (B2B) — no external wiring; verify its CS pin per §2.1.
- **DS3231 coin cell:** fit a LIR2032 **or** remove the ZS-042 charge resistor and use a CR2032
  (§2.4). Camera pins (§2.1) stay unwired/reserved.

## 3. platformio.ini changes

Add two envs; keep the existing `[env:cyd]`, `[env:c5]`, `[env:c5test]` unchanged except the CYD's
`build_src_filter` (now also pulls in `host_common/`).

### 3.1 CYD env — only the filter changes

```
[env:cyd]
board = jczn_2432s028r
build_src_filter = +<cyd/> +<host_common/>      ; was +<cyd/>
board_build.partitions = huge_app.csv
lib_deps = (unchanged: TFT_eSPI, XPT2046, NimBLE, QRCode, ArduinoJson)
build_flags = (unchanged TFT_eSPI config)
```

PlatformIO's LDF compiles every env's `build_src_filter` folders plus everything under `lib/`, so
`lib/link_protocol/` keeps flowing into all envs automatically, and `+<cyd/> +<host_common/>`
compiles both the CYD UI/board layer and the shared core. (Confirmed this is how the current
two-folder split already works.)

### 3.2 New host env — XIAO ESP32-S3

```
[env:xiao_s3]
board = seeed_xiao_esp32s3
build_src_filter = +<xiao_s3/> +<host_common/>
board_build.partitions = huge_app.csv           ; 3 MB app (BLE+WiFi+web server); 8 MB flash, plenty
build_flags =
  -D ARDUINO_USB_MODE=1                          ; (also set by the board def; harmless to restate)
  -D ARDUINO_USB_CDC_ON_BOOT=1                   ; USB-CDC console → frees UART0 pads for the link
lib_deps =
  h2zero/NimBLE-Arduino@^2.2.0
  bblanchon/ArduinoJson@^6.21.5
  ; a DS3231 driver — prefer a small direct-I2C read (no lib) OR adafruit/RTClib; see §6.
  ; NO TFT_eSPI, NO XPT2046, NO QRCode (headless; QR is rendered by the web app).
```

Notes: the S3 board def already supplies `-DBOARD_HAS_PSRAM` and `memory_type qio_opi` — PSRAM is
available but not required by this firmware (keep the existing SRAM-only, no-PSRAM allocations).
Flash is single native USB-C: simple auto-reset upload, no DevKit two-port gotcha. Set
`PYTHONIOENCODING=utf-8` for `pio upload` on Windows (per MEMORY).

### 3.3 New scanner env — XIAO ESP32-C5

```
[env:xiao_c5]
board = seeed_xiao_esp32c5
build_src_filter = +<c5/>                        ; reuses the existing scanner firmware verbatim
board_build.partitions = huge_app.csv
build_flags =
  -D ARDUINO_USB_MODE=1
  -D ARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
  h2zero/NimBLE-Arduino@^2.2.0
```

`src/c5/pins.h` becomes board-conditional (one `#if defined(ARDUINO_XIAO_ESP32C5)` branch for the
new link/GPS pins in §2.2, `#else` keeping the DevKit's GPIO4/5 + GPIO15). The XIAO C5 board def
defines `ARDUINO_XIAO_ESP32C5`; the DevKit def does not — so the single header serves both scanner
envs with no other `src/c5/` change. Native USB hwid is `303A:1001` (same as the DevKit native port
and the ODID-tx S3) → identify the target by `esptool chip-id` before flashing (per MEMORY).

## 4. The `main.cpp` → `host_common/` refactor

Today `src/cyd/main.cpp` (5,011 lines) mixes the host **core** with all TFT/touch/BOOT UI. The
headless host needs the core and none of the UI. The refactor extracts the core into
`src/host_common/`; each host keeps a thin board layer.

**Guiding principle (keeps risk low):** `setup()`, `loop()`, and the entire `SCR_*` state machine
stay **per-board** in each board's `main.cpp`. Only the functions those call are moved. The CYD's
`loop()` and all its `draw*` code are left textually as-is (now calling the moved functions via
headers), so the CYD build is a **no-behavior-change** refactor. The XIAO-S3 gets a brand-new,
tiny `loop()` with no screen machine that calls the same shared functions.

### 4.1 Target layout

```
src/
  cyd/            [env:cyd]      UI + CYD board layer (TFT/touch/BOOT/logo/QR/Explore/power)
  xiao_s3/        [env:xiao_s3]  NEW: headless board layer (setup/loop, LED, RTC wiring, stubs)
  c5/             [env:c5] + [env:xiao_c5]   scanner firmware (pins.h now board-branched)
  c5test/         [env:c5test]   unchanged
  host_common/    compiled into BOTH host envs: the board-agnostic core
lib/
  link_protocol/  UNCHANGED, compiled into all envs
```

Proposed `host_common/` translation units (split along existing logical groups):

| File | Contents |
|------|----------|
| `host_link.{h,cpp}` | `FrameParser` use, `sendFrame`, `pingC5`, `requestScan`; owns the `LinkSerial` instance; `begin(rxPin,txPin)` called by the board. |
| `host_core.{h,cpp}` | detection table (`g_dets/g_score/g_detCount`, `MAX_DET=96`), RID store (`g_rids`, `ridByMac`), scoring (`computeScores`, `FlagRule`/`kFlagRules`, `flagTokens`, `detLabel`), categories (`Category`, `kCatName`, `categoryOf`, `groupCategory`, `countCategories`), grouping (`DevGroup`, `g_groups`, `buildGroups`, `buildSorted`, `groupName`, `countBands`, `countTiers`, `countMuted`), follow engine (all `FOLLOW_*`, `g_follow`, `updateFollowState`, `computeFollowScores`, `findFollow`, `isFollowing`, `resetFollow`, `macIsBleRpa`), position/time (`posHasFix/Lat/Lon/Source`, `timeHasSync/epochLocal/timeSource`, `g_onb*`, `g_gpsEpoch*`, **+ RTC source, §6**), events (`g_deauthRecent/evilCount/ridCount`, `updateEvents`, alert-active helpers), `isSelfDet`/`g_ownMac`, NVS src-mask + bg-scan load/save, `g_srcMask/g_scanActive/g_bgScan/g_linkOk/g_lastLinkOkMs`, and the **scan-cycle pipeline** (§4.4). |
| `host_log.{h,cpp}` | `LogStream`, `g_fg/g_bg`, `SeenKey`, `seen*`, `bgCtx`/`activeStream`/`syncSharePath`, session lifecycle (`sweepEmptySessions`, `beginSessionNamed`, `openSession`, `openBgSession`, `startNewSession`, `ensureLogFile`, `renameStreamOnSync`, `renameSessionOnSync`), `initSD`, `scannerIdHex`, `reseedDbIfNeeded`, `writeSessionMeta`, `FW_VERSION`, `DB_GEN`, `BGDEF_GEN`, `lnAppend`, `jsonEscape`, `sanitizeField`, `logNewDetections`, `logEvent`, `logRidEvents`, `logFollowEvents`, `enforceLogCap`/`logProtected`/`logBase`/`wipeAllLogs`/`deleteSession`, `sendSessionList`/`sendWhitelist`, transfers (`streamFileOverBle`, `streamFilteredOverBle`, `transferLog`, `transferFiltered`, `transferFile`), `sessBase`. |
| `host_status.{h,cpp}` | `pushStatus`, `pushDetections` (DETS v6 builder), `DETS_STREAM_MAX`. |
| `host_phone_cmds.{h,cpp}` | `servicePhoneCommands(PhoneCmdFlags&)` — the pure-core body of the loop()-top dispatch (§4.5). |
| `host_board.h` | board-provided interface (LED, battery %, brightness, download-UI hook, scan-policy). Declarations only; impl per board. |
| `host_ui.h` | optional UI hooks the pipeline calls (CYD draws; S3 no-ops). Declarations only. |
| `pins_host.h` *(or reuse per-board pins.h)* | shared host pin **names**; actual values per board. |

Modules that **already port as-is** (board-agnostic) → **move into `host_common/`** so both host
envs compile them: `sigdb.{h,cpp}`, `phone.{h,cpp}` (NimBLE; S3 has BLE), `webshare.{h,cpp}`
(Wi-Fi SoftAP; S3 has Wi-Fi), `whitelist.{h,cpp}`, `logfilter.h`. Update their `#include` paths.

Modules that are **board-specific** (header shared, impl per board): `power.h` → `host_common/`;
`power.cpp` was later (2026-10-21) moved to `host_common/` too: one implementation, each board passes its
own `BAT_ADC_PIN` to `power::init()` (CYD GPIO35, S3 GPIO4/D3).

Stays **CYD-only** in `src/cyd/` (UI; not compiled for S3): everything TFT/touch/BOOT — the
`Screen` enum + `g_screen` + all `SCR_*` handling in `loop()`, `render`, `updateScan`,
`drawStatusBar`, `drawMenu*`, `drawScan*`, all `icon*`/glyphs, `drawSplash`/`logo.h`, QR screens
(`drawCenteredQr`, `drawDownloadScreen`, `drawAppQrScreen`), `serviceTouchLock`, `serviceGpsModal`,
`buttonEvent`, `tappedRow`/`topBar*`/`bannerTapped`/`listTouch`, brightness apply (`applyBrightness`
+ NVS), `touch.{h,cpp}`, `kCatColor[]` (RGB565 palette), and the on-device **Explore viewer**
(`readLogLine`, `parseJsonLine`, `parseCsvLine`, `parseLogLine`, `buildScanView`, `buildIndex`,
`sessEnumerate`, `drawPickLog`/`drawViewer*`/`drawDetail*`/`openDetail`, sort/filter screens, the
follow drill-down screens, the whitelist UI screens, the category drill-down screens). The headless
host does all of this through the phone instead.

### 4.2 `host_board.h` interface (board implements, core calls)

```
namespace hostboard {
  void begin();                         // board bring-up (pins, link UART, RTC on S3, LED, power)
  void setScanLed(bool r,bool g,bool b);// CYD: RGB on 4/16/17; S3: map to GPIO21 (1 LED) or no-op
  int  batteryPercent();                // CYD: power::percent(); S3: -1
  uint8_t brightness();                 // CYD: g_brightness; S3: 100 (reported in STATUS only)
  bool rtcPresent();                    // CYD: false (no DS3231); S3: DS3231 probe result
}
```
`applyScanLed()` logic moves into core but calls `hostboard::setScanLed()`. `pushStatus` reads
`hostboard::batteryPercent()` / `hostboard::brightness()` instead of `power::`/`g_brightness`
directly (CYD values identical → STATUS string unchanged).

### 4.3 `host_ui.h` hooks (core pipeline calls; CYD draws, S3 no-op)

```
namespace hostui {
  void onScanCycle(const ScanCycleResult&); // CYD: updateScan() if SCR_SCAN; S3: serial line
  void onBusyMiss();                         // CYD: drawStatusBar() (link dot/clock); S3: no-op
  void onDownloadState(bool apActive, bool joined); // CYD: drawDownloadScreen(); S3: no-op (QR via STATUS→web app, §7)
  void onStatusPushed();                     // optional; CYD no-op
}
```
These exist only because a handful of shared orchestrators currently call `draw*`. Keeping the
hook set tiny is deliberate — the big UI surface stays entirely inside the CYD state machine, which
is not shared.

### 4.4 Split `runScanCycle`

`runScanCycle()` becomes `core::runScanCycle()` returning a `ScanCycleResult` and calling the
hooks instead of TFT directly:

```
struct ScanCycleResult { bool busyMiss; int newCount; int n24,n5,nble,nprb,n154; int susp,lk,conf; };
```
- Pure body (moved to core): `power::sample` → `hostboard` sample; `requestScan`; on busy-miss set
  `g_linkOk` stickiness, clear stale C5 events, `hostboard::setScanLed`, call `hostui::onBusyMiss()`,
  schedule fast re-poll, return `{busyMiss=true}`. On success: `computeScores`, `buildGroups`,
  `updateFollowState`, `computeFollowScores`, bg-stream roll check, `logNewDetections`,
  `logRidEvents`, `logFollowEvents`, `pushStatus`, `pushDetections`, then `hostui::onScanCycle(res)`.
- The CYD's `onScanCycle` does `if (!g_viewFrozen) buildScanView(); if (g_screen==SCR_SCAN) updateScan();`
  — preserving current behavior (note: `buildScanView`/`g_viewFrozen` are CYD UI state, so that
  call moves into the CYD hook, not the core body). The S3's `onScanCycle` prints a one-line serial
  summary.

### 4.5 Split the phone-command dispatch

The loop()-top block becomes `core::servicePhoneCommands(PhoneCmdFlags& f)` handling every
board-agnostic command (reload DB, BLE log transfer, session list, whitelist reload/list/add/remove,
file download, filtered export, delete, new session, `renameSessionOnSync`, `syncSharePath`,
Wi-Fi download enter/exit, the `enforceLogCap` timer, scan start/stop). It sets flags for the few
that need a board UI reaction:

```
struct PhoneCmdFlags { bool brightnessChanged; int brightnessPct; bool lockChanged; bool lockState;
                       bool srcMaskChanged; bool sessionChanged; bool scanStarted; bool scanStopped;
                       bool newSession; bool downloadEnter; bool downloadExit; };
```
- CYD `loop()` keeps today's exact reactions (apply+save brightness, `drawStatusBar` on lock,
  `drawScanSettings` on srcmask, `showScan()` on start while off SCR_SCAN, redraw scan menu on
  session change, `runDownload()` on download enter). Behavior identical.
- S3 `loop()` reacts minimally: save brightness/lock/srcmask to NVS + `pushStatus()`; start/stop
  just toggles `g_scanActive`; download enter runs `runDownload()` headless (SSID/PSK go out via
  STATUS, §7). No draws.

### 4.6 Scan-policy abstraction (fg vs bg stream, run-gating)

The core currently decides fg vs bg via `bgCtx() = g_bgScan && g_screen != SCR_SCAN` and gates the
pipeline on several `SCR_*` screens. `g_screen` is UI-only. Introduce a board policy the core reads:

```
namespace hostboard {
  bool foregroundScan();  // true → use g_fg + run the live-scan path. CYD: g_screen==SCR_SCAN. S3: g_scanActive-from-web-"G"
  bool pipelineAllowed();  // CYD: false on Explore/drill-down screens; S3: always true
}
```
`bgCtx()`/`activeStream()` are rewritten in terms of `hostboard::foregroundScan()`. On the CYD the
two functions return exactly what the old `g_screen` checks did → no behavior change (the headless
background cycle vs on-screen cycle distinction is preserved). On the S3 there is one logical scan
driven by the web app `G/X`: a web-app scan writes the `g_fg` stream; the always-on background
scan (when idle) writes `g_bg` — same two-stream model, same files.

### 4.7 Ordered implementation phases (each independently buildable)

**Phase 0 — scaffolding (no logic change).** Create empty `src/host_common/` and `src/xiao_s3/`.
Add `[env:xiao_s3]`/`[env:xiao_c5]` and switch `[env:cyd]` filter to `+<cyd/> +<host_common/>`.
Branch `src/c5/pins.h` on `ARDUINO_XIAO_ESP32C5`.
*Checkpoint:* `pio run -e cyd` builds; `pio run -e c5` builds; `pio run -e xiao_c5` builds (scanner
done — it's just the pin branch + env).

**Phase 1 — CYD no-op core extraction.** Move the modules in §4.1 into `host_common/`
(sigdb/phone/webshare/whitelist/logfilter + the carved host_core/host_log/host_status/host_link/
host_phone_cmds). Introduce `host_board.h`/`host_ui.h`; implement them in `src/cyd/` wrapping the
existing CYD draws/LED/power/brightness. Split `runScanCycle` (§4.4) and the phone dispatch (§4.5);
rewrite `bgCtx/activeStream` via the policy (§4.6). Leave CYD `loop()`/state machine otherwise
intact.
*Checkpoint:* `pio run -e cyd` builds **and behaves identically** (same STATUS string, same DETS,
same NDJSON, same on-screen flow). Compare flash/RAM to pre-refactor — expect ~0 delta (code moved,
not added). This is the riskiest phase; do it with no other change in flight.

**Phase 2 — headless XIAO-S3 host.** Write `src/xiao_s3/main.cpp` (`setup()` + a tiny `loop()`),
`src/xiao_s3/host_board.cpp` (LED on GPIO21, `batteryPercent()=-1`, `brightness()=100`, link UART
begin on GPIO43/44, `foregroundScan()`/`pipelineAllowed()` from `g_scanActive`), `src/xiao_s3/
host_ui.cpp` (serial-only no-op hooks), `src/xiao_s3/power.cpp` (stub). `setup()` = `Serial`
(USB-CDC), `hostboard::begin()`, `host_log::initSD()`, `sigdb::begin()`, `whitelist::begin()`,
`phone::begin("COINTEL-FLO")`, `pingC5()`. `loop()` = `servicePhoneCommands()` + its minimal
reactions, background/active scan cadence calling `core::runScanCycle()`, `webshare::handle()` when
downloading. No touch, no BOOT nav, no screen.
*Checkpoint:* `pio run -e xiao_s3` builds; on hardware the web app connects over BLE, shows live
detections (DETS), logs NDJSON to the Sense SD, and the XIAO-C5 streams dual-band detections over
the new link pins.

**Phase 3 — DS3231 RTC on the host (§6).** Add `host_common/rtc.{h,cpp}`; extend the core time
source/sink; gate `rtc::begin` to the S3 board (`hostboard::rtcPresent()`), CYD passes false.
*Checkpoint:* `pio run -e cyd` still identical (RTC compiled, inactive); `pio run -e xiao_s3` boots
with a valid clock from row 1; a phone/GPS sync disciplines the RTC; power-cycle keeps the time.

**Phase 4 — SoftAP QR over BLE (§7).** Add STATUS fields for SoftAP SSID/PSK/URL; web app renders
the Wi-Fi join QR. CYD unaffected (keeps its on-screen QR too).
*Checkpoint:* headless Wi-Fi bulk download works end-to-end driven only from the phone.

## 5. Reuse summary

- **Link protocol** `lib/link_protocol/` — untouched, v10, compiled into all four firmwares. No new
  message type, **no `PROTOCOL_VERSION` bump** (this is a host-side refactor + a host-local RTC).
- **C5 scanner firmware** `src/c5/` — reused verbatim; only `pins.h` branches for the XIAO's link
  (GPIO23/24) and GPS (GPIO1) pins. The async time-sliced scan table, RID decode, GPS-in-Status,
  behavioral flags all carry over.
- **Signature DB / sigdb**, **phone GATT**, **webshare SoftAP**, **whitelist**, **logfilter**,
  **NDJSON schema**, **DETS v6**, **STATUS string**, **follow engine**, **categories** — all moved
  into `host_common/` and shared unchanged.
- **Web app** `webapp/` — unchanged except the small Wi-Fi-QR touch-point in §7.

## 6. DS3231 RTC — time model integration (host-side, no protocol bump)

Current model (verified in `src/cyd/main.cpp`): `timeHasSync()` true if `phone::hasTime()` **or**
`g_gpsEpochUtc != 0`; `timeSource()` returns 1 (phone) / 2 (GPS) / 0; `epochLocal()` returns phone
local epoch, else GPS UTC (`g_gpsEpochUtc` latched at `g_gpsEpochAtMs`) **+ `phone::tzOffsetMin()`**
(persisted NVS `cydscan/tzoff`). The `timesync` NDJSON anchor + reader backfill cover rows logged
before any sync.

RTC becomes **source priority 3 and a write-back sink**:

- **New module** `host_common/rtc.{h,cpp}`:
  `bool begin(TwoWire& w)` (probe 0x68, ignore 0x57), `bool valid()`, `uint32_t readUtc()`,
  `void writeUtc(uint32_t epoch)`. Prefer a **small direct-I2C read** of the 7 BCD time registers
  (0x00–0x06) over pulling RTClib, to avoid a dependency and keep flash lean — DS3231 register
  layout is trivial; this is clean-room, no attribution issue. RTClib is an acceptable fallback if
  the direct read is fiddly.
- **Boot:** in `hostboard::begin()` (S3 only) call `rtc::begin(Wire)` on GPIO5/6. If `rtc::valid()`,
  latch a core `g_rtcEpochUtc` + `g_rtcAtMs` (same pattern as the GPS latch) **and** seed the
  system clock (`settimeofday`) so a valid wall clock exists **before** any phone/GPS fix.
- **Priority:** `timeSource()` → 1 phone, else 2 GPS, else **3 RTC**, else 0. `timeHasSync()` adds
  `|| g_rtcEpochUtc != 0`. `epochLocal()` adds an RTC branch (`g_rtcEpochUtc + (millis()-g_rtcAtMs)/1000
  + tzOffsetMin*60`) as the lowest-priority fallback. RTC holds **UTC** → reuse `phone::tzOffsetMin()`
  / NVS `cydscan/tzoff` for local display exactly as GPS does.
- **Discipline (sink):** when phone **or** GPS delivers a fresh UTC, write it back via
  `rtc::writeUtc()` — rate-limited (e.g. first fix after boot, then at most hourly) so a drifting
  RTC is re-corrected from the better source. Priority for *live* reads stays phone > GPS > RTC;
  the RTC is the persistent fallback that survives power-off.
- **Benefit (stress this):** on a bag-carried headless device the log carries **correct timestamps
  from row 1 of every boot**, instead of `epoch:0` + the one-boot `timesync` backfill window. Keep
  the `timesync` anchor + reader backfill as the **never-set** fallback (no RTC fitted / dead
  battery) so no behavior is lost.
- **STATUS:** `timeSource()` already feeds `tsrc` in the STATUS string. Value **3 = RTC** is new —
  the web app treats non-zero `tsrc` as "time known"; optionally give RTC a distinct status-icon
  tint later (web-side only, cosmetic).
- **CYD unchanged:** `rtc.cpp` compiles into the CYD too, but `hostboard::rtcPresent()` is false on
  the CYD and `rtc::begin` is never called there (no DS3231, and the CYD has no free I2C — GPIO22/27
  are the link, GPIO21 is backlight). CYD time behavior is byte-identical to today.

I2C pin pick on the S3: **GPIO5/6 (D4/D5)**, clear of the SD SPI (GPIO7/8/9) and the deferred
camera bus (GPIO10–18/38–48, camera SCCB on 39/40) — no collision now or when the camera lands.

## 7. Headless Wi-Fi download — SoftAP SSID/PSK/QR over BLE

Today `webshare` raises a random-per-device WPA2 SoftAP and the **CYD screen** shows the Wi-Fi-join
QR, then the `http://192.168.4.1` QR once joined (`drawDownloadScreen` + `webshare::wifiQr()`). A
headless host has no screen, so the join credentials must reach the phone another way.

Design (no link-protocol change — this is the CYD→phone STATUS string, which is the host's own
contract):

- `webshare` already exposes `ssid()`, `apPass()`, `url()`, `wifiQr()`. **When `webshare::active()`**
  `pushStatus()` appends new fields to the STATUS string, e.g.
  `apssid=<ssid>;appsk=<psk>;apurl=http://192.168.4.1` (omit/blank when the AP is down so the web
  app knows to hide the QR). The PSK is already random-per-device and NVS-persisted (the SEC-M1
  fix) — it is shown only transiently to the paired phone over the encrypted-intent BLE link, never
  committed.
- **Web app touch-point:** in `webapp/js/ble/status.js` parse `apssid/appsk/apurl`; in the Data
  page's Wi-Fi-download flow render a **`WIFI:T:WPA2;S:<ssid>;P:<psk>;;` QR** for the user to scan
  with their camera to join the AP, then surface the `apurl` link/QR to open the index. The web app
  already renders QR codes, so this is a render + one parse addition, not new infrastructure. (The
  browser can't auto-join Wi-Fi or fetch `http://192.168.4.1` from the HTTPS app — same constraints
  as today — which is exactly why the QR-to-join + separate-tab model is reused.)
- **CYD benefit:** harmless on the CYD (it keeps drawing its own QR); the extra STATUS fields are
  additive and ignored by older parsers.
- `hostui::onDownloadState()` is a no-op on the S3 (the QR lives on the phone); on the CYD it still
  calls `drawDownloadScreen`.

## 8. Cost estimate (flash / RAM)

- **Flash.** All host envs keep `huge_app.csv` (3 MB app) on 4 MB (CYD) / 8 MB (XIAO) flash. The
  XIAO-S3 image is **smaller** than the CYD's: it drops TFT_eSPI, XPT2046, QRCode, and all draw/
  glyph/logo code (`logo.h` alone is 653 lines of bitmap) — on the order of tens to ~100+ KB saved
  — while adding only the tiny RTC reader. Comfortably inside 3 MB. XIAO-C5 image ≈ the DevKit C5
  image (same sources). No env approaches its app-partition ceiling.
- **RAM/heap.** The ESP32-S3 has 512 KB SRAM (vs the CYD's 320 KB, no PSRAM) **plus** 8 MB PSRAM —
  so the host is strictly roomier than the CYD, with headroom to spare. The dominant static buffers
  are unchanged and modest: `g_dets[96]` (96×75 ≈ 7.0 KB), `g_score[96]`, `g_groups[96]`,
  `g_frozenRows[96]` (CYD-UI only — **not** linked on the S3), `g_follow[96]`, `g_flagLabel[96][16]`
  (1.5 KB), two `LogStream` with `seen[400]` each (~7 KB total). NimBLE + Wi-Fi SoftAP + web server
  coexist on the S3 exactly as on the CYD and are the real heap consumers; the S3's larger SRAM
  makes this easier, not harder. Keep all allocations SRAM-only (don't gratuitously move to PSRAM);
  PSRAM is a safety margin, not a requirement.
- **CYD delta from the refactor:** moving code between files changes neither flash nor RAM
  materially. The no-op-refactor checkpoint (Phase 1) must show ~0 delta; investigate if it doesn't
  (usually an accidental extra copy or a newly-inlined-vs-not boundary).

## 9. Risks & unknowns (verify before/while building)

1. **XIAO-C5 board def availability.** `seeed_xiao_esp32c5.json` exists in the newer pioarduino
   platform snapshot but **not** in older ones. Confirm the project's pinned `platform =` "stable"
   zip resolves a snapshot that includes it; if `pio run -e xiao_c5` reports "unknown board",
   either bump the platform or vendor a local `boards/seeed_xiao_esp32c5.json` (copy the known-good
   def: mcu `esp32c5`, variant `XIAO_ESP32C5`, 8 MB).
2. **XIAO-C5 `BOARD_HAS_PSRAM`.** The board def forces `-DBOARD_HAS_PSRAM`. If the specific XIAO C5
   unit lacks PSRAM, PSRAM init at boot can stall/fail. The `src/c5/` firmware doesn't use PSRAM —
   verify a clean boot on the actual board; if it misbehaves, drop the flag via a local board def
   override.
3. **5 GHz antenna** (§2.3) — stock XIAO C5 antenna is 2.4 GHz only; a dual-band u.FL antenna is a
   hard pre-req for the C5's reason-to-exist. Validate 5 GHz beacons actually appear.
4. **XIAO-C5 GPIO0/1 as plain GPIO.** They double as LP/XTAL_32K pins. Fine when no 32 kHz crystal
   is fitted; **test** the GPS RX on GPIO1 (and the link on GPIO23/24) with known traffic before
   trusting them — same "verify the pin" discipline the DevKit's 25/26 caveat teaches.
5. **XIAO-S3 Sense microSD pins/CS** (§2.1) — confirm SCK/MISO/MOSI and especially **CS** from
   Seeed's Sense schematic; handle the possible **CS == GPIO21 vs LED** collision.
6. **DS3231 module hazards** (§2.4) — CR2032 trickle-charge on ZS-042 (fit LIR2032 or remove the
   charge resistor); ignore the AT24C32 at 0x57.
7. **Phase-1 no-op refactor is the highest-risk step** — the CYD must stay byte-for-behavior
   identical (STATUS string field order, DETS v6 layout, NDJSON keys, on-screen flow). Diff STATUS/
   DETS/NDJSON output against a pre-refactor capture.
8. **platformio.ini IDE clobber** (per MEMORY) — the IDE may regenerate `platformio.ini`, strip
   comments, and bump ArduinoJson v6→v7 (which breaks the NDJSON reader). Diff before committing
   and revert any clobber; keep ArduinoJson pinned at `^6.21.5`.
9. **USB VID collision** — XIAO-C5 native, DevKit-C5 native, and the ODID-tx S3 all enumerate as
   `303A:1001`. Identify by `esptool chip-id` before flashing to avoid cross-flashing boards.

## 10. Not in this build

- **No camera.** The XIAO ESP32-S3 Sense OV2640/OV3660 camera and any optical/ALPR/IR-flash
  detection are **deferred**. Only its pins (GPIO10–18, 38–48) are reserved so it can be added later
  with no re-layout. No camera driver, no PSRAM framebuffer, no vision code.
- **No attack/interference features** — unchanged project scope. This build only observes and
  fingerprints RF (active scanning that devices answer by design is fine; no jamming/deauth/
  injection/spoofing/floods/DoS). The new host transmits only for its own BLE link, Wi-Fi SoftAP
  download, and active scan requests.
- **No link-protocol change / no `PROTOCOL_VERSION` bump** — this is a host-side refactor plus a
  host-local DS3231 and a STATUS-string addition.
- **No on-S3 battery gauge** (no exposed divider) — reports `bat=-1`; a future add-on can fill it in.
- **No second-repo split** — one repo, new envs (§1).

---

### Files the implementer will create / modify (repo-relative)

- **Modify:** `platformio.ini` (CYD filter; add `[env:xiao_s3]`, `[env:xiao_c5]`), `src/c5/pins.h`
  (board branch), `hardware/PINOUT.md` (new §: both XIAO boards, DS3231, 5 GHz antenna caveat).
- **Move into `src/host_common/`:** `sigdb.{h,cpp}`, `phone.{h,cpp}`, `webshare.{h,cpp}`,
  `whitelist.{h,cpp}`, `logfilter.h`, `power.h`; plus the new carved files `host_link.{h,cpp}`,
  `host_core.{h,cpp}`, `host_log.{h,cpp}`, `host_status.{h,cpp}`, `host_phone_cmds.{h,cpp}`,
  `host_board.h`, `host_ui.h`, `rtc.{h,cpp}`.
- **Keep CYD-only (edit to call `host_common`):** `src/cyd/main.cpp` (UI + state machine + board
  layer impls of `host_board`/`host_ui`), `src/cyd/touch.{h,cpp}`, `src/cyd/logo.h`,
  `src/cyd/power.cpp`.
- **Create `src/xiao_s3/`:** `main.cpp` (headless setup/loop), `host_board.cpp`, `host_ui.cpp`,
  `power.cpp` (stub).
- **Validate:** `pio run -e cyd` (identical), `pio run -e xiao_s3`, `pio run -e xiao_c5`,
  `pio run -e c5` (unchanged).

# Pin Reference — cyd-scanner

Quick-access pin tables for the two boards used in this project. Derived from the
datasheets and pinout diagrams under [hardware/boards/](boards/). Keep this file
in sync when wiring changes.

- CYD pinout diagram: [cyd_esp32-2432S028r_pinout-1.png](boards/cyd_esp32-2432S028r/pinout/cyd_esp32-2432S028r_pinout-1.png)
- ESP32-C5 pinout diagram: [esp32-c5-devkit_pinout.jpg](boards/esp32-c5-devkit/pinout/esp32-c5-devkit_pinout.jpg)
- ESP32-C5 datasheet: [esp32-c5_datasheet_en.pdf](boards/esp32-c5-devkit/datasheets/esp32-c5_datasheet_en.pdf)
- **Full wiring diagram (data link + battery power + GPS + battery sense) — CURRENT BUILD:**
  [cyd-scanner_wiring_diagram_v2.png](diagrams/cyd-scanner_wiring_diagram_v2.png)
  (labeled source: [cyd-scanner_diagram_v2_labeled.svg](diagrams/cyd-scanner_diagram_v2_labeled.svg)).
  The earlier [cyd-scanner_wiring_diagram_labeled.png](diagrams/cyd-scanner_wiring_diagram_labeled.png)
  predates the GPS, battery-sense divider, and the C5-5V isolation switch.

---

## 1. ESP32-2432S028R "CYD" (Cheap Yellow Display)

**SoC:** ESP32-D0WD (dual-core Xtensa LX6, 240 MHz) · Wi-Fi 2.4 GHz b/g/n · BT Classic + BLE 4.2
**Display:** 2.8" 240×320 TFT, ILI9341 controller (some units ship ST7789), SPI
**Touch:** XPT2046 resistive controller, on a **separate** SPI bus
**PlatformIO board id:** `jczn_2432s028r`

> Note: this is a single-core-generation ESP32 with **2.4 GHz Wi-Fi only**. 5 GHz
> scanning is handled by the ESP32-C5 (below).

### Display (ILI9341 / ST7789 — VSPI)

| Signal      | GPIO  | Notes                                  |
|-------------|-------|----------------------------------------|
| TFT_MOSI    | 13    | SPI data to display                    |
| TFT_MISO    | 12    | SPI data from display                  |
| TFT_SCLK    | 14    | SPI clock                              |
| TFT_CS      | 15    | Chip select                            |
| TFT_DC      | 2     | Data/command                           |
| TFT_RST     | -1/EN | Tied to board reset on most units      |
| TFT_BL      | 21    | Backlight LED (active high)            |

### Touch (XPT2046 — bit-banged / separate HSPI)

| Signal            | GPIO | Notes                        |
|-------------------|------|------------------------------|
| TOUCH_CLK (TP CLK)| 25   |                              |
| TOUCH_CS  (TP CS) | 33   |                              |
| TOUCH_MOSI(TP DIN)| 32   |                              |
| TOUCH_MISO(TP OUT)| 39   | Input-only pin               |
| TOUCH_IRQ (TP IRQ)| 36   | Input-only pin (pen down IRQ)|

### microSD card (TF slot — shared SPI)

| Signal        | GPIO | Notes           |
|---------------|------|-----------------|
| SD_CS (DAT3)  | 5    | Card detect/CS  |
| SD_MOSI (CMD) | 23   |                 |
| SD_SCK  (CLK) | 18   |                 |
| SD_MISO (DAT0)| 19   |                 |

### On-board peripherals

| Peripheral           | GPIO       | Notes                                  |
|----------------------|------------|----------------------------------------|
| RGB LED — Red        | 4          | Active low                             |
| RGB LED — Green      | 16         | Active low                             |
| RGB LED — Blue       | 17         | Active low                             |
| Audio out / speaker  | 26         | DAC → on-board amp (2p header). **No speaker fitted in this project → GPIO26 is FREE** (full GPIO, DAC2, ADC2_CH9). |
| BOOT button          | 0          | Strapping pin                          |
| RST button           | EN         | Hardware reset                         |

### Exposed connectors (free GPIO for the C5 link + sensors)

| Connector      | Pins                                                        |
|----------------|------------------------------------------------------------|
| P1 Serial (4p) | VIN · TX (GPIO1/U0TXD) · RX (GPIO3/U0RXD) · GND            |
| P3 / IO1 (4p)  | GND · GPIO35 (input-only, ADC1_CH7) · GPIO22 (SCL) · GPIO21 (SDA) |
| P5 / IO2 (4p)  | GND · GPIO22 (SCL) · GPIO27 (TOUCH7, ADC2) · 3V3          |

> **GPIO allocation** (not consumed by display/touch/SD): **22/27** carry the C5 UART link
> (§3); **GPIO35** (input-only, ADC1_CH7) now carries the **battery-sense divider** (§4/§5a) —
> no longer free. Still free: **GPIO26** (no speaker, and the TP4056 CHRG pad is not wired) and
> the serial header (1, 3). GPIO21 doubles as the backlight. GPIO35/34/39/36 are input-only.

---

## 2. ESP32-C5-DevKitC-1 (DOIT ESPC5-32)

**SoC:** ESP32-C5, 32-bit RISC-V single-core @ 240 MHz
**Radios:** Wi-Fi 6 (802.11 a/b/g/n/ax) **dual-band 2.4 GHz + 5 GHz** · Bluetooth LE 5.0 · IEEE 802.15.4 (Zigbee 3.0 / Thread 1.3)
**Memory:** 384 KB SRAM · 320 KB ROM (+ external flash)
**Peripherals:** 29 GPIO · 2× SPI · 2× UART · 1× I2C · RMT · LED PWM (6 ch) · 1× 12-bit ADC (6 ch) · TWAI/CAN-FD · USB-Serial/JTAG · ETM · MCPWM

> The C5's **5 GHz Wi-Fi 6** radio is the reason it is paired with the CYD: it
> covers the bands the ESP32 in the CYD cannot see, and adds 802.15.4 for
> Zigbee/Thread-based devices.

### Power / control pins

| Pin  | Function                     |
|------|------------------------------|
| 3V3  | 3.3 V power                   |
| 5V0  | 5 V input (USB)              |
| RST  | Reset                        |
| GND  | Ground (multiple)            |
| BOOT | GPIO28 area button           |

### GPIO — left header (top → bottom)

| GPIO | Key alternate functions                                                       |
|------|-------------------------------------------------------------------------------|
| 2    | MTMS · LP_GPIO2 · LP_UART_RTSN · **LP_I2C_SDA** · ADC1_CH1 · FSPIQ            |
| 3    | MTDI · LP_GPIO3 · LP_UART_CTSN · **LP_I2C_SCL** · ADC1_CH2                    |
| 0    | LP_GPIO0 · LP_UART_DTRN · XTAL_32K_P                                           |
| 1    | LP_GPIO1 · LP_UART_DSRN · XTAL_32K_N · ADC1_CH0                                |
| 6    | LP_GPIO6 · ADC1_CH5 · FSPICLK                                                  |
| 7    | FSPID · SDIO_DATA1                                                             |
| 8    | PAD_COMP0 · SDIO_DATA0 · **strapping pin**                                     |
| 9    | PAD_COMP1 · SDIO_CLK · **strapping pin**                                       |
| 10   | FSPICS0 · SDIO_CMD                                                             |
| 25   | GPIO25 ⚠️ see pin 25/26 note below                                            |
| 26   | GPIO26 ⚠️ see pin 25/26 note below                                            |

### GPIO — right header (top → bottom)

| GPIO | Key alternate functions                                                       |
|------|-------------------------------------------------------------------------------|
| 11   | **U0TXD** (default console TX)                                                 |
| 12   | **U0RXD** (default console RX)                                                 |
| 24   | GPIO24                                                                         |
| 23   | GPIO23                                                                         |
| 15   | GPIO15                                                                         |
| 27   | GPIO27 (on-board RGB LED on many units)                                        |
| 4    | MTCK · LP_GPIO4 · LP_UART_RXD · ADC1_CH3 · FSPIHD                              |
| 5    | MTDO · LP_GPIO5 · LP_UART_TXD · ADC1_CH4 · FSPIWP                              |
| 28   | GPIO28                                                                         |
| 14   | **USB_D+** · SDIO_DATA2                                                        |
| 13   | **USB_D-** · SDIO_DATA3                                                        |

> **Strapping pins:** GPIO8, GPIO9 (avoid holding at boot).
> **USB-Serial/JTAG:** GPIO13 (D-) / GPIO14 (D+) — leave free for programming.
> **Console UART0:** GPIO11 (TX) / GPIO12 (RX).
> **GPS (wired):** GPIO15 (NEO-6M TXD→C5 RX) / GPIO23 (C5 TX→NEO-6M RXD, config-only). See §5b —
> the GPS is on the hardware but its firmware is not written yet (protocol-v9 gated).

> ⚠️ **Pin 25 / 26 caveat — UNVERIFIED.** On this physical board, GPIO25 and GPIO26
> appear **swapped** relative to most online ESP32-C5 DevKit documentation. The
> pinout JPEG in this repo has been hand-edited to match the numbers **printed on
> the actual board**, and this table follows the board's printing. It is *not*
> confirmed which is correct: the board's silkscreen could be mislabeled, or the
> online docs could be wrong — the pins have **not been tested**. Treat 25/26 as
> the board prints them for now, but **if either pin is actually used, test it
> first** (e.g. toggle a known GPIO and verify with a meter/LED) before trusting
> the assignment. Prefer other GPIO for critical signals until verified.

---

## 3. Inter-board link (CYD ⇄ ESP32-C5)

The CYD is the host/UI; the ESP32-C5 is a co-processor for 5 GHz + 802.15.4.
The link is a dedicated UART (**not** UART0/console on either side, so USB serial
logging stays free). These pins are **validated end to end** (the CYD commands the C5,
which streams detections back); they match the firmware defaults (`lib/link_protocol/`),
change both together.

| Purpose         | CYD side                  | ESP32-C5 side          |
|-----------------|---------------------------|------------------------|
| UART link TX→RX | GPIO22 (P3/IO1, expansion)| GPIO4 (LP_UART_RXD)    |
| UART link RX←TX | GPIO27 (P5/IO2, expansion)| GPIO5 (LP_UART_TXD)    |
| Common ground   | GND (expansion)           | GND                    |
| 5V power feed   | VIN (P1)                  | 5V0                    |

> Both CYD pins (GPIO22, GPIO27) are output-capable and free (GPIO35 is input-only,
> so it can serve as an RX but never TX). The ESP32's UART matrix lets any UART route
> to any GPIO. On the C5, GPIO4/5 are the LP_UART RX/TX. I2C over GPIO22 (SCL) /
> GPIO21 or GPIO27 on the CYD expansion headers is an alternative transport if UART
> proves tight. **Confirm wiring and test before relying on these assignments.**

---

## 4. Power subsystem — on-board battery (current rig)

The scanner runs from a **self-contained LiPo battery pack**, making it fully portable
(no external USB needed in the field). See the current-build diagram
[cyd-scanner_wiring_diagram_v2.png](diagrams/cyd-scanner_wiring_diagram_v2.png).

**Two switches** (both SPDT slide switches):
- **SW1 — master on/off**, inline on the charger's positive output (TP4056 `OUT+` → boost `+in`).
  Cuts the whole rig. The battery-sense divider (§5a) taps the *switched* side, so it only draws
  when SW1 is on.
- **SW2 — C5 5 V isolation**, inline on the boost `OUT+` → **C5 5V** leg only. The CYD's VIN is
  fed directly from boost `OUT+` (not through SW2). Opening SW2 isolates the C5 from the shared 5 V
  node, so when the CYD is powered from its own USB (e.g. while flashing it) that 5 V **cannot
  back-feed the C5**.

### Components

| Part                              | Role                                                                 |
|-----------------------------------|----------------------------------------------------------------------|
| 5000 mAh 3.7 V LiPo cell          | Energy store (18.5 Wh)                                                |
| TP4056 Type-C USB charger (5V 1A) | Li-ion charge controller + protection; charge via its own USB-C port |
| **SW1** SPDT slide switch         | Master power on/off, inline on the charger's positive output         |
| **SW2** SPDT slide switch         | C5 5 V isolation, inline on the boost-OUT+ → C5 5V leg                |
| 1.5 A boost converter (3.7→5 V)   | Steps the cell up to the 5 V both boards need                        |

### Wiring (power path)

```
Battery + ──┐                             SW1 (master)
Battery − ──┼── TP4056 B+ / B−            ┌──────┐
            │   TP4056 OUT+ ─────────────►│ in   │──► Boost  +in   (and → §5a divider)
            │   TP4056 OUT− ────────────────────────► Boost  −in
            │                             └──────┘
            │                                            ┌──► CYD VIN (P1)   (direct)
            │   Boost OUT+ (5 V) ────────────────────────┤
            │                                            └──[ SW2 ]──► C5 5V (switchable)
            │   Boost OUT− (GND) ──┬──► CYD GND
            └──────────────────────┴──► C5  GND  (also the link common ground)
```

| From                | To                              | Notes                                       |
|---------------------|---------------------------------|---------------------------------------------|
| Battery `+` / `−`   | TP4056 `B+` / `B−`              | The cell; charges via TP4056 USB-C          |
| TP4056 `OUT+`       | **SW1** → Boost `+` input       | SW1 breaks the +rail = master on/off        |
| TP4056 `OUT−`       | Boost `−` input                 |                                             |
| Boost `OUT+` (5 V)  | CYD **VIN** (direct)            | CYD is always fed when SW1 is on            |
| Boost `OUT+` (5 V)  | **SW2** → C5 **5V**             | SW2 lets you cut the C5's feed alone        |
| Boost `OUT−` (GND)  | CYD **GND** + C5 **GND**        | Common ground (shared with the UART link)   |

### Rules / safety

- **Feed 5 V only** — the 5 V boost output drives each board's VIN/5V pin. Each board keeps its
  **own** 3.3 V regulator; **never tie the two 3.3 V rails together** (the CYD's AMS1117-3.3
  can't power a second Wi-Fi SoC → brownouts).
- **One 5 V source at a time.** Never let a board's USB 5 V and the boost output drive the same
  node at once. With the two switches this is now per-board:
  - **Flashing the C5:** open **SW2** (isolates C5 5V from the boost/CYD node). The C5 then runs
    only off its own USB — no back-feed either way. SW1/CYD can stay as-is.
  - **Flashing the CYD:** switch the battery **OFF** at **SW1** (the CYD's VIN is wired straight to
    boost OUT+, so there's no CYD-side isolation switch). Its USB then powers the CYD alone; SW2
    anyway keeps that USB 5 V off the C5.
  - When in doubt, SW1 **OFF** before plugging any programming USB is always safe.
- **Charge-while-run:** the TP4056 can charge the cell from its USB-C while the rig is powered;
  its OUT pads still source the load. Keep total draw within the TP4056's 1 A path.
- Current budget: both ESP32s on Wi-Fi/BLE + the display can peak ~0.7–1 A at 5 V, which is
  within the 1.5 A boost rating. At ~0.5 A average the 5000 mAh cell gives on the order of
  several hours of runtime.

### Alternative: single-USB (bench / in-car) power

The battery pack is removable from the picture — the rig can still be powered from **one USB-C
into the CYD** (no battery, no separate cable to the C5):

- Wire CYD **VIN** (P1, = USB 5 V) → C5 **5V**, GND↔GND (already shared by the link). Plug the
  CYD's USB-C into a car charger / power bank rated **≥1 A**.
- Same "feed 5 V only / one source at a time" rules apply — don't combine this with the battery
  boost output live at the same time.

---

## 5. Add-ons — battery monitor (BUILT) + GPS (WIRED, firmware pending)

> ✅ **Both add-ons are now on the hardware** (matching the current-build diagram, §4 top). The
> battery monitor is **built and firmware is live**; the GPS is **wired but not yet read in firmware**.
> Full wiring, divider math, and part notes: **[addon-wiring.md](addon-wiring.md)**.
>
> 🚫 **Do NOT change the link protocol for the GPS yet.** Build C's GPS-over-link forwarding needs a
> `PROTOCOL_VERSION` bump (8 → 9) — **leave `lib/link_protocol/link_protocol.h` and the GPS firmware
> alone until the owner explicitly says to build this.** Until then, phone GPS (over BLE) remains the
> only position source the firmware uses.

**Layout (Build C):** the battery monitor on the CYD; the GPS on the C5 (which has the spare GPIO),
to be forwarded to the CYD over the existing link once the GPS firmware is written.

### 5a. CYD — battery monitor (BUILT — voltage-only)

| Signal          | CYD pin   | Wiring                                                         |
|-----------------|-----------|---------------------------------------------------------------|
| Battery voltage | **GPIO35**| 100 kΩ/100 kΩ divider midpoint off **switched Vbat** (SW1 side); `analogReadMilliVolts(35)×2`. Only free ADC1 pin (ADC1 is Wi-Fi-safe). |
| ~~Charge status~~ | ~~GPIO26~~ | **NOT wired** — the TP4056 CHRG pad is not broken out on this build, so there is no USB-charge-detect input. GPIO26 stays free. |

**Firmware (live):** `src/cyd/power.{h,cpp}` reads GPIO35, smooths it, and maps the LiPo resting-voltage
curve to 0–100 %. The CYD **status bar** shows a battery glyph (fill + colour: green >50 %, yellow
20–50 %, red <20 %; hollow grey when no cell / on USB), and the phone **STATUS** string carries
`bat=<pct>` (−1 = no battery) for the web app's app-bar battery indicator. CYD-only; **no protocol/C5
change** (STATUS is the CYD→phone string, not the link `PROTOCOL_VERSION`). Charge/not-charging state
is **not** available (no CHRG pin) — the glyph shows level only.

### 5b. C5 — GY-NEO6MV2 GPS (WIRED — firmware pending)

| GY-NEO6MV2 | C5 pin                | Wiring                                                        |
|------------|-----------------------|--------------------------------------------------------------|
| VCC        | **5V0**               | breakout has its own 3.3 V LDO                               |
| GND        | **GND**               | common ground (shared with the link / boost)                |
| TXD        | **GPIO15** (C5 UART RX)| the NMEA data wire (9600 8N1); 3.3 V logic                  |
| RXD        | **GPIO23** (C5 UART TX)| config-only; the module needs no commands for read-only NMEA |

GPIO15/23 are plain, broken-out C5 right-header pins, clear of the link (4/5), console (11/12),
USB-JTAG (13/14), strapping (8/9), RGB LED (27), BOOT (28), and the unverified 25/26. **The pins are
connected on the board, but no firmware reads them yet** — forwarding the fix CYD-ward is a future
protocol-v9 change, not to be started until told (see the warning above).

### Pin-budget note

After the display/touch/SD and the C5 link, the CYD had only **two** free GPIO — **GPIO35**
(input-only, the only free ADC1 pin) and **GPIO26**. The battery monitor now uses **GPIO35**;
because CHRG wasn't broken out, **GPIO26 remains the one free CYD GPIO**. Putting GPS on the C5
(Build C) is what let the CYD keep its ADC pin for battery sense.

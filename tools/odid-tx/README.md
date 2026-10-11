# odid-tx — OpenDroneID BLE transmitter (bench test tool)

A standalone, throwaway bench tool that makes a spare **ESP32-S3** impersonate a drone's
Remote-ID beacon, so the cyd-scanner's Remote-ID decode path can be validated **without an
actual drone or a commercial ODID transmitter**.

It is **not** part of the two-board scanner firmware — it lives in its own PlatformIO project
(its own `platformio.ini`, its own env) so it can't be pulled into the `cyd`/`c5` builds or
clobbered by the IDE regenerating the root `platformio.ini`.

## What it does

Broadcasts **BLE4 legacy** advertisements carrying Service Data under 16-bit UUID `0xFFFA`,
framed as `[0x0D app-code][counter][25-byte ODID message]` — exactly what the scanner's C5
decoder consumes (`src/c5/remoteid.cpp` `ingest()`). A legacy advert is capped at 31 bytes,
which one such element fills exactly, so **one advert carries one message**; the sketch rotates

```
Basic ID -> Location -> System -> Operator ID
```

every 750 ms under a single stable BLE address, so the scanner's per-MAC accumulator
(`remoteid::RidEntry.have`) fills in a full emitter picture over ~3 s. The synthetic flight
values (UAS ID, drone + operator lat/lon, height, speed, heading) are constants at the top of
`src/main.cpp` — edit freely.

## Hardware

A spare **ESP32-S3-DevKitC-1** (tested on the N16R8 variant). Any ESP32 with BLE would do —
every variant supports BLE4 legacy advertising, which is all the scanner captures (the C5 is
deliberately **not** in BT5 extended-scan mode, for Wi-Fi/BLE coexistence).

## Build & flash

```bash
pio run -d tools/odid-tx                               # build
pio run -d tools/odid-tx -t upload --upload-port COMxx # flash (S3 NATIVE USB port, VID:PID 303A:1001)
pio device monitor -p COMxx -b 115200                  # watch [TX] lines
```

> ⚠ The S3's native-USB port shares VID:PID `303A:1001` with the C5's native port, and its
> CH340 UART port shares `1A86:7523` with the CYD. With all boards plugged in, **identify the
> S3 by chip type before uploading** (`esptool --port COMxx chip-id` → `ESP32-S3`) and pass an
> explicit `--upload-port`, or you may flash the wrong board.

## Verifying against the scanner

With the transmitter running, watch the scanner consoles:

- **C5** console → `[RID] <mac> LOC lat=… lon=… h=…m alt=…m spd=…cm/s hdg=…` (increment-1 decode).
- **CYD** console (requires both scanner boards on **protocol v8**, increment 2) →
  `[RID] <mac> uas=… lat=… lon=…` — proof the `Reply::RemoteId` frame crossed the link.

## Scope / ethics

This emits a **standards-compliant** ODID broadcast purely as receive-target test traffic for
our own bench validation. It does not jam, spoof, deauth, flood, or otherwise interfere with any
third-party device — it stays well inside the project's no-attack/no-interference scope.

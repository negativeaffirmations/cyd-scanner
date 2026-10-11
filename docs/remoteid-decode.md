# Remote-ID (ASTM F3411 / OpenDroneID) full decode

Status: **in progress** on branch `remoteid-decode`. Supersedes the presence-only
`FLAG_BLE_ODID` detector with an actual decode of the drone/operator broadcast.

## Goal

Today the C5 only flags *presence* of a Remote-ID emitter (`FLAG_BLE_ODID`, set when a BLE
advert carries service data under 16-bit UUID `0xFFFA`, or a Wi-Fi beacon carries the ODID
vendor element). This feature decodes the actual broadcast so the scanner can show:

- **UAS ID** (drone serial / registration) and UA type — from the *Basic ID* message
- **Drone position** (lat/lon, height, geodetic altitude, horizontal speed, track) — *Location*
- **Operator position** (lat/lon) — *System*
- **Operator ID** string — *Operator ID*

This is the Remote-ID feature's own "phase 2" (HANDOFF outstanding #5), not the
signature-matching Phase 2 (long shipped).

## Wire format (OpenDroneID / ASTM F3411, verified against opendroneid-core-c)

Every message is a fixed **25 bytes**. Byte 0 is a header: low nibble = protocol version,
high nibble = **message type**. Multi-byte integers are **little-endian**; strings are ASCII,
left to right, NUL/space padded.

Message types: `0` Basic ID · `1` Location/Vector · `2` Auth · `3` Self-ID · `4` System ·
`5` Operator ID · `0xF` Message Pack.

**Basic ID (0):** byte1 = `(IDType<<4)|UAType`; bytes 2..21 = 20-byte UAS ID. IDType: 1 =
serial (CTA-2063-A), 2 = CAA registration, 3 = UTM UUID.

**Location (1):** byte1 flags (`bit0` SpeedMult, `bit1` EWDirection, `bit2` HeightType, bits
4–7 Status); byte2 Direction; byte3 SpeedHorizontal; byte4 SpeedVertical(int8); bytes 5..8
`Latitude` int32; 9..12 `Longitude` int32; 13..14 AltBaro u16; 15..16 AltGeo u16; 17..18
Height u16; 19 H/V accuracy nibbles; 20 speed/baro accuracy; 21..22 timestamp; 23 ts-acc.
Decoding: **lat/lon = int32 / 1e7 deg**; **alt/height = enc·0.5 − 1000 m**; **horiz speed =**
`SpeedMult? enc·0.75 + 255·0.25 : enc·0.25` m/s; **track =** `EWDirection? dir+180 : dir` deg.

**System (4):** byte1 (OperatorLocationType bits0–1, ClassificationType bits2–4); bytes 2..5
`OperatorLatitude` int32; 6..9 `OperatorLongitude` int32; 10..11 AreaCount; … 18..19 operator
alt (enc·0.5−1000); 20..23 timestamp (u32, since 2019-01-01). Operator lat/lon = int32 / 1e7.

**Operator ID (5):** byte1 OperatorIdType; bytes 2..21 = 20-byte Operator ID string.

**Message Pack (0xF):** byte1 = single msg size (25), byte2 = count, then `count`×25-byte
messages. BT5 Long Range / extended advertising uses a pack; BT4 legacy sends one message
per advert.

### Transports

- **BLE 4 legacy** (implemented first): Service Data – 16-bit UUID `0xFFFA`. After the UUID,
  the payload is `[0x0D AD-app-code][msg-counter][25-byte message]`. NimBLE's `getServiceData()`
  returns the bytes after the UUID, i.e. starting at `0x0D`.
- **BLE 5 extended / Long Range**: same `0xFFFA` service data but a Message Pack; needs the
  scanner in extended-scan mode (coexistence risk — see below).
- **Wi-Fi Beacon** vendor IE (OUI `FA-0B-BC`, already matched in `promisc.cpp`) and **Wi-Fi NaN**:
  carry a Message Pack. Decode path is shared with BLE once the 25-byte messages are extracted.

## Increment plan

1. **C5 decode + serial readout (this increment, no protocol change).** `src/c5/remoteid.*`:
   decode the message types above from `0xFFFA` BLE service data (single message, pack, and the
   `0x0D` legacy framing), accumulate per emitter MAC in a small bounded table (TTL-expired),
   and print decoded fields to the USB console. Bench-testable with an ODID transmitter/sim
   (e.g. opendroneid/transmitter-linux, or a drone) — no CYD/link change, safe to flash C5 alone.
2. **Link frame + CYD store (protocol v8). — DONE (2026-10-11, both boards flashed + validated).**
   Added `Reply::RemoteId = 0x91` + the `RemoteId` struct (below); the C5 emits one per active emitter
   in its scan-dump burst (mutex-guarded `remoteid::snapshot()`); the CYD keys it to the matching
   `Detection` by MAC (`g_rids[8]`) + a temporary serial `[RID]` readout. **Bumped PROTOCOL_VERSION to
   8 → both boards flashed together.** Validated end-to-end against `tools/odid-tx/` (a spare ESP32-S3
   broadcasting synthetic ODID): the transmitted flight values round-trip to matching C5 + CYD `[RID]`
   serial lines. The temporary CYD readout is replaced by increment 3's real UI.
3. **CYD UI + NDJSON log. — DONE (2026-10-11, CYD flashed).** A `-- Remote-ID --` detail block (UAS
   ID, drone + operator lat/lon, height, geo-alt, speed, heading) on the device-detail screen (reused
   by the scanner detail + category drill-down), shown per value-sentinel; a per-emitter `"evt":"rid"`
   NDJSON line (rate-limited ~once/30 s); temporary serial readout removed. CYD-only, no protocol bump.
4. **Web app map. — DONE (2026-10-11).** The CYD appends a RID block to the DETS phone stream
   (`R:<ridCount>` + one row per emitter; DETS v6, CYD→phone, no link bump). The web app plots the
   drone + operator positions (green drone marker, operator marker, connecting line) on the
   device-modal map and the Map page — live from the stream and from logged `"evt":"rid"` lines —
   and shows a Remote-ID section in the device modal. **Remote-ID decode complete (increments 1–4).**

### Planned `RemoteId` link struct (increment 2)

```c
struct RemoteId {            // Reply::RemoteId (0x91) payload; keyed to a Detection by mac
  uint8_t  mac[6];           // emitter BLE MAC (matches Detection.mac)
  uint8_t  have;             // RID_HAVE_* bits decoded so far
  uint8_t  idType, uaType;   // Basic ID
  int32_t  droneLat, droneLon;       // Location, deg * 1e7 (0 = unknown)
  int32_t  operatorLat, operatorLon; // System,   deg * 1e7 (0 = unknown)
  int16_t  heightM, altGeoM;         // Location, metres (-1000 = unknown)
  uint16_t speedCms, headingDeg;     // Location (headingDeg 0-359; 361 = unknown)
  char     uasId[21];        // Basic ID UAS ID
  char     operatorId[21];   // Operator ID
};   // 75 bytes packed
```

## Coexistence note

BT5 extended-scan ODID capture must not reintroduce the promiscuous-RX starvation the project
already fixed ([[ieee802154-optin-ble-coexistence]]): BLE is paused during the passive Wi-Fi
promiscuous windows. Decode runs inside the existing active BLE scan callback (no new scan), so
increment 1 adds no radio-time cost. Enabling extended advertising for packs is a later,
measured change — check it against the BLE/15.4 coexistence rule before shipping.

## References

- opendroneid-core-c (`libopendroneid/opendroneid.h` encoded structs) — ASTM F3411 / ASD-STAN
  prEN 4709-002. Add formal attribution in `docs/references.md` if any code is ported (none so
  far — this is a clean-room decode from the documented layout).

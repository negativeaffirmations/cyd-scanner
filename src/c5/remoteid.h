// remoteid.h — ASTM F3411 / OpenDroneID broadcast decode (C5).
//
// Clean-room decode of the documented OpenDroneID over-the-air message layout (see
// docs/remoteid-decode.md). Increment 1: decode from BLE 0xFFFA service data, accumulate per
// emitter MAC, print to the USB console. No link-protocol involvement yet (that's increment 2,
// which adds Reply::RemoteId); this module just owns the decode + the per-emitter table.
#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace remoteid {

// Which message types have been decoded for an emitter (RidEntry.have bits).
enum RidHave : uint8_t {
  RID_HAVE_BASICID  = 1 << 0,
  RID_HAVE_LOCATION = 1 << 1,
  RID_HAVE_SYSTEM   = 1 << 2,
  RID_HAVE_OPID     = 1 << 3,
};

constexpr int32_t RID_LATLON_UNKNOWN = 0;      // 0,0 treated as "no fix"
constexpr int16_t RID_ALT_UNKNOWN    = -1000;  // encoded alt/height sentinel (enc 0 -> -1000 m)
constexpr uint16_t RID_HEADING_UNKNOWN = 361;

// Accumulated Remote-ID state for one emitter, merged across its message types.
struct RidEntry {
  uint8_t  mac[6];
  uint8_t  have;
  uint8_t  idType, uaType;
  int32_t  droneLat, droneLon;        // deg * 1e7
  int32_t  operatorLat, operatorLon;  // deg * 1e7
  int16_t  heightM, altGeoM;          // metres
  uint16_t speedCms;                  // horizontal speed, cm/s
  uint16_t headingDeg;                // track, 0..359 (361 = unknown)
  char     uasId[21];
  char     operatorId[21];
  uint32_t lastMs;                    // millis() of the last ingested message
};

// Decode the OpenDroneID bytes carried in a BLE 0xFFFA Service Data element (the bytes NimBLE
// returns after the UUID: [0x0D][counter][25-byte message], a bare 25-byte message, or a
// Message Pack). Updates/creates the per-MAC table entry and prints decoded fields to Serial.
void ingest(const uint8_t mac[6], const uint8_t* svcData, size_t len);

// Drop entries not updated within ttlMs (call periodically from the C5 loop).
void expire(uint32_t ttlMs);

// Table access (for increment 2's Reply::RemoteId emission).
int             count();
const RidEntry* at(int i);

}  // namespace remoteid

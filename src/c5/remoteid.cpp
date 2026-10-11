// remoteid.cpp — ASTM F3411 / OpenDroneID broadcast decode (C5). See remoteid.h / docs/remoteid-decode.md.
#include "remoteid.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace remoteid {
namespace {

constexpr int      RID_MAX   = 8;        // concurrent emitters tracked (few drones in range)
constexpr uint8_t  ODID_MSG_SIZE = 25;
constexpr uint8_t  AD_APP_CODE   = 0x0D; // BLE ODID "AD Application Code" preceding the message
constexpr uint32_t LOC_PRINT_GAP = 1000; // throttle Location serial lines to ~1/s per emitter
constexpr uint32_t RID_TTL_MS    = 30000;// drop an emitter unseen this long

RidEntry g_tab[RID_MAX];
int      g_count = 0;

// g_tab/g_count are touched from two tasks: ingest()/expire() on the NimBLE scan-callback task and
// snapshot() on the Arduino loop task (streamTable). A FreeRTOS mutex (not a portMUX critical
// section) because ingest() prints to Serial while holding it. Lazily created via a thread-safe
// function-local static so no setup() hook is needed.
SemaphoreHandle_t ridMux() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}
struct RidLock {
  RidLock()  { xSemaphoreTake(ridMux(), portMAX_DELAY); }
  ~RidLock() { xSemaphoreGive(ridMux()); }
};

// Caller must hold the lock.
void expireLocked(uint32_t ttlMs) {
  uint32_t now = millis();
  int w = 0;
  for (int i = 0; i < g_count; i++)
    if (now - g_tab[i].lastMs <= ttlMs) { if (w != i) g_tab[w] = g_tab[i]; w++; }
  g_count = w;
}

// Little-endian readers.
int32_t rd32(const uint8_t* p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

// Copy a fixed-width ODID ASCII field into a NUL-terminated C string, trimming trailing
// spaces/NULs. `dstCap` includes the NUL.
void copyId(char* dst, size_t dstCap, const uint8_t* src, size_t n) {
  if (n > dstCap - 1) n = dstCap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
  for (int i = (int)n - 1; i >= 0 && (dst[i] == ' ' || dst[i] == 0); i--) dst[i] = 0;
}

RidEntry* findOrAdd(const uint8_t mac[6]) {
  uint32_t now = millis();
  for (int i = 0; i < g_count; i++)
    if (memcmp(g_tab[i].mac, mac, 6) == 0) return &g_tab[i];
  RidEntry* e;
  if (g_count < RID_MAX) {
    e = &g_tab[g_count++];
  } else {  // full: evict the least-recently-updated
    int o = 0;
    for (int i = 1; i < g_count; i++) if (g_tab[i].lastMs < g_tab[o].lastMs) o = i;
    e = &g_tab[o];
  }
  memset(e, 0, sizeof(*e));
  memcpy(e->mac, mac, 6);
  e->heightM = e->altGeoM = RID_ALT_UNKNOWN;
  e->headingDeg = RID_HEADING_UNKNOWN;
  return e;
}

void macStr(const uint8_t m[6], char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Decode one 25-byte ODID message into `e`. Returns the message type (high nibble of byte 0).
int decodeMessage(RidEntry* e, const uint8_t* m) {
  int type = m[0] >> 4;
  switch (type) {
    case 0: {  // Basic ID
      e->idType = m[1] >> 4;
      e->uaType = m[1] & 0x0F;
      copyId(e->uasId, sizeof(e->uasId), m + 2, 20);
      e->have |= RID_HAVE_BASICID;
      break;
    }
    case 1: {  // Location / Vector
      uint8_t fl = m[1];
      bool speedMult = fl & 0x01, ewDir = fl & 0x02;
      uint8_t encDir = m[2], encSpd = m[3];
      e->headingDeg = (uint16_t)(ewDir ? encDir + 180 : encDir) % 360;
      float spd = speedMult ? (encSpd * 0.75f + 255 * 0.25f) : (encSpd * 0.25f);  // m/s
      e->speedCms = (uint16_t)(spd * 100.0f + 0.5f);
      e->droneLat = rd32(m + 5);
      e->droneLon = rd32(m + 9);
      e->altGeoM  = (int16_t)(rd16(m + 15) * 0.5f - 1000.0f);
      e->heightM  = (int16_t)(rd16(m + 17) * 0.5f - 1000.0f);
      e->have |= RID_HAVE_LOCATION;
      break;
    }
    case 4: {  // System (operator location)
      e->operatorLat = rd32(m + 2);
      e->operatorLon = rd32(m + 6);
      e->have |= RID_HAVE_SYSTEM;
      break;
    }
    case 5: {  // Operator ID
      copyId(e->operatorId, sizeof(e->operatorId), m + 2, 20);
      e->have |= RID_HAVE_OPID;
      break;
    }
    default: break;  // Auth (2), Self-ID (3), etc. — not decoded (not needed for the readout)
  }
  return type;
}

// Serial readout. Basic/System/OpID print on first decode (have-bit flip); Location throttled.
void report(RidEntry* e, int type, uint8_t prevHave) {
  char mac[18]; macStr(e->mac, mac);
  if (type == 1) {  // Location — throttled
    static uint32_t lastPrint = 0;  // coarse global throttle is fine (one drone usually)
    uint32_t now = millis();
    if (now - lastPrint < LOC_PRINT_GAP) return;
    lastPrint = now;
    Serial.printf("[RID] %s LOC lat=%.6f lon=%.6f h=%dm alt=%dm spd=%ucm/s hdg=%u\n",
                  mac, e->droneLat / 1e7, e->droneLon / 1e7, e->heightM, e->altGeoM,
                  e->speedCms, e->headingDeg);
  } else if ((e->have & ~prevHave) == 0) {
    return;  // nothing newly decoded (repeat of a type we already have) — stay quiet
  } else if (type == 0) {
    Serial.printf("[RID] %s BASIC idType=%u uaType=%u uasId=\"%s\"\n", mac, e->idType, e->uaType, e->uasId);
  } else if (type == 4) {
    Serial.printf("[RID] %s SYSTEM operator lat=%.6f lon=%.6f\n", mac, e->operatorLat / 1e7, e->operatorLon / 1e7);
  } else if (type == 5) {
    Serial.printf("[RID] %s OPID operatorId=\"%s\"\n", mac, e->operatorId);
  }
}

}  // namespace

void ingest(const uint8_t mac[6], const uint8_t* svcData, size_t len) {
  if (!svcData || len < ODID_MSG_SIZE) return;
  RidLock lock;  // table is shared with the loop task (snapshot); held for the whole ingest
  expireLocked(RID_TTL_MS);
  // Strip the legacy BLE framing: [0x0D app-code][counter][message...].
  if (svcData[0] == AD_APP_CODE && len >= (size_t)ODID_MSG_SIZE + 2) { svcData += 2; len -= 2; }
  if (len < ODID_MSG_SIZE) return;

  RidEntry* e = findOrAdd(mac);
  e->lastMs = millis();

  if ((svcData[0] >> 4) == 0x0F) {  // Message Pack: [hdr][singleSize][count][messages...]
    if (len < 3) return;
    uint8_t single = svcData[1], cnt = svcData[2];
    if (single != ODID_MSG_SIZE) return;
    for (uint8_t i = 0; i < cnt; i++) {
      size_t off = 3 + (size_t)i * ODID_MSG_SIZE;
      if (off + ODID_MSG_SIZE > len) break;
      uint8_t prev = e->have;
      int t = decodeMessage(e, svcData + off);
      report(e, t, prev);
    }
  } else {  // single message
    uint8_t prev = e->have;
    int t = decodeMessage(e, svcData);
    report(e, t, prev);
  }
}

void expire(uint32_t ttlMs) {
  RidLock lock;
  expireLocked(ttlMs);
}

void toLinkStruct(const RidEntry& e, link_protocol::RemoteId& out) {
  memcpy(out.mac, e.mac, 6);
  out.have = e.have;
  out.idType = e.idType;           out.uaType = e.uaType;
  out.droneLat = e.droneLat;       out.droneLon = e.droneLon;
  out.operatorLat = e.operatorLat; out.operatorLon = e.operatorLon;
  out.heightM = e.heightM;         out.altGeoM = e.altGeoM;
  out.speedCms = e.speedCms;       out.headingDeg = e.headingDeg;
  strncpy(out.uasId, e.uasId, sizeof(out.uasId));
  out.uasId[sizeof(out.uasId) - 1] = 0;
  strncpy(out.operatorId, e.operatorId, sizeof(out.operatorId));
  out.operatorId[sizeof(out.operatorId) - 1] = 0;
}

// Copy the current entries (as wire structs) under the lock; callers stream them outside it.
int snapshot(link_protocol::RemoteId* out, int maxOut) {
  RidLock lock;
  int n = (g_count < maxOut) ? g_count : maxOut;
  for (int i = 0; i < n; i++) toLinkStruct(g_tab[i], out[i]);
  return n;
}

}  // namespace remoteid

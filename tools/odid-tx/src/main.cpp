// ODID-TX — minimal OpenDroneID / ASTM F3411 BLE transmitter (bench test tool, NOT scanner firmware).
//
// Broadcasts BLE4 **legacy** advertisements carrying Service Data under 16-bit UUID 0xFFFA, framed as
// [0x0D app-code][counter][25-byte ODID message] — exactly what the cyd-scanner C5 decoder consumes
// (src/c5/remoteid.cpp ingest()). A legacy advert is capped at 31 bytes, which one such element fills
// exactly, so one advert carries ONE message; this rotates Basic ID -> Location -> System ->
// Operator ID under a single stable BLE address every TX_ROTATE_MS, so the scanner's per-MAC
// accumulator (remoteid RidEntry.have) fills in a full emitter picture over ~3 s.
//
// Flash to a spare ESP32-S3-DevKitC-1 (see tools/odid-tx/platformio.ini). The scanner then shows the
// drone on its C5 [RID] serial lines, and — once protocol v8 is flashed on both scanner boards —
// relays it to the CYD (its own [RID] line proves the RemoteId frame crossed the link).
//
// Scope: this emits a standards-compliant ODID broadcast purely for our own bench validation. It is
// receive-target test traffic, not interference with, or an attack on, any third-party device.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

// ---- Synthetic flight values (edit freely) ---------------------------------
static const char*   UAS_ID      = "TEST-DRONE-0001";  // Basic ID serial  (<= 20 chars)
static const char*   OPERATOR_ID = "OP-TEST-12345";    // Operator ID       (<= 20 chars)
static const int32_t DRONE_LAT_E7 =  407128000;        //  40.7128 N  (deg * 1e7)
static const int32_t DRONE_LON_E7 = -740060000;        // -74.0060 W
static const int32_t OPER_LAT_E7  =  407130000;        // operator, just offset from the drone
static const int32_t OPER_LON_E7  = -740062000;
static const float    HEIGHT_M    = 50.0f;             // height above takeoff (AGL)
static const float    ALT_GEO_M   = 120.0f;            // geodetic altitude
static const float    SPEED_MS    = 5.0f;              // horizontal speed
static const uint16_t HEADING_DEG = 45;                // track, degrees

static const uint32_t TX_ROTATE_MS = 750;              // advertise each message type this long

static const uint8_t ODID_VER = 2;  // protocol version carried in each message header's low nibble

// ---- Little-endian field writers -------------------------------------------
static void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t* p, int32_t v) {
  uint32_t u = (uint32_t)v;
  p[0] = u & 0xFF; p[1] = (u >> 8) & 0xFF; p[2] = (u >> 16) & 0xFF; p[3] = (u >> 24) & 0xFF;
}
static void putStr(uint8_t* p, const char* s, size_t n) {  // n-byte ASCII field, NUL padded
  memset(p, 0, n);
  size_t l = strlen(s); if (l > n) l = n;
  memcpy(p, s, l);
}
// Inverse of the decoder's `value = enc * 0.5 - 1000` (metres) altitude/height encoding.
static uint16_t encAlt(float m) {
  float e = (m + 1000.0f) / 0.5f;
  if (e < 0) e = 0; if (e > 65535.0f) e = 65535.0f;
  return (uint16_t)(e + 0.5f);
}

// ---- ODID message builders (25 bytes each) ---------------------------------
static void buildBasicId(uint8_t* m) {
  memset(m, 0, 25);
  m[0] = (0 << 4) | ODID_VER;          // type 0 = Basic ID
  m[1] = (1 << 4) | 1;                 // IDType=1 (serial CTA-2063-A), UAType=1
  putStr(m + 2, UAS_ID, 20);
}
static void buildLocation(uint8_t* m) {
  memset(m, 0, 25);
  m[0] = (1 << 4) | ODID_VER;          // type 1 = Location / Vector
  m[1] = 0x20;                         // status=airborne(2) in bits4-7; speedMult=0, ewDir=0
  m[2] = (uint8_t)(HEADING_DEG & 0xFF);            // Direction (ewDir=0 -> track = dir)
  m[3] = (uint8_t)(SPEED_MS / 0.25f + 0.5f);       // SpeedHorizontal enc (speedMult=0 -> enc*0.25)
  m[4] = 0;                            // SpeedVertical (int8)
  put32(m + 5, DRONE_LAT_E7);
  put32(m + 9, DRONE_LON_E7);
  put16(m + 13, encAlt(ALT_GEO_M));    // AltBaro (reuse geo alt for the test)
  put16(m + 15, encAlt(ALT_GEO_M));    // AltGeo
  put16(m + 17, encAlt(HEIGHT_M));     // Height (AGL)
  // 19 accuracy nibbles, 20 speed/baro acc, 21..22 ts, 23 ts-acc, 24 reserved -> left 0
}
static void buildSystem(uint8_t* m) {
  memset(m, 0, 25);
  m[0] = (4 << 4) | ODID_VER;          // type 4 = System (operator location)
  m[1] = 0x01;                         // OperatorLocationType=1 (live GNSS) in bits0-1
  put32(m + 2, OPER_LAT_E7);
  put32(m + 6, OPER_LON_E7);
  // area count/radius, operator alt, timestamp -> left 0
}
static void buildOperatorId(uint8_t* m) {
  memset(m, 0, 25);
  m[0] = (5 << 4) | ODID_VER;          // type 5 = Operator ID
  m[1] = 0;                            // OperatorIdType
  putStr(m + 2, OPERATOR_ID, 20);
}

// ---- BLE advertising -------------------------------------------------------
static NimBLEAdvertising* g_adv = nullptr;
static uint8_t g_counter = 0;

static void advertiseMessage(const uint8_t* msg25, const char* label) {
  // Build one Service Data - 16-bit UUID AD element on the wire:
  //   [len=30][0x16][0xFA][0xFF][0x0D][counter][25-byte message]   = 31 bytes total (legacy budget)
  uint8_t ad[4 + 27];
  ad[0] = 1 + 2 + 27;                  // AD length byte = type(1) + uuid(2) + 27 payload = 30
  ad[1] = 0x16;                        // GAP AD type: Service Data - 16-bit UUID
  ad[2] = 0xFA; ad[3] = 0xFF;          // UUID 0xFFFA, little-endian
  ad[4] = 0x0D;                        // ODID BLE "AD application code"
  ad[5] = g_counter++;                 // message counter (rolls)
  memcpy(ad + 6, msg25, 25);

  NimBLEAdvertisementData advData;
  advData.addData(ad, sizeof(ad));     // custom payload only (no Flags AD -> fits 31 bytes)

  g_adv->stop();
  g_adv->setAdvertisementData(advData);
  g_adv->start();
  Serial.printf("[TX] %-8s counter=%u (%u-byte adv)\n", label, (unsigned)(uint8_t)(g_counter - 1),
                (unsigned)sizeof(ad));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[ODID-TX] OpenDroneID BLE legacy transmitter");

  NimBLEDevice::init("ODID-TX");
  NimBLEDevice::setPower(9);                           // +9 dBm, easy bench catch
  g_adv = NimBLEDevice::getAdvertising();
  g_adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);    // non-connectable broadcast
  g_adv->enableScanResponse(false);

  Serial.printf("[ODID-TX] BLE address: %s  (watch the scanner's C5 [RID] lines for this MAC)\n",
                NimBLEDevice::getAddress().toString().c_str());
}

void loop() {
  uint8_t m[25];
  buildBasicId(m);    advertiseMessage(m, "BASIC");    delay(TX_ROTATE_MS);
  buildLocation(m);   advertiseMessage(m, "LOCATION"); delay(TX_ROTATE_MS);
  buildSystem(m);     advertiseMessage(m, "SYSTEM");   delay(TX_ROTATE_MS);
  buildOperatorId(m); advertiseMessage(m, "OPID");     delay(TX_ROTATE_MS);
}

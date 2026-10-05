// c5test — 802.15.4 TEST SOURCE (bench test transmitter). NOT the scanner.
//
// Purpose: validate the scanner's 802.15.4 RX path (src/c5/ieee154.cpp, PH_154) with
// controlled, known traffic. It cycles three crafted frames that exercise every parse
// path of the scanner: short-addr data, extended (EUI-64) data, and a beacon.
//
// IMPORTANT:
//  - This is the only firmware here that transmits CRAFTED test frames (the shipped scanner
//    only does standard active scans + its own phone/AP links, never crafted/attack traffic).
//    It is dev/bench test equipment, not part of the shipped counter-surveillance device.
//  - It must not be confused with the scanner (env:c5). Never flash it to the scanner's C5.
//  - Polite by design: ~1 frame/s, CCA (listen-before-talk) on, ack-request = 0, and a
//    made-up PAN/address set (below). No Wi-Fi, no BLE.
//
// Usage: flash a SECOND ESP32-C5 DevKit (native USB port, 303A:1001):
//   pio run -e c5test -t upload      then watch the USB console; correlate each "TX" line
//   with what the scanner's C5 reports (set TEST_CHANNEL within the scanner's hop set).
#include <Arduino.h>
#include <esp_ieee802154.h>
#include <string.h>

// ---- Configuration -------------------------------------------------------------
#define TEST_CHANNEL     15          // 802.15.4 channel 11-26 (scanner hops 11,15,20,25,26)
#define TEST_PAN_ID      0x1234      // made-up PAN ID
#define TEST_SHORT_ADDR  0xBEEF      // made-up short source address
// Made-up EUI-64, MSB first (OUI 00:12:4B = a real Zigbee/TI OUI -> scanner OUI layer).
#define TEST_EUI64       { 0x00, 0x12, 0x4B, 0x00, 0xDE, 0xAD, 0xBE, 0xEF }
#define TEST_TX_INTERVAL_MS 1000     // one frame per interval, cycling through the set
#define TEST_TX_POWER_DBM   0        // low power: bench use only
// --------------------------------------------------------------------------------

namespace {

constexpr uint16_t BCAST = 0xFFFF;
constexpr uint8_t  EUI64[8] = TEST_EUI64;

// TX buffer = [len][MHR][payload]; len counts MHR+payload+2 (FCS, appended by hardware).
// Must stay valid until transmit_done/failed, hence static.
uint8_t g_frame[127];
uint8_t g_seq = 0;

volatile bool    g_txBusy = false;
volatile bool    g_txDone = false;
volatile uint8_t g_txErr  = 0;  // esp_ieee802154_tx_error_t (0 = none)

inline void put16(uint8_t*& p, uint16_t v) { *p++ = v & 0xFF; *p++ = v >> 8; }  // LE

// Legacy (2003/2006, version 0) FCF: type | dst mode<<10 | src mode<<14. AR=0, comp=0.
// With ver 0 + comp 0 the scanner's parser expects both dest PAN and source PAN present.
inline uint16_t fcf(uint8_t type, uint8_t dm, uint8_t sm) {
  return (uint16_t)type | ((uint16_t)dm << 10) | ((uint16_t)sm << 14);
}

// Finish the buffer: g_frame[0] = body length + 2 (FCS). Returns total body bytes.
inline uint8_t seal(uint8_t* end) {
  uint8_t body = (uint8_t)(end - (g_frame + 1));
  g_frame[0] = body + 2;
  return body;
}

// Frame 0: data, broadcast dest, SHORT src.
// MHR: FCF(01 88) seq dstPAN(FFFF) dstAddr(FFFF) srcPAN(1234) srcShort(BEEF) | payload "T154"
uint8_t buildDataShort() {
  uint8_t* p = g_frame + 1;
  put16(p, fcf(1, 2, 2));
  *p++ = g_seq;
  put16(p, BCAST); put16(p, BCAST);
  put16(p, TEST_PAN_ID); put16(p, TEST_SHORT_ADDR);
  memcpy(p, "T154", 4); p += 4;
  return seal(p);
}

// Frame 1: data, broadcast dest, EXTENDED src.
// MHR: FCF(01 C8) seq dstPAN(FFFF) dstAddr(FFFF) srcPAN(1234) srcEUI64 (LE on air) | "T154"
uint8_t buildDataExt() {
  uint8_t* p = g_frame + 1;
  put16(p, fcf(1, 2, 3));
  *p++ = g_seq;
  put16(p, BCAST); put16(p, BCAST);
  put16(p, TEST_PAN_ID);
  for (int i = 7; i >= 0; i--) *p++ = EUI64[i];  // EUI-64 is sent LSB first
  memcpy(p, "T154", 4); p += 4;
  return seal(p);
}

// Frame 2: beacon (type 0), no dest, SHORT src.
// MHR: FCF(00 80) seq srcPAN(1234) srcShort(BEEF) | superframe spec(FF CF) GTS(00) pend(00)
uint8_t buildBeacon() {
  uint8_t* p = g_frame + 1;
  put16(p, fcf(0, 0, 2));
  *p++ = g_seq;
  put16(p, TEST_PAN_ID); put16(p, TEST_SHORT_ADDR);
  *p++ = 0xFF; *p++ = 0xCF;  // beacon order/superframe order 15, PAN coordinator, assoc permit
  *p++ = 0x00;               // GTS spec (none)
  *p++ = 0x00;               // pending address spec (none)
  return seal(p);
}

}  // namespace

// TX callbacks (driver context): record the result only; main loop prints it.
extern "C" void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t* frame, const uint8_t* ack,
                                                       esp_ieee802154_frame_info_t* info) {
  if (ack) esp_ieee802154_receive_handle_done(ack);  // AR=0, so normally NULL
  g_txErr = 0;
  g_txDone = true;
}
extern "C" void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t* frame,
                                                         esp_ieee802154_tx_error_t error) {
  g_txErr = (uint8_t)error;
  g_txDone = true;
}
// Receive is unused here, but a frame heard while idle must still be released.
extern "C" void IRAM_ATTR esp_ieee802154_receive_done(uint8_t* frame,
                                                      esp_ieee802154_frame_info_t* info) {
  esp_ieee802154_receive_handle_done(frame);
}

static const char* kNames[] = {"DATA/short", "DATA/ext", "BEACON/short"};

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("==============================================");
  Serial.println(" 802.15.4 TEST SOURCE  (bench transmitter)");
  Serial.println(" NOT the scanner. Dev/test tool only.");
  Serial.println("==============================================");
  Serial.printf(" channel : %d\n", TEST_CHANNEL);
  Serial.printf(" PAN     : 0x%04X\n", TEST_PAN_ID);
  Serial.printf(" short   : 0x%04X\n", TEST_SHORT_ADDR);
  Serial.printf(" EUI-64  : %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n", EUI64[0], EUI64[1],
                EUI64[2], EUI64[3], EUI64[4], EUI64[5], EUI64[6], EUI64[7]);
  Serial.printf(" interval: %d ms, CCA on, AR=0\n", TEST_TX_INTERVAL_MS);

  esp_err_t e = esp_ieee802154_enable();
  if (e != ESP_OK) Serial.printf("enable failed: %d\n", (int)e);
  uint8_t ext[8];
  for (int i = 0; i < 8; i++) ext[i] = EUI64[7 - i];  // driver wants LSB first
  esp_ieee802154_set_extended_address(ext);
  esp_ieee802154_set_panid(TEST_PAN_ID);
  esp_ieee802154_set_short_address(TEST_SHORT_ADDR);
  esp_ieee802154_set_channel(TEST_CHANNEL);
  esp_ieee802154_set_txpower(TEST_TX_POWER_DBM);
  esp_ieee802154_set_cca_mode(ESP_IEEE802154_CCA_MODE_CARRIER_OR_ED);
  esp_ieee802154_set_rx_when_idle(false);
}

void loop() {
  static uint32_t last = 0;
  static uint8_t  idx  = 0;

  if (g_txBusy && g_txDone) {
    g_txBusy = false;
    Serial.printf("  -> %s\n", g_txErr ? "FAILED (tx_error above 0 = CCA busy/abort/etc)" : "tx done");
    if (g_txErr) Serial.printf("     tx_error=%u (1=CCA_BUSY 2=ABORT 3=NO_ACK 6=COEXIST)\n", g_txErr);
  }

  if (!g_txBusy && millis() - last >= TEST_TX_INTERVAL_MS) {
    last = millis();
    uint8_t body = idx == 0 ? buildDataShort() : idx == 1 ? buildDataExt() : buildBeacon();
    g_txDone = false;
    g_txBusy = true;
    esp_err_t r = esp_ieee802154_transmit(g_frame, true);  // cca = listen-before-talk
    if (idx == 1)
      Serial.printf("TX seq=%u %s ch=%d pan=0x%04X src=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X "
                    "bytes=%u ret=%d\n", g_seq, kNames[idx], TEST_CHANNEL, TEST_PAN_ID, EUI64[0],
                    EUI64[1], EUI64[2], EUI64[3], EUI64[4], EUI64[5], EUI64[6], EUI64[7], body,
                    (int)r);
    else
      Serial.printf("TX seq=%u %s ch=%d pan=0x%04X src=0x%04X bytes=%u ret=%d\n", g_seq,
                    kNames[idx], TEST_CHANNEL, TEST_PAN_ID, TEST_SHORT_ADDR, body, (int)r);
    if (r != ESP_OK) g_txBusy = false;
    g_seq++;
    idx = (idx + 1) % 3;
  }
  delay(5);
}

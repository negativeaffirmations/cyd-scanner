// phone.cpp — see phone.h.
#include "phone.h"
#include <NimBLEDevice.h>

namespace {

// Custom 128-bit UUIDs (base 9a1e000X-2b7e-4c1a-9b00-1a2b3c4d5e6f).
const char* SVC_UUID    = "9a1e0000-2b7e-4c1a-9b00-1a2b3c4d5e6f";
const char* TIME_UUID   = "9a1e0001-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // write: epoch secs
const char* GPS_UUID    = "9a1e0002-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // write: "lat,lon"
const char* CMD_UUID    = "9a1e0003-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // write: "1"/"0"/"R"/"L"
const char* STATUS_UUID = "9a1e0004-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // read/notify
const char* LOGDATA_UUID= "9a1e0005-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // notify: log chunks
const char* DETS_UUID   = "9a1e0006-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // notify: live detections

NimBLECharacteristic* g_status  = nullptr;
NimBLECharacteristic* g_logData = nullptr;
NimBLECharacteristic* g_dets    = nullptr;
bool     g_connected  = false;
uint32_t g_epochBase  = 0;   // epoch secs at g_baseMillis
uint32_t g_baseMillis = 0;
bool     g_haveTime   = false;
float    g_lat = 0, g_lon = 0;
bool     g_haveGps    = false;
bool     g_download   = false;
bool     g_reload     = false;
bool     g_logReq     = false;
uint8_t  g_ownMac[6]  = {0};
bool     g_haveOwnMac = false;
uint8_t  g_peerMac[6] = {0};
bool     g_havePeer   = false;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& ci) override {
    g_connected = true;
    std::string mac = ci.getAddress().toString();  // connected phone's BLE address
    unsigned b[6];
    if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
      for (int i = 0; i < 6; i++) g_peerMac[i] = (uint8_t)b[i];
      g_havePeer = true;
    }
    Serial.printf("[phone] connected (peer %s)\n", mac.c_str());
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo&, int) override {
    g_connected = false;
    g_download  = false;  // drop download mode if the phone leaves
    g_havePeer  = false;  // stop filtering the (now gone) phone
    Serial.println("[phone] disconnected");
    NimBLEDevice::getAdvertising()->start();  // keep discoverable
  }
};

class WriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    std::string uuid = c->getUUID().toString();
    std::string val  = c->getValue();
    if (uuid == TIME_UUID) {
      // "utcEpoch;tzOffsetMinutes" (offset = UTC-local, as JS getTimezoneOffset()).
      // Store the LOCAL epoch so the display reads local time. Bare "epoch" also works.
      long utc = 0, offMin = 0;
      int n = sscanf(val.c_str(), "%ld;%ld", &utc, &offMin);
      if (n >= 1 && utc > 1600000000L) {  // sanity: after 2020
        g_epochBase = (uint32_t)(utc - offMin * 60);
        g_baseMillis = millis();
        g_haveTime = true;
        Serial.printf("[phone] time sync: utc=%ld offMin=%ld -> local base %lu\n",
                      utc, offMin, (unsigned long)g_epochBase);
      }
    } else if (uuid == GPS_UUID) {
      float la, lo;
      if (sscanf(val.c_str(), "%f,%f", &la, &lo) == 2) {
        g_lat = la; g_lon = lo; g_haveGps = true;
        Serial.printf("[phone] gps: %.5f,%.5f\n", la, lo);
      }
    } else if (uuid == CMD_UUID) {
      char c0 = val.empty() ? 0 : val[0];
      if (c0 == 'R' || c0 == 'r') {
        g_reload = true;
        Serial.println("[phone] cmd reload signature DB");
      } else if (c0 == 'L' || c0 == 'l') {
        g_logReq = true;
        Serial.println("[phone] cmd BLE log download");
      } else {
        g_download = (c0 == '1');
        Serial.printf("[phone] cmd download=%d\n", g_download);
      }
    }
  }
};

ServerCB g_serverCB;
WriteCB  g_writeCB;

}  // namespace

namespace phone {

void begin(const char* devName) {
  NimBLEDevice::init(devName);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(&g_serverCB);

  NimBLEService* svc = server->createService(SVC_UUID);
  svc->createCharacteristic(TIME_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(&g_writeCB);
  svc->createCharacteristic(GPS_UUID,  NIMBLE_PROPERTY::WRITE)->setCallbacks(&g_writeCB);
  svc->createCharacteristic(CMD_UUID,  NIMBLE_PROPERTY::WRITE)->setCallbacks(&g_writeCB);
  g_status = svc->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_logData = svc->createCharacteristic(LOGDATA_UUID, NIMBLE_PROPERTY::NOTIFY);
  g_dets    = svc->createCharacteristic(DETS_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);       // service UUID in the adv packet (filterable)
  adv->setName(devName);
  // The 128-bit service UUID fills the adv packet, so put the name in an explicit
  // scan response so Chrome shows "CYD-Scanner" instead of "Unknown device".
  NimBLEAdvertisementData scanResp;
  scanResp.setName(devName);
  adv->setScanResponseData(scanResp);
  adv->enableScanResponse(true);
  adv->start();

  // Cache our own BLE MAC (same MSB-first text form the C5 parses) so the scanner
  // can drop its own advertisement. Runtime value -> works on any CYD unit.
  std::string mac = NimBLEDevice::getAddress().toString();
  unsigned b[6];
  if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
    for (int i = 0; i < 6; i++) g_ownMac[i] = (uint8_t)b[i];
    g_haveOwnMac = true;
  }
  Serial.printf("[phone] BLE advertising as '%s' (own MAC %s)\n", devName, mac.c_str());
}

bool ownMac(uint8_t out[6]) {
  if (!g_haveOwnMac) return false;
  memcpy(out, g_ownMac, 6);
  return true;
}

bool peerMac(uint8_t out[6]) {
  if (!g_havePeer) return false;
  memcpy(out, g_peerMac, 6);
  return true;
}

void setStatus(const String& s) {
  if (!g_status) return;
  g_status->setValue((uint8_t*)s.c_str(), s.length());
  if (g_connected) g_status->notify();
}

bool     connected()        { return g_connected; }
bool     hasTime()          { return g_haveTime; }
uint32_t epochNow()         { return g_haveTime ? g_epochBase + (millis() - g_baseMillis) / 1000 : 0; }
bool     hasGps()           { return g_haveGps; }
float    lat()              { return g_lat; }
float    lon()              { return g_lon; }
bool     downloadRequested(){ return g_download; }
bool     reloadRequested()  { bool r = g_reload; g_reload = false; return r; }
bool     logRequested()     { bool r = g_logReq; g_logReq = false; return r; }

void logNotify(const uint8_t* data, size_t len) {
  if (!g_logData || !g_connected) return;
  g_logData->setValue(data, len);
  g_logData->notify();
}

void detsNotify(const String& line) {
  if (!g_dets || !g_connected) return;
  g_dets->setValue((uint8_t*)line.c_str(), line.length());
  g_dets->notify();
}

}  // namespace phone

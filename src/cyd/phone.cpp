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

NimBLECharacteristic* g_status  = nullptr;
NimBLECharacteristic* g_logData = nullptr;
bool     g_connected  = false;
uint32_t g_epochBase  = 0;   // epoch secs at g_baseMillis
uint32_t g_baseMillis = 0;
bool     g_haveTime   = false;
float    g_lat = 0, g_lon = 0;
bool     g_haveGps    = false;
bool     g_download   = false;
bool     g_reload     = false;
bool     g_logReq     = false;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    g_connected = true;
    Serial.println("[phone] connected");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo&, int) override {
    g_connected = false;
    g_download  = false;  // drop download mode if the phone leaves
    Serial.println("[phone] disconnected");
    NimBLEDevice::getAdvertising()->start();  // keep discoverable
  }
};

class WriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    std::string uuid = c->getUUID().toString();
    std::string val  = c->getValue();
    if (uuid == TIME_UUID) {
      uint32_t e = (uint32_t)strtoul(val.c_str(), nullptr, 10);
      if (e > 1600000000UL) {  // sanity: after 2020
        g_epochBase = e;
        g_baseMillis = millis();
        g_haveTime = true;
        Serial.printf("[phone] time sync: %lu\n", (unsigned long)e);
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
  Serial.printf("[phone] BLE advertising as '%s'\n", devName);
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

}  // namespace phone

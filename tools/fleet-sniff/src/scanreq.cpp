// scanreq: shows the address a BLE scanner really sends. A SCAN_REQ can't be seen from Wi-Fi, so this C5 advertises a
// scannable beacon and prints the address of every scanner that asks it for a scan response. It transmits only that
// advert and the scan responses, which is what any BLE beacon does; it attacks nothing.
// Output: S <ms> <address> <0 public | 1 random>
#include <Arduino.h>
#include <NimBLEDevice.h>

class Cb : public NimBLEExtAdvertisingCallbacks {
  void onScanRequest(NimBLEExtAdvertising*, uint8_t, NimBLEAddress a) override {
    Serial.printf("S %lu %s %u\n", (unsigned long)millis(), a.toString().c_str(), (unsigned)a.getType());
  }
};
static Cb g_cb;

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  NimBLEDevice::init("");
  NimBLEExtAdvertisement adv(BLE_HCI_LE_PHY_1M, BLE_HCI_LE_PHY_1M);
  adv.setLegacyAdvertising(true);
  adv.setConnectable(false);
  adv.setScannable(true);
  adv.enableScanRequestCallback(true);
  adv.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  NimBLEExtAdvertisement rsp(BLE_HCI_LE_PHY_1M, BLE_HCI_LE_PHY_1M);
  rsp.setLegacyAdvertising(true);
  rsp.setConnectable(false);
  rsp.setScannable(true);
  rsp.setName("scanreq");
  NimBLEExtAdvertising* e = NimBLEDevice::getAdvertising();
  e->setCallbacks(&g_cb, false);
  const bool ok = e->setInstanceData(0, adv) && e->setScanResponseData(0, rsp) && e->start(0);
  Serial.printf("# scanreq %s\n", ok ? "advertising" : "FAILED");
}

void loop() { delay(100); }

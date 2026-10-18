// host_ui.cpp — headless hostui:: hooks: serial summary only, no screen.
#include <Arduino.h>
#include "../host_common/host_ui.h"

void hostui::onScanCycle(const ScanCycleResult& r) {
  Serial.printf("[S3] cycle: 2.4:%d 5G:%d BLE:%d PRB:%d 154:%d new=%d threats S%d/L%d/C%d\n",
                r.n24, r.n5, r.nble, r.nprb, r.n154, r.newCount, r.susp, r.lk, r.conf);
}
void hostui::onBusyMiss() {}
void hostui::onDownloadState(bool, bool) {}  // the join QR is rendered by the phone web app (§7)
void hostui::onStatusPushed() {}
bool hostui::exploreHolds(const char*) { return false; }  // no on-device viewer

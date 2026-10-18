// host_ui.h — optional UI hooks the shared host core calls (docs/mini-build.md §4.3).
// The CYD implements them with TFT draws; the headless XIAO-S3 implements them as no-ops/serial.
#pragma once
#include "host_core.h"

namespace hostui {
void onScanCycle(const ScanCycleResult& r);         // a full (non-busy-miss) cycle finished
void onBusyMiss();                                  // the C5 missed a poll (busy window)
void onDownloadState(bool apActive, bool joined);   // Wi-Fi download AP state changed
void onStatusPushed();                              // pushStatus() just sent a STATUS line
bool exploreHolds(const char* path);                // true if a board-local viewer has this /logs/ file open
}

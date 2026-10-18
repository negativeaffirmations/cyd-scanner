// host_phone_cmds.h — the loop()-top phone-command dispatch, split from the CYD main.cpp into the
// board-agnostic part (docs/mini-build.md §4.5). The board reacts to the returned flags.
#pragma once
#include <stdint.h>

struct PhoneCmdFlags {
  bool brightnessChanged = false; int brightnessPct = 0;   // "B:<pct>" (raw; the board clamps/applies/saves)
  bool lockChanged = false;       bool lockState = false;  // "K1"/"K0" (g_touchLocked already updated)
  bool srcMaskChanged = false;                             // "S:" (g_srcMask already updated + saved)
  bool sessionChanged = false;                             // the fg session file path changed (new session / time-sync rename)
  bool newSession = false;                                 // "N"
  bool scanStarted = false, scanStopped = false;           // "G"/"X" (g_scanActive already updated)
  bool downloadEnter = false;                              // Wi-Fi download requested (level: true every loop while active)
  bool downloadExit = false;                               // download just ended (AP torn down, g_scanActive restored)
  bool acceptScanStart = true;                             // INPUT to servicePhoneScan(): false defers a "G" (left pending)
};

namespace core {
// Everything board-agnostic that runs ahead of the Wi-Fi-download check: DB reload, BLE log/list/file/export/
// delete transfers, whitelist reload/list/add/remove, brightness/lock/srcmask/new-session requests,
// time-sync rename + share-path sync, and download enter/exit detection.
void servicePhoneCommands(PhoneCmdFlags& f);
// Phone "G"/"X" scan start/stop. Called AFTER the download early-return (same ordering the CYD loop always had).
void servicePhoneScan(PhoneCmdFlags& f);
// Low-frequency (~10 min) /logs/ cap check. `sdBusy` = the board is reading the SD card for its own UI.
void tickLogCap(bool sdBusy);
}

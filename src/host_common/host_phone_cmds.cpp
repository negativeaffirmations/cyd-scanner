// host_phone_cmds.cpp — see host_phone_cmds.h. Code moved verbatim from the CYD loop() top.
#include <Arduino.h>
#include "host_phone_cmds.h"
#include "host_core.h"
#include "host_log.h"
#include "host_status.h"
#include "host_board.h"
#include "phone.h"
#include "sigdb.h"
#include "webshare.h"
#include "whitelist.h"

void core::servicePhoneCommands(PhoneCmdFlags& f) {
  disciplineRtc();  // phone/GPS time -> RTC write-back (no-op without an RTC)
  // Phone commands are honored from any screen.
  if (phone::reloadRequested()) sigdb::reload();
  if (phone::logRequested())    transferLog();
  if (phone::listRequested())   sendSessionList();
  if (phone::wlReloadRequested()) { whitelist::reload(); pushStatus(); }
  if (phone::wlListRequested())   sendWhitelist();
  { char ln[96]; if (phone::wlAddRequested(ln, sizeof(ln))) {  // "A:<rule>": SD write in the loop, not mid-scan
      bool ok = true;
      for (const char* c = ln; *c; c++) if ((unsigned char)*c < 0x20) { ok = false; break; }  // one line only
      if (ok) ok = whitelist::add(ln);
      Serial.printf("[CYD] whitelist add %s: %s\n", ok ? "ok" : "REJECTED", ln);
      sendWhitelist(); pushStatus();
    } }
  { int ix; if (phone::wlRemoveRequested(&ix)) {
      bool ok = whitelist::removeAt(ix);
      Serial.printf("[CYD] whitelist remove %d: %s\n", ix, ok ? "ok" : "FAILED");
      sendWhitelist(); pushStatus();
    } }
  { char fn[48]; if (phone::fileRequested(fn, sizeof(fn)))   transferFile(fn); }
  { int m, a; char fn[48]; if (phone::exportRequested(&m, &a, fn, sizeof(fn))) transferFiltered(m, a, fn); }
  { char fn[48]; if (phone::deleteRequested(fn, sizeof(fn))) deleteSession(fn); }
  { int b; if (phone::brightnessRequested(&b)) {           // web-app brightness slider
      f.brightnessChanged = true; f.brightnessPct = b;     // the board clamps, applies and saves it
    } }
  { bool lk; if (phone::lockRequested(&lk)) {              // web-app lock / unlock the display
      g_touchLocked = lk;
      f.lockChanged = true; f.lockState = lk;              // the board repaints + pushes status
    } }
  { uint8_t m; if (phone::srcMaskRequested(&m) && m != 0) {  // web-app source toggles (ignore all-off)
      g_srcMask = m; saveSrcMask();
      f.srcMaskChanged = true;
    } }
  { uint8_t t, g; if (phone::sourcePrefRequested(&t, &g)) {  // web-app time / GPS source preference
      if (t != g_timePref || g != g_gpsPref) { g_timePref = t; g_gpsPref = g; saveSrcPrefs(); }  // skip a no-op NVS write
      pushStatus();  // the Settings selects + source icons reflect it right away
    } }
  if (phone::newSessionRequested()) { startNewSession(); f.newSession = true; }  // web-app "New Session"
  renameSessionOnSync();  // give each live session a date-time filename once time is known (also syncs the share path)
  syncSharePath();        // the screen context (fg/bg) may have changed
  {  // detect a changed session path (phone "N", or the time-sync rename) so the board can refresh its label
    static char lastSess[48] = "";
    if (strcmp(lastSess, g_fg.path) != 0) {
      strncpy(lastSess, g_fg.path, sizeof(lastSess) - 1);
      f.sessionChanged = true;
    }
  }

  // Phone-initiated Wi-Fi download overrides the current screen while active.
  if (phone::downloadRequested()) { f.downloadEnter = true; return; }
  if (webshare::active()) {  // just left download mode: tear down AP
    webshare::stop();
    g_scanActive = g_bgScan;  // the download interrupted any running scan (bg scan resumes)
    f.downloadExit = true;
  }
}

void core::servicePhoneScan(PhoneCmdFlags& f) {
  // Phone can start/stop the scan remotely (single toggle in the web app). The board may defer a start
  // (acceptScanStart=false) so a stray phone toggle can't disturb a screen holding indices into the live
  // tables; scanStartRequested() is consume-on-read, so short-circuiting leaves it pending.
  if (f.acceptScanStart && phone::scanStartRequested()) {
    g_scanActive = true;
    g_lastCycleMs = 0;  // run the first cycle immediately
    f.scanStarted = true;
  }
  // Consume the stop flag unconditionally. Stop ends the scan but the board stays where it is
  // (it just goes idle), so the user can review the list or restart.
  if (phone::scanStopRequested()) {
    g_scanActive = false;
    showLinkLed();
    f.scanStopped = true;
  }
}

void core::tickLogCap(bool sdBusy) {
  // Low-frequency /logs/ cap check (~10 min). Callers skip it while a Wi-Fi/phone download is active
  // and while the board is reading the SD card for its own UI (avoid contending for the card).
  static uint32_t lastCap = 0;
  if (g_sdOk && millis() - lastCap >= 600000UL && !sdBusy) {
    lastCap = millis();
    enforceLogCap();
  }
}

// main.cpp — XIAO ESP32-S3 Sense headless host firmware (env:xiao_s3). docs/mini-build.md §4.7 Phase 2.
//
// No screen, touch or BOOT UI: the phone web app drives everything over the BLE GATT link (phone.*).
// Polls the XIAO-C5 over the UART link, scores, logs NDJSON to the Sense microSD, mirrors to the phone.
// All the logic is the shared host_common core; this file is just setup(), a small loop() and the
// headless Wi-Fi download tick.
#include <Arduino.h>
#include <Preferences.h>
#include "pins.h"
#include "link_protocol.h"
#include "../host_common/phone.h"
#include "../host_common/webshare.h"
#include "../host_common/sigdb.h"
#include "../host_common/whitelist.h"
#include "../host_common/host_link.h"
#include "../host_common/host_core.h"
#include "../host_common/host_log.h"
#include "../host_common/host_status.h"
#include "../host_common/host_phone_cmds.h"
#include "../host_common/host_board.h"

static const char* DEVICE_NAME = "COINTEL-FLO";

uint8_t g_brightness = 100;   // phone slider value, persisted + echoed in STATUS (no backlight to drive)
bool    g_fgScan     = false; // phone-started scan -> logs to the fg stream; boot/bg scan logs to the bg stream

static void loadBrightness() {
  Preferences p; p.begin("cydui", true);
  g_brightness = p.getUChar("bright", 100);
  p.end();
  if (g_brightness < 10 || g_brightness > 100) g_brightness = 100;
}
static void saveBrightness() {
  Preferences p; p.begin("cydui", false);
  p.putUChar("bright", g_brightness);
  p.end();
}

// Headless Wi-Fi bulk-download tick: SoftAP + web server; SSID/PSK reach the phone via STATUS (Phase 4).
static void runDownload() {
  if (!webshare::active()) webshare::start();
  webshare::handle();
  static uint32_t lastS = 0;
  if (millis() - lastS > 1000) { lastS = millis(); pushStatus(); }
  delay(2);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[S3] boot: headless scanner host + phone link + SD");
  hostboard::begin();
  loadBrightness();
  loadSrcMask();
  loadBgScan();
  g_scanActive = g_bgScan;  // background scan resumes at boot (first cycle runs right away)
  initSD();
  sigdb::begin();       // load /signatures.csv (seeds it if absent) or fall back
  whitelist::begin();   // load /whitelist.csv
  phone::begin(DEVICE_NAME);
  g_haveOwnMac = phone::ownMac(g_ownMac);  // for self-detection filtering
  g_linkOk = pingC5();
  if (g_linkOk) g_lastLinkOkMs = millis();
  Serial.printf("[S3] ready (link=%s, sd=%s)\n", g_linkOk ? "up" : "down", g_sdOk ? "ok" : "none");
}

void loop() {
  PhoneCmdFlags pf;
  core::servicePhoneCommands(pf);
  if (pf.brightnessChanged) {
    g_brightness = (uint8_t)constrain(pf.brightnessPct, 10, 100);
    saveBrightness();
    pushStatus();
  }
  if (pf.lockChanged || pf.srcMaskChanged || pf.newSession) pushStatus();  // lock/srcmask already applied in core

  if (pf.downloadEnter) { runDownload(); return; }
  if (pf.downloadExit) { g_fgScan = false; pushStatus(); }  // core restored g_scanActive = g_bgScan

  core::tickLogCap(false);

  pf.acceptScanStart = true;
  core::servicePhoneScan(pf);
  if (pf.scanStarted) { g_fgScan = true; resetFollow(); pushStatus(); }
  if (pf.scanStopped) { g_fgScan = false; pushStatus(); }

  // One logical scan (bg at boot, or phone G/X). Back the cadence off while the C5 link is down.
  if (g_scanActive && !webshare::active() &&
      millis() - g_lastCycleMs >= (g_linkOk ? SCAN_CYCLE_MS : SCAN_CYCLE_DOWN_MS)) {
    g_lastCycleMs = millis();
    core::runScanCycle();
  } else if (!g_scanActive) {  // idle: keep the link dot + phone STATUS fresh (scan=0)
    static uint32_t lastIdle = 0;
    if (millis() - lastIdle > 2000) {
      lastIdle = millis();
      if (pingC5()) g_lastLinkOkMs = millis();
      g_linkOk = (millis() - g_lastLinkOkMs < LINK_STICKY_MS);
      pushStatus();
    }
  }
  delay(10);
}

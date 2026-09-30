// main.cpp — CYD host firmware (env:cyd)
//
// Two build modes (LINK_MONITOR):
//   0 = NORMAL. Polls the ESP32-C5 for its live detection table (dual-band Wi-Fi +
//       BLE), shows it, and logs newly-seen devices to the SD card. Also runs a BLE
//       GATT peripheral (phone.*) so a phone web app can sync time, push GPS, and
//       request Wi-Fi log download. In download mode the CYD raises a SoftAP
//       (webshare.*) and shows a QR code the phone scans to join and grab the CSV.
//   1 = LINK MONITOR — heartbeat/loss tester for hunting flaky link wiring.
//
// SD=HSPI, display=VSPI, C5 link=UART1. BLE peripheral + Wi-Fi AP coexist.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <algorithm>
#include <TFT_eSPI.h>
#include <qrcode.h>
#include "pins.h"
#include "link_protocol.h"
#include "phone.h"
#include "webshare.h"
#include "sigdb.h"
#include "touch.h"

using namespace link_protocol;

// >>> Set to 1 to run the link connection monitor, 0 for the normal scanner. <<<
#define LINK_MONITOR 0

static constexpr uint8_t UI_ROTATION = 0;  // portrait 240x320 (90 CCW from landscape)
static const char*       DEVICE_NAME = "CYD-Scanner";

TFT_eSPI       tft = TFT_eSPI();
HardwareSerial LinkSerial(1);  // UART1 on GPIO22/GPIO27
FrameParser    parser;

static void sendFrame(uint8_t type, const void* payload, uint16_t len) {
  uint8_t out[MAX_FRAME];
  uint16_t n = encodeFrame(type, (const uint8_t*)payload, len, out);
  LinkSerial.write(out, n);
  LinkSerial.flush();
}

static void setLed(bool r, bool g, bool b) {  // active low
  digitalWrite(LED_R_PIN, r ? LOW : HIGH);
  digitalWrite(LED_G_PIN, g ? LOW : HIGH);
  digitalWrite(LED_B_PIN, b ? LOW : HIGH);
}

static void initCommon() {
  pinMode(0, INPUT_PULLUP);  // BOOT button (runtime use: pop up the app QR)
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  setLed(false, false, false);
  ledcAttach(TFT_BL_PIN, 5000, 8);   // backlight on PWM (8-bit) for brightness control
  ledcWrite(TFT_BL_PIN, 255);        // full brightness until a saved level is applied
  tft.init();
  tft.setRotation(UI_ROTATION);
  LinkSerial.setRxBufferSize(2048);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
}

static const char* srcTag(const Detection& d) {
  if (d.source == (uint8_t)Source::BleScan)   return "BLE";
  if (d.source == (uint8_t)Source::WifiProbe) return "PRB";  // Wi-Fi client probe
  return d.channel > 14 ? "5G" : "2.4";
}

#if LINK_MONITOR
// ---------------------------------------------------------------- monitor mode
uint32_t g_rx = 0, g_rawBytes = 0, g_firstSeq = 0, g_lastSeq = 0;
bool     g_haveFirst = false;
uint32_t g_lastHbMs = 0;

static void render() {
  bool up = g_haveFirst && (millis() - g_lastHbMs < 1500);
  uint32_t expected = g_haveFirst ? (g_lastSeq - g_firstSeq + 1) : 0;
  uint32_t missed   = expected > g_rx ? expected - g_rx : 0;
  float loss = expected ? (100.0f * missed / expected) : 0.0f;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("link monitor", 6, 4, 4);
  tft.setTextColor(up ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.drawString(up ? "Link: UP" : "Link: DOWN", 6, 40, 4);
  char buf[48];
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "recv:%lu  exp:%lu  loss:%.1f%%",
           (unsigned long)g_rx, (unsigned long)expected, loss);
  tft.drawString(buf, 6, 90, 2);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[CYD] boot: LINK MONITOR");
  initCommon();
  render();
}

void loop() {
  while (LinkSerial.available()) {
    uint8_t b = LinkSerial.read();
    g_rawBytes++;
    if (!parser.feed(b)) continue;
    if (parser.type() == (uint8_t)Reply::Heartbeat &&
        parser.length() >= sizeof(Heartbeat)) {
      const Heartbeat* hb = reinterpret_cast<const Heartbeat*>(parser.payload());
      if (!g_haveFirst) { g_firstSeq = hb->seq; g_haveFirst = true; }
      g_lastSeq = hb->seq; g_rx++; g_lastHbMs = millis();
    }
  }
  bool up = g_haveFirst && (millis() - g_lastHbMs < 1500);
  setLed(!up, up, false);
  static uint32_t lastRender = 0;
  if (millis() - lastRender > 250) { lastRender = millis(); render(); }
}

#else
// ---------------------------------------------------------------- normal mode
static constexpr int MAX_DET  = 96;
static constexpr int MAX_SEEN = 400;

Detection          g_dets[MAX_DET];
sigdb::ScoreResult g_score[MAX_DET];  // aligned with g_dets
int                g_detCount = 0;
bool               g_linkOk   = false;

Touch    g_touch;
bool     g_touchOk    = false;   // calibration loaded/valid -> touch selection enabled
uint8_t  g_brightness = 100;     // backlight %, persisted in NVS

// URL of the hosted control web app (GitHub Pages). Scan the QR to open it.
static const char* APP_URL = "https://negativeaffirmations.github.io/cyd-scanner/webapp/";

// --- On-device navigation. Two inputs: the BOOT button (always works) and the
// touchscreen (once calibrated). A short tap of BOOT moves through the menu; a long
// hold selects, and a long hold from any screen returns to the menu. With touch, tap
// an item to select it directly. ---
enum Screen { SCR_MENU, SCR_SCAN, SCR_APPQR, SCR_SETTINGS };
static Screen g_screen  = SCR_MENU;
static int    g_menuSel = 0;
static int    g_setSel  = 0;
static const char* kMenuItems[] = { "Phone Link", "Start Scan", "Settings" };
static constexpr int MENU_N = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static constexpr int SET_N  = 3;  // Calibrate Touch / Brightness / Back

// Shared menu-row geometry (used for drawing AND touch hit-testing).
static constexpr int ROW_Y0 = 80, ROW_STEP = 40, ROW_H = 34;

// Scan-screen "stop" button (full-width bar under the status bar).
static constexpr int STOP_X = 4, STOP_Y = 24, STOP_H = 18;

enum BtnEv { BTN_NONE, BTN_SHORT, BTN_LONG };
static constexpr uint32_t LONG_PRESS_MS = 550;

// Poll the BOOT button (GPIO0, active low). Returns a SHORT event on a quick
// tap-release, and a LONG event once the hold passes the threshold (while still
// held, so it feels responsive). Call frequently.
static BtnEv buttonEvent() {
  static bool     prevLow   = false;
  static uint32_t downAt    = 0;
  static bool     longFired = false;
  bool  low = (digitalRead(0) == LOW);
  BtnEv ev  = BTN_NONE;
  if (low && !prevLow) { downAt = millis(); longFired = false; }        // pressed
  else if (low && !longFired && millis() - downAt >= LONG_PRESS_MS) {   // held long
    ev = BTN_LONG; longFired = true;
  } else if (!low && prevLow && !longFired && millis() - downAt < LONG_PRESS_MS) {
    ev = BTN_SHORT;                                                     // quick release
  }
  prevLow = low;
  return ev;
}

// Send a Ping and wait briefly for the C5's Pong — used to light the link dot on the
// menu BEFORE any scan is started. Safe when idle: the C5 answers Ping immediately
// (scanning is async and never blocks its link reader).
static bool pingC5(uint32_t timeoutMs = 400) {
  while (LinkSerial.available()) LinkSerial.read();
  parser.reset();
  sendFrame((uint8_t)Command::Ping, nullptr, 0);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (LinkSerial.available())
      if (parser.feed(LinkSerial.read()) && parser.type() == (uint8_t)Reply::Pong) return true;
    delay(2);
  }
  return false;
}

// --- Backlight brightness: LEDC PWM on TFT_BL_PIN, persisted in NVS ---
static void applyBrightness() {
  ledcWrite(TFT_BL_PIN, map(g_brightness, 0, 100, 20, 255));  // floor so never fully dark
}
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

// Touch hit-test over the shared menu-row geometry. On a fresh touch-down edge,
// returns the tapped row index (0..count-1), else -1. No-op until touch is calibrated.
static int tappedRow(int count) {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return -1; }
  bool now = g_touch.touched();
  int  hit = -1;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z))
      for (int i = 0; i < count; i++) {
        int y = ROW_Y0 + i * ROW_STEP;
        if (sx >= 6 && sx <= tft.width() - 6 && sy >= y - 5 && sy <= y - 5 + ROW_H) { hit = i; break; }
      }
  }
  prev = now;
  return hit;
}

// True on a fresh tap inside the scan screen's top "stop" button.
static bool stopButtonTapped() {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return false; }
  bool now = g_touch.touched();
  bool hit = false;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z))
      hit = (sx >= STOP_X && sx <= tft.width() - STOP_X &&
             sy >= STOP_Y && sy <= STOP_Y + STOP_H);
  }
  prev = now;
  return hit;
}

SPIClass  sdSPI(HSPI);
bool      g_sdOk = false;
char      g_logPath[48] = "/scanlog.csv";  // current session log; set in openSession()
bool      g_logNamed    = false;           // true once renamed to the date-time form
bool      g_logStarted  = false;           // true once the file is actually created (first row)
static const char* kLogHeader =
    "epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,ie,cid,uuid,name,score,tier,signature";

struct SeenKey { uint8_t source; uint8_t mac[6]; };
SeenKey g_seen[MAX_SEEN];
int     g_seenCount = 0;

static bool seenContains(const Detection& d) {
  for (int i = 0; i < g_seenCount; i++)
    if (g_seen[i].source == d.source && memcmp(g_seen[i].mac, d.mac, 6) == 0) return true;
  return false;
}
static void seenAdd(const Detection& d) {
  if (g_seenCount >= MAX_SEEN) return;
  g_seen[g_seenCount].source = d.source;
  memcpy(g_seen[g_seenCount].mac, d.mac, 6);
  g_seenCount++;
}

static void countBands(int& n24, int& n5, int& nble, int& nprb) {
  n24 = n5 = nble = nprb = 0;
  for (int i = 0; i < g_detCount; i++) {
    switch (g_dets[i].source) {
      case (uint8_t)Source::BleScan:   nble++; break;
      case (uint8_t)Source::WifiProbe: nprb++; break;
      default: (g_dets[i].channel > 14 ? n5 : n24)++; break;
    }
  }
}

// Score every current detection against the signature DB (aligned into g_score[]).
static void computeScores() {
  for (int i = 0; i < g_detCount; i++) sigdb::score(g_dets[i], g_score[i]);
}

// Fill idx[0..g_detCount) with detection indices sorted threat-tier-first then
// RSSI — the order shown on screen and streamed to the phone. Returns the count.
static int buildSorted(int* idx) {
  for (int i = 0; i < g_detCount; i++) idx[i] = i;
  std::sort(idx, idx + g_detCount, [](int a, int b) {
    if (g_score[a].tier != g_score[b].tier) return g_score[a].tier > g_score[b].tier;
    return g_dets[a].rssi > g_dets[b].rssi;
  });
  return g_detCount;
}

static void countTiers(int& susp, int& lk, int& conf) {
  susp = lk = conf = 0;
  for (int i = 0; i < g_detCount; i++) {
    switch (g_score[i].tier) {
      case sigdb::Tier::Suspect:   susp++; break;
      case sigdb::Tier::Likely:    lk++;   break;
      case sigdb::Tier::Confirmed: conf++; break;
      default: break;
    }
  }
}

// Delete leftover header-only session logs (no detections were ever written). Past boots
// created a file up front, so a card can accumulate many ~90-byte empties; sweep them on
// startup. A file with even one data row is larger than the header and is kept.
static void sweepEmptySessions() {
  const size_t emptyMax = strlen(kLogHeader) + 2;  // header + CRLF, nothing else
  File dir = SD.open("/logs");
  if (!dir) return;
  char victims[16][48];
  int nv = 0;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    if (!e.isDirectory() && e.size() <= emptyMax && nv < 16) {
      String nm = e.name();                       // may be a full path or a bare basename
      int slash = nm.lastIndexOf('/');
      String base = slash >= 0 ? nm.substring(slash + 1) : nm;
      snprintf(victims[nv++], sizeof(victims[0]), "/logs/%s", base.c_str());
    }
    e.close();
  }
  dir.close();
  for (int i = 0; i < nv; i++) {
    if (SD.remove(victims[i])) Serial.printf("[CYD] removed empty session log %s\n", victims[i]);
  }
}

// Start a fresh per-boot session log under /logs/. Named by a boot counter so it works
// before the phone has synced wall-clock time; renamed to /logs/YYYYMMDD-HHMMSS.csv once
// time is known (renameSessionOnSync). The file itself is created lazily on the first
// logged detection (ensureLogFile), so idle boots leave no empty file behind. Rows still
// carry absolute epoch after sync.
static void openSession() {
  SD.mkdir("/logs");
  if (SD.exists("/scanlog.csv")) SD.remove("/scanlog.csv");  // retire the legacy single file
  sweepEmptySessions();
  Preferences p; p.begin("cydscan", false);
  uint32_t boot = p.getULong("bootcnt", 0) + 1;
  p.putULong("bootcnt", boot);
  p.end();
  snprintf(g_logPath, sizeof(g_logPath), "/logs/sess-%05lu.csv", (unsigned long)boot);
  g_logNamed   = false;
  g_logStarted = false;
  Serial.printf("[CYD] session log (created on first detection): %s\n", g_logPath);
}

// Create the current session file with its header the first time a row needs writing.
// No-op once created. Returns false if SD is unavailable or the file can't be opened.
static bool ensureLogFile() {
  if (g_logStarted) return true;
  if (!g_sdOk) return false;
  File f = SD.open(g_logPath, FILE_WRITE);
  if (!f) return false;
  f.println(kLogHeader);
  f.close();
  g_logStarted = true;
  Serial.printf("[CYD] session log created: %s\n", g_logPath);
  return true;
}

// Once the phone provides wall-clock time, rename the boot-counter session file to a
// date-time name. Runs once per session (even if the rename fails, it won't retry-spam).
static void renameSessionOnSync() {
  if (g_logNamed || !g_sdOk || !phone::hasTime()) return;
  time_t t = (time_t)phone::epochNow();
  struct tm tmv;
  gmtime_r(&t, &tmv);  // epoch is stored already-localized, so gmtime gives local fields
  char nn[48];
  snprintf(nn, sizeof(nn), "/logs/%04d%02d%02d-%02d%02d%02d.csv",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  if (SD.exists(nn)) { g_logNamed = true; return; }  // name taken; keep the current one
  if (!g_logStarted) {
    // File not created yet (no detections logged): just adopt the date-time name so the
    // file is born correctly named when the first row is written.
    strncpy(g_logPath, nn, sizeof(g_logPath) - 1);
    g_logPath[sizeof(g_logPath) - 1] = 0;
    webshare::setLogPath(g_logPath);
    Serial.printf("[CYD] session log will use %s\n", g_logPath);
  } else if (SD.rename(g_logPath, nn)) {
    Serial.printf("[CYD] session log renamed %s -> %s\n", g_logPath, nn);
    strncpy(g_logPath, nn, sizeof(g_logPath) - 1);
    g_logPath[sizeof(g_logPath) - 1] = 0;
    webshare::setLogPath(g_logPath);
  }
  g_logNamed = true;
}

// Bump DB_GEN to force a one-time delete of /signatures.csv so sigdb re-seeds it from
// the firmware (e.g. after adding seed rules). This DISCARDS any on-card DB edits, so
// only bump it when that's intended.
static constexpr uint32_t DB_GEN = 1;  // 1: Phase-3 seed adds the Flock GATT bleuuid rule

static void reseedDbIfNeeded() {
  Preferences p;
  p.begin("cydscan", false);
  uint32_t gen = p.getULong("dbgen", 0);
  if (gen != DB_GEN) {
    if (SD.exists("/signatures.csv")) SD.remove("/signatures.csv");
    p.putULong("dbgen", DB_GEN);
    Serial.printf("[CYD] DB reseed (gen %lu -> %lu); signatures.csv rewritten from firmware\n",
                  (unsigned long)gen, (unsigned long)DB_GEN);
  }
  p.end();
}

static void initSD() {
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  g_sdOk = SD.begin(SD_CS_PIN, sdSPI) && SD.cardType() != CARD_NONE;
  if (!g_sdOk) { Serial.println("[CYD] SD unavailable - logging disabled"); return; }
  reseedDbIfNeeded();  // delete an outdated /signatures.csv so sigdb::begin() re-seeds it
  openSession();       // create this boot's /logs/sess-NNNNN.csv (renamed on time sync)
  webshare::setLogPath(g_logPath);
  Serial.println("[CYD] SD ready");
}

static int logNewDetections() {
  int n = 0;
  File f;  // opened lazily on the first new detection so idle boots write no file
  uint32_t ms = millis();
  uint32_t epoch = phone::epochNow();
  bool gps = phone::hasGps();
  for (int i = 0; i < g_detCount; i++) {
    const Detection& d = g_dets[i];
    if (seenContains(d)) continue;
    seenAdd(d);
    n++;
    if (!f && ensureLogFile()) f = SD.open(g_logPath, FILE_APPEND);
    if (!f) continue;
    char safe[33];
    strncpy(safe, d.name, sizeof(safe) - 1);
    safe[sizeof(safe) - 1] = 0;
    for (char* p = safe; *p; ++p) if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    const sigdb::ScoreResult& sc = g_score[i];
    char sig[24];
    strncpy(sig, sigdb::labelFor(sc), sizeof(sig) - 1);
    sig[sizeof(sig) - 1] = 0;
    for (char* p = sig; *p; ++p) if (*p == ',' || *p == '\n' || *p == '\r') *p = ' ';
    char uuidStr[33]; uuidStr[0] = 0;
    for (int k = 0; k < 16; k++)
      if (d.svc[k]) { for (int j = 0; j < 16; j++) sprintf(uuidStr + j * 2, "%02X", d.svc[j]); break; }
    f.printf("%lu,%lu,", (unsigned long)epoch, (unsigned long)ms);
    if (gps) f.printf("%.6f,%.6f,", phone::lat(), phone::lon());
    else     f.print(",,");
    f.printf("%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%d,%08lX,%04X,%s,%s,%d,%s,%s\n", srcTag(d),
             d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
             d.rssi, d.channel, (unsigned long)d.ie_hash, d.companyId, uuidStr, safe,
             sc.score, sigdb::tierName(sc.tier), sig);
  }
  if (f) f.close();
  return n;
}

// Stream an SD file to the phone over BLE: a "SIZE=<n>" header, then the raw file in
// chunks. The web app reassembles and saves it (no Wi-Fi needed).
static void streamFileOverBle(const char* path) {
  if (!g_sdOk || !SD.exists(path)) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE dl: no file %s\n", path);
    return;
  }
  File f = SD.open(path, "r");
  if (!f) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  size_t sz = f.size();
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SIZE=%u", (unsigned)sz);
  phone::logNotify((const uint8_t*)hdr, hn);
  delay(30);
  uint8_t buf[180];  // chunk < MTU-3 (Android negotiates a large MTU)
  size_t sent = 0;
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    phone::logNotify(buf, n);
    sent += n;
    delay(15);
  }
  f.close();
  Serial.printf("[CYD] BLE sent %u/%u bytes of %s\n", (unsigned)sent, (unsigned)sz, path);
}

static void transferLog() { streamFileOverBle(g_logPath); }  // current session ("L")

// Download a specific session file requested by the phone ("F:<path>"). Restricted to
// /logs/ (no path traversal) so the phone can't pull arbitrary SD files.
static void transferFile(const char* path) {
  if (strncmp(path, "/logs/", 6) != 0 || strstr(path, "..")) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE dl: rejected path %s\n", path);
    return;
  }
  streamFileOverBle(path);
}

// Delete a session file the phone selected ("D:<path>"). Restricted to /logs/ (no
// traversal) and refuses the live session being written. Pushes the refreshed list back.
static void sendSessionList();  // fwd decl
static void deleteSession(const char* path) {
  if (strncmp(path, "/logs/", 6) != 0 || strstr(path, "..")) {
    Serial.printf("[CYD] delete rejected (bad path) %s\n", path);
  } else if (strcmp(path, g_logPath) == 0) {
    Serial.println("[CYD] delete refused: that's the current session");
  } else {
    if (SD.exists(path)) SD.remove(path);
    Serial.printf("[CYD] deleted %s\n", path);
  }
  sendSessionList();  // refresh the phone's picker either way
}

static String logBase(const String& nm) {  // dir entry -> bare basename (core may add a path)
  int slash = nm.lastIndexOf('/');
  return slash >= 0 ? nm.substring(slash + 1) : nm;
}

// Send the list of session logs to the phone: a "SESS=<n>" header then <n> bytes of
// "<path>\t<size>\t<flag>\n" lines. <flag> marks the newest file: "current" while a scan
// is running (its file is being written), else "latest"; blank for all other rows. The web
// app parses it into a picker and labels/floats the flagged row.
static void sendSessionList() {
  // The newest file is the current session when its file exists this boot; otherwise the
  // most recently written prior file (by FS timestamp).
  String curBase = logBase(String(g_logPath));
  String newestBase;
  if (g_logStarted) {
    newestBase = curBase;
  } else {
    time_t newestT = -1;
    File d = SD.open("/logs");
    if (d) {
      for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        if (!e.isDirectory()) {
          time_t t = e.getLastWrite();
          if (t >= newestT) { newestT = t; newestBase = logBase(String(e.name())); }
        }
        e.close();
      }
      d.close();
    }
  }
  const char* flagWord = (g_logStarted && g_screen == SCR_SCAN) ? "current" : "latest";

  String list;
  File dir = SD.open("/logs");
  if (dir) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (e.isDirectory()) continue;
      String base = logBase(String(e.name()));
      const char* flag = (newestBase.length() && base == newestBase) ? flagWord : "";
      list += "/logs/" + base + "\t" + String((uint32_t)e.size()) + "\t" + flag + "\n";
    }
    dir.close();
  }
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SESS=%u", (unsigned)list.length());
  phone::logNotify((const uint8_t*)hdr, hn);
  delay(30);
  const char* p = list.c_str();
  size_t rem = list.length();
  while (rem) {
    size_t n = rem > 180 ? 180 : rem;
    phone::logNotify((const uint8_t*)p, n);
    p += n; rem -= n;
    delay(15);
  }
  Serial.printf("[CYD] session list sent (%u bytes)\n", (unsigned)list.length());
}

// This device's own BLE MAC, cached at boot so we can drop our own advertisement
// from the scan results (the CYD's phone-link peripheral is visible to the C5's BLE
// scan). Captured at runtime -> device-agnostic, works on any CYD unit.
static uint8_t g_ownMac[6]   = {0};
static bool    g_haveOwnMac  = false;

static bool isSelfDet(const Detection& d) {
  return g_haveOwnMac && d.source == (uint8_t)Source::BleScan &&
         memcmp(d.mac, g_ownMac, 6) == 0;
}

static void requestScan(uint32_t timeoutMs = 5000) {
  g_detCount = 0;
  bool sawStart = false, done = false;
  uint8_t peer[6]; bool havePeer = phone::peerMac(peer);  // connected phone (dynamic)
  while (LinkSerial.available()) LinkSerial.read();
  parser.reset();
  ScanConfig cfg{};
  cfg.sources  = MASK_WIFI | MASK_BLE | MASK_PROBE;
  cfg.dwell_ms = 0;
  sendFrame((uint8_t)Command::StartScan, &cfg, sizeof(cfg));
  uint32_t t0 = millis();
  while (!done && millis() - t0 < timeoutMs) {
    while (LinkSerial.available()) {
      if (!parser.feed(LinkSerial.read())) continue;
      uint8_t t = parser.type();
      if (t == (uint8_t)Reply::Detection && parser.length() >= sizeof(Detection)) {
        if (g_detCount < MAX_DET) {
          Detection& nd = g_dets[g_detCount];
          memcpy(&nd, parser.payload(), sizeof(Detection));
          bool isPhone = havePeer && nd.source == (uint8_t)Source::BleScan &&
                         memcmp(nd.mac, peer, 6) == 0;
          if (!isSelfDet(nd) && !isPhone) g_detCount++;  // drop our own + the paired phone
        }
      } else if (t == (uint8_t)Reply::Status && parser.length() >= sizeof(Status)) {
        const Status* st = reinterpret_cast<const Status*>(parser.payload());
        if (st->scanning != 0) sawStart = true;
        else                   done     = true;
      }
    }
  }
  g_linkOk = done || sawStart || g_detCount > 0;
}

static void pushStatus() {
  int n24, n5, nble, nprb; countBands(n24, n5, nble, nprb);
  int susp, lk, conf; countTiers(susp, lk, conf);
  char s[224];
  snprintf(s, sizeof(s),
           "link=%d;w24=%d;w5=%d;ble=%d;prb=%d;uniq=%d;time=%d;gps=%d;dl=%d;"
           "susp=%d;lk=%d;conf=%d;db=%d;scan=%d;bri=%d",
           g_linkOk ? 1 : 0, n24, n5, nble, nprb, g_seenCount,
           phone::hasTime() ? 1 : 0, phone::hasGps() ? 1 : 0,
           webshare::active() ? 1 : 0, susp, lk, conf, sigdb::loaded() ? 1 : 0,
           g_screen == SCR_SCAN ? 1 : 0, g_brightness);
  phone::setStatus(String(s));
}

// Stream the live detection list to the phone so its app mirrors the CYD screen:
// a "D:<count>" header then one row per device (top-of-list first). Tab-separated
// so the app can split cleanly; the name is last and stripped of tabs. Capped so
// the burst stays small on the BLE link.
static constexpr int DETS_STREAM_MAX = 12;
static void pushDetections() {
  if (!phone::connected()) return;
  static int idx[MAX_DET];
  buildSorted(idx);
  int n = min(g_detCount, DETS_STREAM_MAX);
  char hdr[16];
  snprintf(hdr, sizeof(hdr), "D:%d", n);
  phone::detsNotify(String(hdr));
  for (int r = 0; r < n; r++) {
    int i = idx[r];
    const Detection& d = g_dets[i];
    char name[24];
    strncpy(name, d.name[0] ? d.name : "<hidden>", sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    for (char* p = name; *p; ++p) if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    char row[96];
    snprintf(row, sizeof(row), "%d\t%s\t%d\t%02X:%02X:%02X:%02X:%02X:%02X\t%08lX\t%s",
             (int)g_score[i].tier, srcTag(d), d.rssi,
             d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
             (unsigned long)d.ie_hash, name);
    phone::detsNotify(String(row));
    delay(6);  // let the BLE stack drain each notification
  }
}

// --- status-bar icons (drawn with primitives, ~14 px) ---
static void iconBle(int x, int y, uint16_t c) {  // stylized Bluetooth rune
  int cx = x + 5;
  tft.drawLine(cx, y,     cx, y + 14, c);
  tft.drawLine(cx, y,     x + 9, y + 4,  c);
  tft.drawLine(x + 9, y + 4, cx, y + 7,  c);
  tft.drawLine(cx, y + 7, x + 9, y + 10, c);
  tft.drawLine(x + 9, y + 10, cx, y + 14, c);
  tft.drawLine(x + 1, y + 4, cx, y + 7, c);
  tft.drawLine(x + 1, y + 10, cx, y + 7, c);
}
static void iconWifi(int x, int y, uint16_t c) {  // fan of arcs + node
  int cx = x + 7, cy = y + 12;
  tft.fillCircle(cx, cy, 1, c);
  tft.drawCircleHelper(cx, cy, 4, 0x3, c);   // top two corners
  tft.drawCircleHelper(cx, cy, 7, 0x3, c);
  tft.drawCircleHelper(cx, cy, 10, 0x3, c);
}
static void iconNoConn(int x, int y, uint16_t c) {  // circle with a slash
  int cx = x + 7, cy = y + 7;
  tft.drawCircle(cx, cy, 6, c);
  tft.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, c);
}

static void drawStatusBar() {
  int W = tft.width();
  tft.fillRect(0, 0, W, 21, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  // time (left) — local, from the phone's timezone
  char t[8];
  if (phone::hasTime()) {
    uint32_t e = phone::epochNow();
    snprintf(t, sizeof(t), "%02u:%02u", (unsigned)((e / 3600) % 24), (unsigned)((e / 60) % 60));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  } else { strcpy(t, "00:00"); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); }
  tft.drawString(t, 4, 4, 2);
  // GPS (middle, abbreviated)
  char g[24];
  if (phone::hasGps()) {
    snprintf(g, sizeof(g), "%.2f,%.2f", phone::lat(), phone::lon());
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  } else { strcpy(g, "Lat:- Lon:-"); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); }
  tft.drawString(g, 66, 7, 1);
  // phone connection icon (right) + C5-link status dot to its right
  int ix = W - 30;
  if (webshare::active())      iconWifi(ix, 3, TFT_CYAN);
  else if (phone::connected()) iconBle(ix + 2, 3, TFT_BLUE);
  else                         iconNoConn(ix, 3, TFT_DARKGREY);
  tft.fillCircle(W - 8, 10, 4, g_linkOk ? TFT_GREEN : TFT_RED);  // C5 link up/down
  tft.drawFastHLine(0, 21, W, TFT_DARKGREY);
}

static void render() {
  int n24, n5, nble, nprb; countBands(n24, n5, nble, nprb);
  int susp, lk, conf; countTiers(susp, lk, conf);
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();

  // Full-width stop button across the top: tap to end the scan and return to the
  // menu (a long BOOT hold does the same).
  int W = tft.width();
  tft.fillRoundRect(STOP_X, STOP_Y, W - 2 * STOP_X, STOP_H, 4, TFT_MAROON);
  tft.drawRoundRect(STOP_X, STOP_Y, W - 2 * STOP_X, STOP_H, 4, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("STOP", W / 2, STOP_Y + STOP_H / 2, 1);

  tft.setTextDatum(TL_DATUM);
  char buf[48];
  // Link status lives in the status-bar dot; this line covers SD + DB.
  tft.setTextColor(g_sdOk ? TFT_WHITE : TFT_RED, TFT_BLACK);
  snprintf(buf, sizeof(buf), "SD:%s  db:%s",
           g_sdOk ? "on" : "off", sigdb::loaded() ? "on" : "fb");
  tft.drawString(buf, 4, 46, 1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "2.4:%d 5G:%d BLE:%d PRB:%d U:%d", n24, n5, nble, nprb, g_seenCount);
  tft.drawString(buf, 4, 58, 1);

  uint16_t tcol = conf ? TFT_RED : lk ? TFT_ORANGE : susp ? TFT_YELLOW : TFT_DARKGREY;
  tft.setTextColor(tcol, TFT_BLACK);
  snprintf(buf, sizeof(buf), "threats  S:%d  L:%d  C:%d", susp, lk, conf);
  tft.drawString(buf, 4, 70, 1);

  // Sort by threat tier first, then RSSI, so flagged devices surface at the top.
  static int idx[MAX_DET];
  buildSorted(idx);
  int maxRows = (tft.height() - 84) / 13;  // rows from y84, 13 px each
  int rows = min(g_detCount, maxRows);
  for (int r = 0; r < rows; r++) {
    int i = idx[r];
    const Detection& d = g_dets[i];
    uint16_t col;
    switch (g_score[i].tier) {
      case sigdb::Tier::Confirmed: col = TFT_RED;    break;
      case sigdb::Tier::Likely:    col = TFT_ORANGE; break;
      case sigdb::Tier::Suspect:   col = TFT_YELLOW; break;
      default: col = d.source == (uint8_t)Source::BleScan ? TFT_MAGENTA
                   : d.channel > 14 ? TFT_CYAN : TFT_WHITE;
    }
    tft.setTextColor(col, TFT_BLACK);
    char name[16];
    strncpy(name, d.name[0] ? d.name : "<hidden>", 15);
    name[15] = 0;
    char flag = g_score[i].tier != sigdb::Tier::None ? '!' : ' ';
    snprintf(buf, sizeof(buf), "%c%-3s %-15s %4d", flag, srcTag(d), name, d.rssi);
    tft.drawString(buf, 4, 84 + r * 13, 1);
  }
}

// Draw a QR centered horizontally at the given top y; returns the y just below it.
static int drawCenteredQr(const char* text, int qy) {
  QRCode qr;
  static uint8_t qrbuf[256];  // fits version 4
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, text);
  const int scale = 6, side = qr.size * scale;
  const int qx = (tft.width() - side) / 2;
  tft.fillRect(qx - 6, qy - 6, side + 12, side + 12, TFT_WHITE);  // quiet zone
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        tft.fillRect(qx + x * scale, qy + y * scale, scale, scale, TFT_BLACK);
  return qy + side + 6;
}

static void drawDownloadScreen() {
  tft.fillScreen(TFT_BLACK);
  int W = tft.width();
  tft.setTextDatum(MC_DATUM);
  bool joined = webshare::clientConnected();

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(joined ? "OPEN DOWNLOAD" : "JOIN WI-FI", W / 2, 14, 2);

  if (joined) {
    // Phone is on the AP — QR now opens the download page directly.
    int ty = drawCenteredQr(webshare::url(), 40) + 12;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Scan to open & download", W / 2, ty, 2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString(webshare::url(), W / 2, ty + 18, 2);
  } else {
    // Not joined yet — QR joins the Wi-Fi network.
    int ty = drawCenteredQr(webshare::wifiQr().c_str(), 40) + 12;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Scan to join Wi-Fi", W / 2, ty, 2);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(String("SSID ") + webshare::ssid(), W / 2, ty + 18, 1);
    tft.drawString(String("PW ") + webshare::password(), W / 2, ty + 30, 1);
  }
}

// QR linking to the hosted web app, so the phone can open it by scanning.
static void drawAppQrScreen() {
  tft.fillScreen(TFT_BLACK);
  int W = tft.width();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("CONNECT TO PHONE", W / 2, 14, 2);
  int ty = drawCenteredQr(APP_URL, 40) + 14;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan to open the app", W / 2, ty, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Press button: back to menu", W / 2, ty + 20, 2);
}

// Home menu. Reuses the status bar (time / GPS / connection / link dot) up top.
static void drawMenu() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  int W = tft.width();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("HOME", W / 2, 32, 4);
  // Button text is centered in the row; font-4 (26px) in the 34px row leaves 4px
  // of padding above and below.
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == g_menuSel);
    int  y   = ROW_Y0 + i * ROW_STEP;
    if (sel) tft.fillRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_NAVY);
    else     tft.drawRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? TFT_NAVY : TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(kMenuItems[i], W / 2, y - 5 + ROW_H / 2, 4);
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(g_touchOk ? "Tap an item, or BOOT: tap=next hold=select"
                           : "BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// Settings screen: Calibrate Touch / Brightness / Back.
static void drawSettings() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  int W = tft.width();
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("SETTINGS", 10, 32, 4);
  char bright[24];
  snprintf(bright, sizeof(bright), "Brightness: %d%%", g_brightness);
  const char* items[SET_N] = { "Calibrate Touch", bright, "Back" };
  for (int i = 0; i < SET_N; i++) {
    bool sel = (i == g_setSel);
    int  y   = ROW_Y0 + i * ROW_STEP;
    if (sel) tft.fillRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_NAVY);
    else     tft.drawRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? TFT_NAVY : TFT_BLACK);
    tft.drawString(items[i], 18, y, 4);
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(g_touchOk ? "Touch OK - tap an item" : "Touch not calibrated yet",
                 10, tft.height() - 18, 1);
}

// Act on a settings row (touch tap or long-press select).
static void activateSettings(int sel) {
  if (sel == 0) {                                  // Calibrate Touch
    g_touch.calibrate(tft);
    g_touchOk = g_touch.cal().valid;
    drawSettings();
  } else if (sel == 1) {                           // Brightness: cycle 25/50/75/100%
    g_brightness = (g_brightness >= 100) ? 25 : (uint8_t)(g_brightness + 25);
    applyBrightness();
    saveBrightness();
    drawSettings();
  } else {                                         // Back
    g_screen = SCR_MENU; g_menuSel = 0; drawMenu();
  }
}

// One scan → score → log → render cycle, also mirrored to the phone app.
static void runScanCycle() {
  requestScan();
  computeScores();
  setLed(!g_linkOk, g_linkOk, false);
  int newCount = logNewDetections();
  int n24, n5, nble, nprb; countBands(n24, n5, nble, nprb);
  int susp, lk, conf; countTiers(susp, lk, conf);
  Serial.printf("[CYD] table=%d (2.4:%d 5G:%d BLE:%d PRB:%d) new=%d uniq=%d threats S%d/L%d/C%d db=%d\n",
                g_detCount, n24, n5, nble, nprb, newCount, g_seenCount,
                susp, lk, conf, sigdb::loaded());
  render();
  pushStatus();
  pushDetections();
}

// Phone-initiated Wi-Fi bulk-download mode: SoftAP + web server + on-screen QR.
static void runDownload() {
  static int lastJoined = -1;
  if (!webshare::active()) { webshare::start(); lastJoined = -1; }
  int joined = webshare::clientConnected() ? 1 : 0;
  if (joined != lastJoined) { lastJoined = joined; drawDownloadScreen(); }  // swap QR
  webshare::handle();
  static uint32_t lastS = 0;
  if (millis() - lastS > 1000) { lastS = millis(); pushStatus(); }
  delay(2);
}

// Act on the highlighted menu item (touch tap or long-press select).
static void activateMenu() {
  if (g_menuSel == 0)      { g_screen = SCR_APPQR;    drawAppQrScreen(); }
  else if (g_menuSel == 1) g_screen = SCR_SCAN;                    // first cycle draws it
  else if (g_menuSel == 2) { g_screen = SCR_SETTINGS; g_setSel = 0; drawSettings(); }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[CYD] boot: scanner + phone link + SD");
  initCommon();
  loadBrightness();
  applyBrightness();
  initSD();
  sigdb::begin();  // load /signatures.csv (seeds it if absent) or fall back
  g_touch.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN, TOUCH_IRQ_PIN);
  g_touchOk = g_touch.loadCal();
  phone::begin(DEVICE_NAME);
  g_haveOwnMac = phone::ownMac(g_ownMac);  // for self-detection filtering
  g_linkOk = pingC5();  // check the C5 link so the menu dot is correct before any scan
  Serial.printf("[CYD] ready (link=%s, touch=%s, bright=%d%%)\n",
                g_linkOk ? "up" : "down", g_touchOk ? "cal" : "uncal", g_brightness);
  drawMenu();      // start on the home menu
}

void loop() {
  // Phone commands are honored from any screen.
  if (phone::reloadRequested()) sigdb::reload();
  if (phone::logRequested())    transferLog();
  if (phone::listRequested())   sendSessionList();
  { char fn[48]; if (phone::fileRequested(fn, sizeof(fn)))   transferFile(fn); }
  { char fn[48]; if (phone::deleteRequested(fn, sizeof(fn))) deleteSession(fn); }
  { int b; if (phone::brightnessRequested(&b)) {           // web-app brightness slider
      g_brightness = (uint8_t)constrain(b, 10, 100);
      applyBrightness(); saveBrightness();
    } }
  renameSessionOnSync();  // give this session a date-time filename once time is known

  // Phone-initiated Wi-Fi download overrides the current screen while active.
  if (phone::downloadRequested()) { runDownload(); return; }
  if (webshare::active()) {  // just left download mode: tear down AP and repaint
    webshare::stop();
    g_screen = SCR_MENU; g_menuSel = 0; drawMenu(); pushStatus();
  }

  // Phone can start/stop the scan remotely (single toggle in the web app).
  if (phone::scanStartRequested()) g_screen = SCR_SCAN;
  if (phone::scanStopRequested())  { g_screen = SCR_MENU; drawMenu(); pushStatus(); }

  BtnEv ev = buttonEvent();

  switch (g_screen) {
    case SCR_MENU: {
      int t = tappedRow(MENU_N);
      if (t >= 0)               { g_menuSel = t; activateMenu(); return; }  // touch select
      if      (ev == BTN_SHORT) { g_menuSel = (g_menuSel + 1) % MENU_N; drawMenu(); }
      else if (ev == BTN_LONG)  { activateMenu(); }
      else {  // idle: re-check the C5 link and refresh the status bar (dot + clock)
        static uint32_t lastPing = 0;
        if (millis() - lastPing > 2000) {
          lastPing = millis();
          g_linkOk = pingC5();
          drawStatusBar();  // just the top bar — no full-screen flicker
          pushStatus();     // keep the phone informed (incl. scan=0) while idle
        }
      }
      delay(20);
      return;
    }

    case SCR_SETTINGS: {
      int t = tappedRow(SET_N);
      if (t >= 0)               { g_setSel = t; activateSettings(t); return; }  // touch select
      if      (ev == BTN_SHORT) { g_setSel = (g_setSel + 1) % SET_N; drawSettings(); }
      else if (ev == BTN_LONG)  { activateSettings(g_setSel); }
      delay(20);
      return;
    }

    case SCR_APPQR:
      if (ev != BTN_NONE) { g_screen = SCR_MENU; drawMenu(); }  // any press: back
      delay(20);
      return;

    case SCR_SCAN:
      // Stop via the on-screen button (touch), a long BOOT hold, or the phone.
      if (ev == BTN_LONG || stopButtonTapped()) { g_screen = SCR_MENU; drawMenu(); pushStatus(); return; }
      runScanCycle();
      // Responsive ~2 s wait that also honors stop requests and pending downloads.
      {
        uint32_t t0 = millis();
        while (millis() - t0 < 2000) {
          if (buttonEvent() == BTN_LONG || stopButtonTapped() || phone::scanStopRequested()) {
            g_screen = SCR_MENU; drawMenu(); pushStatus(); return;
          }
          if (phone::downloadRequested()) return;
          delay(20);
        }
      }
      return;
  }
}
#endif

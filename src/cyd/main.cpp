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
static const char* kMenuItems[] = { "Start Scan", "Connect to Phone", "Settings" };
static constexpr int MENU_N = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static constexpr int SET_N  = 3;  // Calibrate Touch / Brightness / Back

// Shared menu-row geometry (used for drawing AND touch hit-testing).
static constexpr int ROW_Y0 = 80, ROW_STEP = 40, ROW_H = 34;

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

SPIClass  sdSPI(HSPI);
bool      g_sdOk = false;
const char* kLogPath = "/scanlog.csv";

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

// Bump LOG_GEN to force a one-time wipe of the SD log on the next boot.
static constexpr uint32_t LOG_GEN = 4;  // v4: added BLE 'cid' + 'uuid' columns

static void wipeLogsIfNeeded() {
  Preferences p;
  p.begin("cydscan", false);
  uint32_t gen = p.getULong("loggen", 0);
  if (gen != LOG_GEN) {
    if (SD.exists(kLogPath)) SD.remove(kLogPath);
    p.putULong("loggen", LOG_GEN);
    Serial.printf("[CYD] log wipe (gen %lu -> %lu); fresh file will be created\n",
                  (unsigned long)gen, (unsigned long)LOG_GEN);
  }
  p.end();
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
  wipeLogsIfNeeded();
  reseedDbIfNeeded();  // delete an outdated /signatures.csv so sigdb::begin() re-seeds it
  if (!SD.exists(kLogPath)) {
    File f = SD.open(kLogPath, FILE_WRITE);
    if (f) { f.println("epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,ie,cid,uuid,name,score,tier,signature"); f.close(); }
  }
  Serial.printf("[CYD] SD ready - logging to %s\n", kLogPath);
}

static int logNewDetections() {
  int n = 0;
  File f;
  if (g_sdOk) f = SD.open(kLogPath, FILE_APPEND);
  uint32_t ms = millis();
  uint32_t epoch = phone::epochNow();
  bool gps = phone::hasGps();
  for (int i = 0; i < g_detCount; i++) {
    const Detection& d = g_dets[i];
    if (seenContains(d)) continue;
    seenAdd(d);
    n++;
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

// Stream /scanlog.csv to the phone over BLE: first a "SIZE=<n>" header, then the
// raw file in chunks. The web app reassembles and downloads it (no Wi-Fi needed).
static void transferLog() {
  if (!g_sdOk || !SD.exists(kLogPath)) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.println("[CYD] BLE log: no file");
    return;
  }
  File f = SD.open(kLogPath, "r");
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
  Serial.printf("[CYD] BLE log sent %u/%u bytes\n", (unsigned)sent, (unsigned)sz);
}

static void requestScan(uint32_t timeoutMs = 5000) {
  g_detCount = 0;
  bool sawStart = false, done = false;
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
        if (g_detCount < MAX_DET)
          memcpy(&g_dets[g_detCount++], parser.payload(), sizeof(Detection));
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
  char s[192];
  snprintf(s, sizeof(s),
           "link=%d;w24=%d;w5=%d;ble=%d;prb=%d;uniq=%d;time=%d;gps=%d;dl=%d;"
           "susp=%d;lk=%d;conf=%d;db=%d",
           g_linkOk ? 1 : 0, n24, n5, nble, nprb, g_seenCount,
           phone::hasTime() ? 1 : 0, phone::hasGps() ? 1 : 0,
           webshare::active() ? 1 : 0, susp, lk, conf, sigdb::loaded() ? 1 : 0);
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

  tft.setTextDatum(TL_DATUM);
  char buf[48];
  // Link status now lives in the status-bar dot; this line covers SD + DB.
  tft.setTextColor(g_sdOk ? TFT_WHITE : TFT_RED, TFT_BLACK);
  snprintf(buf, sizeof(buf), "SD:%s  db:%s",
           g_sdOk ? "on" : "off", sigdb::loaded() ? "on" : "fb");
  tft.drawString(buf, 4, 26, 1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "2.4:%d 5G:%d BLE:%d PRB:%d U:%d", n24, n5, nble, nprb, g_seenCount);
  tft.drawString(buf, 4, 38, 1);

  uint16_t tcol = conf ? TFT_RED : lk ? TFT_ORANGE : susp ? TFT_YELLOW : TFT_DARKGREY;
  tft.setTextColor(tcol, TFT_BLACK);
  snprintf(buf, sizeof(buf), "threats  S:%d  L:%d  C:%d", susp, lk, conf);
  tft.drawString(buf, 4, 50, 1);

  // Sort by threat tier first, then RSSI, so flagged devices surface at the top.
  static int idx[MAX_DET];
  buildSorted(idx);
  int maxRows = (tft.height() - 64) / 13;  // rows from y64, 13 px each
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
    tft.drawString(buf, 4, 64 + r * 13, 1);
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
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("MAIN MENU", 10, 32, 4);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == g_menuSel);
    int  y   = ROW_Y0 + i * ROW_STEP;
    if (sel) tft.fillRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_NAVY);
    else     tft.drawRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? TFT_NAVY : TFT_BLACK);
    tft.drawString(kMenuItems[i], 18, y, 4);
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
  if (g_menuSel == 0)      g_screen = SCR_SCAN;                    // first cycle draws it
  else if (g_menuSel == 1) { g_screen = SCR_APPQR;    drawAppQrScreen(); }
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
  g_linkOk = pingC5();  // check the C5 link so the menu dot is correct before any scan
  Serial.printf("[CYD] ready (link=%s, touch=%s, bright=%d%%)\n",
                g_linkOk ? "up" : "down", g_touchOk ? "cal" : "uncal", g_brightness);
  drawMenu();      // start on the home menu
}

void loop() {
  // Phone commands are honored from any screen.
  if (phone::reloadRequested()) sigdb::reload();
  if (phone::logRequested())    transferLog();

  // Phone-initiated Wi-Fi download overrides the current screen while active.
  if (phone::downloadRequested()) { runDownload(); return; }
  if (webshare::active()) webshare::stop();  // just left download mode

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
      if (ev == BTN_LONG) { g_screen = SCR_MENU; drawMenu(); return; }
      runScanCycle();
      // Responsive ~2 s wait: a long hold returns to the menu; a pending phone
      // download is handled on the next loop.
      {
        uint32_t t0 = millis();
        while (millis() - t0 < 2000) {
          if (buttonEvent() == BTN_LONG) { g_screen = SCR_MENU; drawMenu(); return; }
          if (phone::downloadRequested()) return;
          delay(20);
        }
      }
      return;
  }
}
#endif

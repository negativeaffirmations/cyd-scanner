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
#include <ArduinoJson.h>
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
  if (d.source == (uint8_t)Source::Ieee802154) return "154";  // Zigbee / Thread
  return d.channel > 14 ? "5G" : "2.4";
}

// Copy a device-controlled free-text field, replacing delimiter-breaking characters
// (comma, tab, CR/LF and other control chars) with a space so plain delimiter-split
// parsing of the tab-delimited DETS rows stays correct (the log uses jsonEscape instead).
static void sanitizeField(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i]; i++) {
    char c = src[i];
    dst[i] = (c == ',' || (uint8_t)c < 0x20 || c == 0x7F) ? ' ' : c;
  }
  dst[i] = 0;
}

// Escape a device-controlled string for use inside a JSON string literal (no surrounding
// quotes): " \ and control chars (<0x20, 0x7F) become escapes. Bounded and NUL-terminated;
// stops before a whole escape sequence would overflow, so output is always valid JSON.
static void jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  for (size_t i = 0; src[i]; i++) {
    uint8_t c = (uint8_t)src[i];
    char esc[7];
    size_t n;
    if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; n = 2; }
    else if (c == '\n') { memcpy(esc, "\\n", 2); n = 2; }
    else if (c == '\r') { memcpy(esc, "\\r", 2); n = 2; }
    else if (c == '\t') { memcpy(esc, "\\t", 2); n = 2; }
    else if (c < 0x20 || c == 0x7F) { n = snprintf(esc, sizeof(esc), "\\u%04X", c); }
    else { esc[0] = (char)c; n = 1; }
    if (o + n + 1 > cap) break;
    memcpy(dst + o, esc, n);
    o += n;
  }
  if (cap) dst[o] = 0;
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
enum Screen { SCR_MENU, SCR_SCAN, SCR_APPQR, SCR_SETTINGS, SCR_SCANMENU, SCR_SCANSETTINGS,
              SCR_PICKLOG, SCR_SCANVIEWER, SCR_DETAIL, SCR_SORT, SCR_FILTER };
static Screen g_screen  = SCR_MENU;
static int    g_menuSel = 0;
static int    g_setSel  = 0;
static int    g_scanSel = 0;
static int    g_scanSetSel = 0;
static constexpr int SCANSET_N = 5;  // BLE / Wi-Fi 2.4 / Wi-Fi 5G / 802.15.4 / Back
static const char* kMenuItems[] = { "Phone Link", "Scan", "Settings" };
static constexpr int MENU_N = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static constexpr int SET_N  = 3;  // Calibrate Touch / Brightness / Back
static const char* kScanItems[] = { "Start Scan", "Explore Scan", "Scan Settings", "Back" };
static constexpr int SCAN_N = sizeof(kScanItems) / sizeof(kScanItems[0]);

// Shared menu-row geometry (used for drawing AND touch hit-testing).
static constexpr int ROW_Y0 = 80, ROW_STEP = 40, ROW_H = 34;

// Top-bar button (full-width bar under the status bar): STOP on the live scan, BACK in the
// scan viewer / detail screens.
static constexpr int STOP_X = 4, STOP_Y = 24, STOP_H = 18;

// Scan-screen device list geometry + scrolling (rows are one MAC group each).
static constexpr int LIST_Y0 = 84, LIST_ROW_H = 13;
static constexpr int SCROLL_R = 14, SCROLL_CX_INSET = 18;  // icon radius / centre inset from right edge
// Rows must keep their content left of this x so they never run under the scroll icons.
static constexpr int ICON_GUTTER_X = 240 - SCROLL_CX_INSET - SCROLL_R - 2;
static int g_scrollOffset = 0;  // group index of the first visible row

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

// --- Scan-source enable mask (ScanConfig.sources), persisted in NVS. The "2.4" toggle
// covers probe-request capture too (both are 2.4 GHz Wi-Fi). 802.15.4 has its own toggle. ---
static uint8_t g_srcMask = MASK_ALL;
static constexpr uint8_t SRC_24_BITS = MASK_WIFI24 | MASK_PROBE;
static void loadSrcMask() {
  Preferences p; p.begin("cydui", true);
  g_srcMask = p.getUChar("srcmask", MASK_ALL);
  p.end();
}
static void saveSrcMask() {
  Preferences p; p.begin("cydui", false);
  p.putUChar("srcmask", g_srcMask);
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

// True on a fresh tap inside the top-bar button (STOP / BACK).
// The bar can be split into `nseg` equal segments (see drawTopBarSeg); returns the tapped
// segment index (0..nseg-1) on a fresh tap, else -1.
static int topBarSegTapped(int nseg) {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return -1; }
  bool now = g_touch.touched();
  int  hit = -1;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z) && sx >= STOP_X && sx <= tft.width() - STOP_X &&
        sy >= STOP_Y && sy <= STOP_Y + STOP_H)
      hit = min(nseg - 1, (sx - STOP_X) * nseg / (tft.width() - 2 * STOP_X));
  }
  prev = now;
  return hit;
}
static bool topBarTapped() { return topBarSegTapped(1) == 0; }

SPIClass  sdSPI(HSPI);
bool      g_sdOk = false;
char      g_logPath[48] = "/scanlog.jsonl";  // current session log (NDJSON); set in openSession()
bool      g_logNamed    = false;           // true once renamed to the date-time form
bool      g_logStarted  = false;           // true once the file is actually created (first row)
// Legacy CSV header (schema v5): only used to recognize header-only old .csv files in the sweep.
static const char* kLogHeader =
    "epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,ie,cid,uuid,pan,name,score,tier,signature";
static constexpr size_t LOG_LINE = 512;  // max log line (NDJSON lines are ~100-330 B; worst case < 512)

struct SeenKey { uint8_t source; uint8_t mac[6]; uint16_t pan; };
SeenKey g_seen[MAX_SEEN];
int     g_seenCount = 0;

// Short-address 15.4 devices are only unique within a PAN, so key them by panId too
// (pan = 0 for every other source, which keeps their key source+MAC).
static uint16_t seenPan(const Detection& d) {
  return (d.source == (uint8_t)Source::Ieee802154 && !(d.flags & FLAG_154_EXTENDED)) ? d.panId : 0;
}
static bool seenContains(const Detection& d) {
  for (int i = 0; i < g_seenCount; i++)
    if (g_seen[i].source == d.source && memcmp(g_seen[i].mac, d.mac, 6) == 0 &&
        g_seen[i].pan == seenPan(d)) return true;
  return false;
}
static void seenAdd(const Detection& d) {
  if (g_seenCount >= MAX_SEEN) return;
  g_seen[g_seenCount].source = d.source;
  g_seen[g_seenCount].pan = seenPan(d);
  memcpy(g_seen[g_seenCount].mac, d.mac, 6);
  g_seenCount++;
}

static void countBands(int& n24, int& n5, int& nble, int& nprb, int& n154) {
  n24 = n5 = nble = nprb = n154 = 0;
  for (int i = 0; i < g_detCount; i++) {
    switch (g_dets[i].source) {
      case (uint8_t)Source::BleScan:   nble++; break;
      case (uint8_t)Source::WifiProbe: nprb++; break;
      case (uint8_t)Source::Ieee802154: n154++; break;
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

// One device = one MAC, merged across sources (2.4 / 5G / BLE / PRB). Built from the
// sorted detections; ordered tier-first then strongest RSSI. Shared by the on-screen
// list and the phone DETS stream.
struct DevGroup {
  uint8_t  mac[6];
  int      tier;      // max tier across members
  int      bestRssi;  // strongest member RSSI
  int      rep;       // g_dets index of the strongest member (colour/source hint)
  int      nameIdx;   // g_dets index of the first member with a name, else -1
  uint32_t ie;        // first non-zero IE fingerprint, else 0
  char     tag[12];   // combined distinct source tag, e.g. "2.4+PRB"
  char     srcs[48];  // "tag:rssi,tag:rssi" list for the phone stream
};
static int      g_sortIdx[MAX_DET];   // detections sorted tier-first then RSSI (see buildGroups)
static DevGroup g_groups[MAX_DET];
static int      g_groupCount = 0;

static int buildGroups() {
  buildSorted(g_sortIdx);
  int n = 0;
  for (int r = 0; r < g_detCount; r++) {
    int i = g_sortIdx[r];
    const Detection& d = g_dets[i];
    int gi = -1;
    for (int k = 0; k < n; k++)
      if (memcmp(g_groups[k].mac, d.mac, 6) == 0) { gi = k; break; }
    if (gi < 0) {
      gi = n++;
      DevGroup& g = g_groups[gi];
      memcpy(g.mac, d.mac, 6);
      g.tier = (int)g_score[i].tier; g.bestRssi = d.rssi; g.rep = i;
      g.nameIdx = -1; g.ie = 0;
    }
    DevGroup& g = g_groups[gi];
    if ((int)g_score[i].tier > g.tier) g.tier = (int)g_score[i].tier;
    if (d.rssi > g.bestRssi) { g.bestRssi = d.rssi; g.rep = i; }
    if (g.nameIdx < 0 && d.name[0]) g.nameIdx = i;
    if (!g.ie && d.ie_hash) g.ie = d.ie_hash;
  }
  std::sort(g_groups, g_groups + n, [](const DevGroup& a, const DevGroup& b) {
    if (a.tier != b.tier) return a.tier > b.tier;
    return a.bestRssi > b.bestRssi;
  });
  // Per-group source strings, built once per cycle: distinct tags only (exact compare),
  // strongest first. tag = "2.4+PRB" for the screen, srcs = "2.4:-41,PRB:-55" for the phone.
  for (int k = 0; k < n; k++) {
    DevGroup& g = g_groups[k];
    g.tag[0] = 0; g.srcs[0] = 0;
    const char* seen[4]; int ns = 0;
    size_t tl = 0, sl = 0;
    for (int r = 0; r < g_detCount && ns < 4; r++) {
      const Detection& d = g_dets[g_sortIdx[r]];
      if (memcmp(d.mac, g.mac, 6) != 0) continue;
      const char* t = srcTag(d);
      bool dup = false;
      for (int s = 0; s < ns; s++) if (strcmp(seen[s], t) == 0) { dup = true; break; }
      if (dup) continue;
      seen[ns++] = t;
      int w = snprintf(g.tag + tl, sizeof(g.tag) - tl, tl ? "+%s" : "%s", t);
      if (w > 0 && (size_t)w < sizeof(g.tag) - tl) tl += w; else g.tag[tl] = 0;
      w = snprintf(g.srcs + sl, sizeof(g.srcs) - sl, sl ? ",%s:%d" : "%s:%d", t, d.rssi);
      if (w > 0 && (size_t)w < sizeof(g.srcs) - sl) sl += w; else g.srcs[sl] = 0;
    }
  }
  g_groupCount = n;
  return n;
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
  const size_t emptyMax = strlen(kLogHeader) + 2;  // legacy .csv: header + CRLF, nothing else
  File dir = SD.open("/logs");
  if (!dir) return;
  char victims[16][48];
  int nv = 0;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    // .jsonl has no header, so an empty one is 0 bytes; .csv is header-only.
    String en = e.name();
    bool jl = en.endsWith(".jsonl");
    if (!e.isDirectory() && e.size() <= (jl ? 0 : emptyMax) && nv < 16) {
      String nm = en;                             // may be a full path or a bare basename
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
// before the phone has synced wall-clock time; renamed to /logs/YYYYMMDD-HHMMSS.jsonl once
// time is known (renameSessionOnSync). The file itself is created lazily on the first
// logged detection (ensureLogFile), so idle boots leave no empty file behind. Rows still
// carry absolute epoch after sync.
static void openSession() {
  SD.mkdir("/logs");
  if (SD.exists("/scanlog.csv")) SD.remove("/scanlog.csv");  // retire the legacy single file (CSV era)
  sweepEmptySessions();
  Preferences p; p.begin("cydscan", false);
  uint32_t boot = p.getULong("bootcnt", 0) + 1;
  p.putULong("bootcnt", boot);
  p.end();
  snprintf(g_logPath, sizeof(g_logPath), "/logs/sess-%05lu.jsonl", (unsigned long)boot);
  g_logNamed   = false;
  g_logStarted = false;
  Serial.printf("[CYD] session log (created on first detection): %s\n", g_logPath);
}

// Create the (empty) current session file the first time a row needs writing. NDJSON has
// no header. No-op once created. Returns false if SD is unavailable or can't be opened.
static bool ensureLogFile() {
  if (g_logStarted) return true;
  if (!g_sdOk) return false;
  File f = SD.open(g_logPath, FILE_WRITE);
  if (!f) return false;
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
  snprintf(nn, sizeof(nn), "/logs/%04d%02d%02d-%02d%02d%02d.jsonl",
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
  openSession();       // create this boot's /logs/sess-NNNNN.jsonl (renamed on time sync)
  webshare::setLogPath(g_logPath);
  Serial.println("[CYD] SD ready");
}

// Append to an NDJSON line buffer. snprintf returns the would-be length, so a plain
// "n += snprintf(ln+n, sizeof-n, ...)" chain underflows once n passes the buffer; instead
// any truncation parks n at cap+1 (sticky), which the caller treats as "drop this line".
static void lnAppend(char* ln, int& n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void lnAppend(char* ln, int& n, const char* fmt, ...) {
  if (n > (int)LOG_LINE) return;  // already overflowed
  va_list ap; va_start(ap, fmt);
  int w = vsnprintf(ln + n, LOG_LINE - n, fmt, ap);
  va_end(ap);
  if (w < 0 || n + w >= (int)LOG_LINE) n = (int)LOG_LINE + 1;
  else n += w;
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
    char safe[97];  // escaped name (32 B name, worst case \uXXXX per byte truncates safely)
    jsonEscape(safe, sizeof(safe), d.name);
    const sigdb::ScoreResult& sc = g_score[i];
    char sig[49];
    jsonEscape(sig, sizeof(sig), sigdb::labelFor(sc));
    char panStr[5]; panStr[0] = 0;  // hex PAN, 802.15.4 only
    if (d.source == (uint8_t)Source::Ieee802154 && d.panId) snprintf(panStr, sizeof(panStr), "%04X", d.panId);
    char uuidStr[33]; uuidStr[0] = 0;
    for (int k = 0; k < 16; k++)
      if (d.svc[k]) { for (int j = 0; j < 16; j++) sprintf(uuidStr + j * 2, "%02X", d.svc[j]); break; }
    // One NDJSON object per line; optional keys are omitted when blank. Worst case (every
    // optional key + a fully escaped name): ~60 B fixed/numeric + 96 name + 48 sig + 32 uuid
    // + key text ~ 440-480 B, inside the 512 B buffer. lnAppend() is overflow-proof anyway.
    char ln[LOG_LINE];
    int n2 = 0;
    lnAppend(ln, n2, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epoch, (unsigned long)ms);
    if (gps && isfinite(phone::lat()) && isfinite(phone::lon()))
      lnAppend(ln, n2, ",\"lat\":%.6f,\"lon\":%.6f", phone::lat(), phone::lon());
    lnAppend(ln, n2,
             ",\"src\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"ch\":%d",
             srcTag(d), d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
             d.rssi, d.channel);
    if (d.ie_hash)   lnAppend(ln, n2, ",\"ie\":\"%08lX\"", (unsigned long)d.ie_hash);
    if (d.companyId) lnAppend(ln, n2, ",\"cid\":\"%04X\"", d.companyId);
    if (uuidStr[0])  lnAppend(ln, n2, ",\"uuid\":\"%s\"", uuidStr);
    if (panStr[0])   lnAppend(ln, n2, ",\"pan\":\"%s\"", panStr);
    if (safe[0])     lnAppend(ln, n2, ",\"name\":\"%s\"", safe);
    lnAppend(ln, n2, ",\"score\":%d,\"tier\":\"%s\"", sc.score, sigdb::tierName(sc.tier));
    if (sig[0])      lnAppend(ln, n2, ",\"sig\":\"%s\"", sig);
    if (n2 > (int)LOG_LINE - 2) continue;  // can't close the object in-buffer: drop the row, never write truncated JSON
    ln[n2++] = '}'; ln[n2++] = '\n';
    f.write((const uint8_t*)ln, n2);
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
static bool exploreHolds(const char* path);  // true if the Scan Viewer has this file open
static void deleteSession(const char* path) {
  if (strncmp(path, "/logs/", 6) != 0 || strstr(path, "..")) {
    Serial.printf("[CYD] delete rejected (bad path) %s\n", path);
  } else if (strcmp(path, g_logPath) == 0) {
    Serial.println("[CYD] delete refused: that's the current session");
  } else if (exploreHolds(path)) {
    Serial.println("[CYD] delete refused: open in the scan viewer");
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
  cfg.sources  = g_srcMask;
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
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  char s[256];
  snprintf(s, sizeof(s),
           "link=%d;w24=%d;w5=%d;ble=%d;prb=%d;z=%d;uniq=%d;time=%d;gps=%d;dl=%d;"
           "susp=%d;lk=%d;conf=%d;db=%d;scan=%d;bri=%d;src=%d",
           g_linkOk ? 1 : 0, n24, n5, nble, nprb, n154, g_seenCount,
           phone::hasTime() ? 1 : 0, phone::hasGps() ? 1 : 0,
           webshare::active() ? 1 : 0, susp, lk, conf, sigdb::loaded() ? 1 : 0,
           g_screen == SCR_SCAN ? 1 : 0, g_brightness, (int)g_srcMask);
  phone::setStatus(String(s));
}

// Stream the live detection list to the phone so its app mirrors the CYD screen.
// DETS stream v2 = "seq-tagged atomic snapshot": a "D:<seq>:<groups>" header, then one
// row per device (one MAC, merged across sources), top-of-list first:
//   <seq>\t<tier>\t<mac>\t<bestRssi>\t<ie>\t<name>\t<tag:rssi,tag:rssi,...>
// The app drops rows whose seq != the current header's and swaps the list in only when
// the snapshot is complete. Fallbacks considered and held in reserve if this proves
// lossy: (a) a length-prefixed blob like the log download, (b) a polled READ
// characteristic. Capped so the burst stays small on the BLE link.
static constexpr int DETS_STREAM_MAX = 12;
static void pushDetections() {
  if (!phone::connected()) return;
  static uint16_t g_detsSeq = 0;
  int n = min(g_groupCount, DETS_STREAM_MAX);
  g_detsSeq++;
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "D:%u:%d", (unsigned)g_detsSeq, n);
  phone::detsNotify(String(hdr));
  for (int r = 0; r < n; r++) {
    const DevGroup& g = g_groups[r];
    char name[24];
    sanitizeField(name, sizeof(name), g.nameIdx >= 0 ? g_dets[g.nameIdx].name : "<hidden>");
    char row[192];
    int len = snprintf(row, sizeof(row), "%u\t%d\t%02X:%02X:%02X:%02X:%02X:%02X\t%d\t%08lX\t%s\t",
                       (unsigned)g_detsSeq, g.tier,
                       g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5],
                       g.bestRssi, (unsigned long)g.ie, name);
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    snprintf(row + len, sizeof(row) - len, "%s", g.srcs);  // list pre-built (de-duped) in buildGroups
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

// --- shared scrolling list: geometry, icons, touch (live scan, scan viewer, detail,
// session picker all use these) ---
static int visibleRows()   { return (tft.height() - LIST_Y0) / LIST_ROW_H; }
static int scrollCx()      { return tft.width() - SCROLL_CX_INSET; }
static int scrollUpCy()    { return LIST_Y0 + SCROLL_R + 2; }
static int scrollDownCy()  { return tft.height() - SCROLL_R - 4; }

static void drawScrollIcon(int cx, int cy, bool up, bool enabled) {
  uint16_t c = enabled ? TFT_WHITE : TFT_DARKGREY;
  tft.drawCircle(cx, cy, SCROLL_R, c);
  int dy = up ? -1 : 1;  // chevron: apex toward the scroll direction
  for (int t = 0; t < 2; t++) {  // 2 px thick
    tft.drawLine(cx - 6, cy - dy * 3 + t, cx, cy + dy * 3 + t, c);
    tft.drawLine(cx + 6, cy - dy * 3 + t, cx, cy + dy * 3 + t, c);
  }
}

// Up/down arrows (dimmed at the limits) for a list of `count` rows, `vis` visible.
static void drawScrollIcons(int offset, int count, int vis) {
  int maxOff = max(0, count - vis);
  drawScrollIcon(scrollCx(), scrollUpCy(),   true,  offset > 0);
  drawScrollIcon(scrollCx(), scrollDownCy(), false, offset < maxOff);
}

static bool inCircle(int x, int y, int cx, int cy) {
  int dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= SCROLL_R * SCROLL_R;
}

// Shared row painter: "<flag><tag:tagW> <name:13> <rssi:4>" at row y, plus an optional
// trailing field (the scan viewer's timecode) ellipsised to fit left of the scroll-icon
// gutter. The live scan uses tagW=9 and trailing=""; the viewer uses a 3-wide tag.
static void drawDetRow(int y, uint16_t color, const char* tag, const char* name, int rssi,
                       const char* trailing, bool flag = false, int tagW = 9) {
  char buf[56];
  int n = snprintf(buf, sizeof(buf), "%c%-*.*s %-13.13s %4d", flag ? '!' : ' ', tagW, tagW, tag,
                   name, rssi);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(buf, 4, y, 1);
  if (trailing && trailing[0]) {
    int x0   = 4 + (n + 1) * 6;
    int room = (ICON_GUTTER_X - x0) / 6;  // chars that fit before the icons
    char t[16];
    int  len = strlen(trailing);
    if (len <= room || room < 3) snprintf(t, sizeof(t), "%.*s", room < 15 ? room : 15, trailing);
    else snprintf(t, sizeof(t), "..%s", trailing + len - (room - 2));  // ellipsis + tail
    tft.drawString(t, x0, y, 1);
  }
}

// Row colour: threat tier first, else by source/band.
static uint16_t tierColor(int tier, uint8_t source, int channel) {
  switch ((sigdb::Tier)tier) {
    case sigdb::Tier::Confirmed: return TFT_RED;
    case sigdb::Tier::Likely:    return TFT_ORANGE;
    case sigdb::Tier::Suspect:   return TFT_YELLOW;
    default: return source == (uint8_t)Source::BleScan ? TFT_MAGENTA
                  : source == (uint8_t)Source::Ieee802154 ? TFT_GREEN  // distinct from 5G cyan
                  : channel > 14 ? TFT_CYAN : TFT_WHITE;
  }
}

// Shared touch handling for any scrolling list: tap an arrow to step one row, drag the
// list to scroll, tap a row (touch-down + release without moving). `redraw` is called
// when the offset changes. Returns the tapped ABSOLUTE row index on a tap, else -1.
// Rows are `rowH` tall starting at y0; `vis` rows are visible. Arrows/drag live strictly
// below the top bar. Armed only after the finger has been up once, so the touch that
// opened the screen can't leak in (call listTouchReset() on screen entry). No-op until
// touch is calibrated.
static bool g_ltArmed = false;
static void listTouchReset() { g_ltArmed = false; }
static int listTouch(int count, int* offset, int vis, int y0, int rowH, void (*redraw)()) {
  static bool prev = false, dragging = false, moved = false;
  static int  startY = 0, startOffset = 0;
  if (!g_touchOk) { prev = false; dragging = false; return -1; }
  bool now = g_touch.touched();
  if (!g_ltArmed) {
    if (!now) g_ltArmed = true;
    prev = now; dragging = false;
    return -1;
  }
  int before = *offset, tap = -1;
  int maxOff = max(0, count - vis);
  int16_t sx, sy, z;
  if (now && g_touch.getScreen(tft, sx, sy, z)) {
    if (!prev) {  // touch-down edge
      dragging = false; moved = false;
      if (sy > STOP_Y + STOP_H) {
        if      (inCircle(sx, sy, scrollCx(), scrollUpCy()))   (*offset)--;
        else if (inCircle(sx, sy, scrollCx(), scrollDownCy())) (*offset)++;
        else if (sy >= y0) { dragging = true; startY = sy; startOffset = *offset; }
      }
    } else if (dragging) {
      int d = sy - startY;
      if (d > 8 || d < -8) moved = true;
      *offset = startOffset - d / rowH;
    }
    *offset = constrain(*offset, 0, maxOff);
  }
  if (!now && prev && dragging && !moved) {  // released without dragging: a row tap
    int r = (startY - y0) / rowH;
    if (r >= 0 && r < vis && startOffset + r < count) tap = startOffset + r;
  }
  if (!now) dragging = false;
  prev = now;
  if (*offset != before) redraw();
  return tap;
}

// Repaint the device list region (below the status/count lines) + scroll icons. With
// clear=true the region is wiped first — used by touch scrolling to avoid a full-screen
// flicker. Uses the groups built for this cycle (buildGroups). One row per MAC, tier
// first then strongest RSSI.
static void drawScanList(bool clear) {
  int W = tft.width();
  if (clear) tft.fillRect(0, LIST_Y0, W, tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  int maxOff = max(0, g_groupCount - vis);
  g_scrollOffset = constrain(g_scrollOffset, 0, maxOff);  // self-corrects when the list shrinks
  int rows = min(g_groupCount - g_scrollOffset, vis);
  for (int r = 0; r < rows; r++) {
    const DevGroup& g = g_groups[g_scrollOffset + r];
    const Detection& d = g_dets[g.rep];
    drawDetRow(LIST_Y0 + r * LIST_ROW_H, tierColor(g.tier, d.source, d.channel), g.tag,
               g.nameIdx >= 0 ? g_dets[g.nameIdx].name : "<hidden>", g.bestRssi, "",
               g.tier != (int)sigdb::Tier::None);
  }
  drawScrollIcons(g_scrollOffset, g_groupCount, vis);
}

// Touch scrolling for the live scan list. Call frequently while on SCR_SCAN.
static void handleScanTouch() {
  listTouch(g_groupCount, &g_scrollOffset, visibleRows(), LIST_Y0, LIST_ROW_H,
            []() { drawScanList(true); });
}

// Full-width top-bar button (hit-tested by topBarTapped()).
// Segment `seg` of `nseg` equal parts of the bar (hit-tested by topBarSegTapped(nseg)).
static void drawTopBarSeg(const char* label, int seg, int nseg, uint16_t fill, uint16_t edge) {
  int total = tft.width() - 2 * STOP_X;
  int x0 = STOP_X + seg * total / nseg, x1 = STOP_X + (seg + 1) * total / nseg;
  int w = x1 - x0 - (nseg > 1 ? 2 : 0);
  tft.fillRoundRect(x0, STOP_Y, w, STOP_H, 4, fill);
  tft.drawRoundRect(x0, STOP_Y, w, STOP_H, 4, edge);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawString(label, x0 + w / 2, STOP_Y + STOP_H / 2, 1);
}
static void drawTopBar(const char* label, uint16_t fill, uint16_t edge) {
  drawTopBarSeg(label, 0, 1, fill, edge);
}

static void render() {
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();

  // Full-width stop button across the top: tap to end the scan and return to the
  // menu (a long BOOT hold does the same).
  drawTopBar("STOP", TFT_MAROON, TFT_RED);

  tft.setTextDatum(TL_DATUM);
  char buf[48];
  // Link status lives in the status-bar dot; this line covers SD + DB.
  tft.setTextColor(g_sdOk ? TFT_WHITE : TFT_RED, TFT_BLACK);
  snprintf(buf, sizeof(buf), "SD:%s  db:%s",
           g_sdOk ? "on" : "off", sigdb::loaded() ? "on" : "fb");
  tft.drawString(buf, 4, 46, 1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "2.4:%d 5G:%d BLE:%d PRB:%d Z:%d U:%d", n24, n5, nble, nprb, n154, g_seenCount);
  tft.drawString(buf, 4, 58, 1);

  uint16_t tcol = conf ? TFT_RED : lk ? TFT_ORANGE : susp ? TFT_YELLOW : TFT_DARKGREY;
  tft.setTextColor(tcol, TFT_BLACK);
  snprintf(buf, sizeof(buf), "threats  S:%d  L:%d  C:%d", susp, lk, conf);
  tft.drawString(buf, 4, 70, 1);

  drawScanList(false);
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

// Shared list-menu screen: status bar, centered title, and a highlighted row list on the
// ROW_Y0/ROW_STEP/ROW_H geometry (the same geometry tappedRow() hit-tests). Every list
// screen (home, settings, scan menu) draws through this. Hint text is drawn by the caller.
static void drawListMenu(const char* title, const char* const* items, int n, int sel) {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  int W = tft.width();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(title, W / 2, 32, 4);
  // Button text is centered in the row; font-4 (26px) in the 34px row leaves 4px
  // of padding above and below.
  for (int i = 0; i < n; i++) {
    bool s = (i == sel);
    int  y = ROW_Y0 + i * ROW_STEP;
    if (s) tft.fillRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_NAVY);
    else   tft.drawRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(s ? TFT_WHITE : TFT_LIGHTGREY, s ? TFT_NAVY : TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(items[i], W / 2, y - 5 + ROW_H / 2, 4);
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
}

// ---- Shared FILE-SELECTION list style (use this for any "pick a file" screen) ----
// Same button geometry as drawListMenu (ROW_Y0/ROW_STEP/ROW_H, so tappedRow()/listTouch()
// hit-testing is identical) but a smaller font (2) and up to two text lines per row inside
// the FIXED-height button (one line is vertically centred). The size (size<0 = none, e.g. a
// Back row) is right-aligned and dim on the row's last line. A name too long for two lines
// is left-truncated: ".." + the tail, so the distinguishing timestamp/sequence survives.
// Draws the window items[off .. off+FILE_LIST_VIS); sel/off are absolute indices.
struct FileItem { const char* name; int32_t size; };
static constexpr int FILE_LIST_VIS = 5;

static int fileTextW(const char* s, int n) {  // pixel width of the first n chars (font 2)
  char t[32];
  n = min(n, (int)sizeof(t) - 1);
  memcpy(t, s, n);
  t[n] = 0;
  return tft.textWidth(t, 2);
}
static int filePrefixFit(const char* s, int maxW) {  // most leading chars that fit in maxW px
  int len = strlen(s), k = 0;
  while (k < len && fileTextW(s, k + 1) <= maxW) k++;
  return k;
}

static void drawFileList(const char* title, const FileItem* items, int n, int sel, int off) {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  int W = tft.width();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(title, W / 2, 32, 4);
  const int xL = 12, xR = W - 12, fullW = xR - xL, gap = 8, lineH = 16;
  int rows = min(n - off, FILE_LIST_VIS);
  for (int i = 0; i < rows; i++) {
    const FileItem& it = items[off + i];
    bool s = (off + i == sel);
    int  rt = ROW_Y0 + i * ROW_STEP - 5;  // button top (height ROW_H, fixed)
    if (s) tft.fillRoundRect(6, rt, W - 12, ROW_H, 6, TFT_NAVY);
    else   tft.drawRoundRect(6, rt, W - 12, ROW_H, 6, TFT_DARKGREY);
    uint16_t bg = s ? TFT_NAVY : TFT_BLACK;

    char sz[12] = "";
    if (it.size >= 0) {
      if (it.size < 1024)         snprintf(sz, sizeof(sz), "%u B", (unsigned)it.size);
      else if (it.size < 1048576) snprintf(sz, sizeof(sz), "%.1f KB", it.size / 1024.0f);
      else                        snprintf(sz, sizeof(sz), "%.1f MB", it.size / 1048576.0f);
    }
    int lastW = fullW - (sz[0] ? tft.textWidth(sz, 2) + gap : 0);  // room beside the size

    // Fit the name: 1 line if it fits beside the size, else 2 lines (line 1 full width,
    // line 2 beside the size), else ".." + tail until the two lines hold it.
    char name[24];
    snprintf(name, sizeof(name), "%.23s", it.name);
    const char* w = name;
    char trunc[26];
    bool one = tft.textWidth(w, 2) <= lastW;
    int  p = 0;
    if (!one) {
      for (int skip = 0;; skip++) {
        if (skip == 0) w = name;
        else { snprintf(trunc, sizeof(trunc), "..%s", name + skip); w = trunc; }
        p = filePrefixFit(w, fullW);
        if (fileTextW(w + p, strlen(w + p)) <= lastW || !w[1]) break;
      }
    }
    tft.setTextColor(s ? TFT_WHITE : TFT_LIGHTGREY, bg);
    tft.setTextDatum(TL_DATUM);
    int lastY;
    if (one) {
      lastY = rt + (ROW_H - lineH) / 2;
      tft.drawString(w, xL, lastY, 2);
    } else {
      char l1[26];
      snprintf(l1, sizeof(l1), "%.*s", p, w);
      tft.drawString(l1, xL, rt + 1, 2);
      lastY = rt + 1 + lineH;
      tft.drawString(w + p, xL, lastY, 2);
    }
    if (sz[0]) {
      tft.setTextDatum(TR_DATUM);
      tft.setTextColor(s ? TFT_LIGHTGREY : TFT_DARKGREY, bg);
      tft.drawString(sz, xR, lastY, 2);
    }
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
}

// Home menu. Reuses the status bar (time / GPS / connection / link dot) up top.
static void drawMenu() {
  drawListMenu("HOME", kMenuItems, MENU_N, g_menuSel);
  tft.drawString(g_touchOk ? "Tap an item, or BOOT: tap=next hold=select"
                           : "BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// Settings screen: Calibrate Touch / Brightness / Back.
static void drawSettings() {
  char bright[24];
  snprintf(bright, sizeof(bright), "Brightness: %d%%", g_brightness);
  const char* items[SET_N] = { "Calibrate Touch", bright, "Back" };
  drawListMenu("SETTINGS", items, SET_N, g_setSel);
  tft.drawString(g_touchOk ? "Touch OK - tap an item" : "Touch not calibrated yet",
                 10, tft.height() - 18, 1);
}

// Scan sub-menu: Start Scan / Explore Scan / Scan Settings / Back.
static void drawScanMenu() {
  drawListMenu("SCAN", kScanItems, SCAN_N, g_scanSel);
  tft.drawString(g_touchOk ? "Tap an item, or BOOT: tap=next hold=select"
                           : "BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// Scan Settings: per-source enable toggles (labels rebuilt each draw to show state).
static void drawScanSettings() {
  char b[20], w24[24], w5[24], z[24];
  snprintf(b,   sizeof(b),   "BLE: %s",       (g_srcMask & MASK_BLE)    ? "On" : "Off");
  snprintf(w24, sizeof(w24), "Wi-Fi 2.4: %s", (g_srcMask & MASK_WIFI24) ? "On" : "Off");
  snprintf(w5,  sizeof(w5),  "Wi-Fi 5G: %s",  (g_srcMask & MASK_WIFI5)  ? "On" : "Off");
  snprintf(z,   sizeof(z),   "802.15.4: %s",  (g_srcMask & MASK_154)    ? "On" : "Off");
  const char* items[SCANSET_N] = { b, w24, w5, z, "Back" };
  drawListMenu("SCAN SETTINGS", items, SCANSET_N, g_scanSetSel);
  tft.drawString(g_touchOk ? "Tap an item, or BOOT: tap=next hold=select"
                           : "BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// ---------------------------------------------------------------- Explore Scan
// On-device session browser: SCR_PICKLOG (choose a /logs/ session, newest first) ->
// SCR_SCANVIEWER (rows oldest -> newest, shared row painter + scroll) -> SCR_DETAIL
// (label/value list mirroring the web app's detail modal). The session file is STREAMED:
// one index pass stores a 12-byte record per data row; only the visible rows are re-read
// and parsed per frame. The index + session list live in one heap block that exists only
// while in Explore (live scan and Explore are mutually exclusive) — see exploreFree().
static constexpr int MAX_LOG_ROWS = 512;  // 6 KB index; later rows are not indexed (truncated)
static constexpr int MAX_SESS     = 48;
static constexpr int PICK_VIS     = FILE_LIST_VIS;  // picker rows on screen (ROW_STEP geometry)
static constexpr int DET_MAX_LINES = 28;

struct LogIdx { uint32_t offset; uint32_t epoch; int16_t rssi; uint8_t srcType; uint8_t tier; };  // 12 B
struct SessEnt { char name[20]; uint32_t wr; uint32_t size; bool jl; };  // basename w/o ext; FS write time; bytes; .jsonl (else legacy .csv)
struct ExploreMem { LogIdx idx[MAX_LOG_ROWS]; SessEnt sess[MAX_SESS]; uint16_t view[MAX_LOG_ROWS]; };
static ExploreMem* g_ex = nullptr;
static int   g_sessN = 0, g_pickSel = 0, g_pickOff = 0;
static int   g_idxN = 0;   // rows indexed from the file (M)
static int   g_viewN = 0;  // rows shown after filter + sort (N); g_ex->view[] indexes into idx[]
// Viewer filter / sort (CYD subset), applied to the in-memory index only. Reset per session.
static uint8_t g_fType = 0x1F;  // bit per srcType: 2.4, 5G, BLE, PRB, 154
static bool    g_fThreat = false;
static uint8_t g_fDist = 0;     // 0 All, 1 Far, 2 Mid, 3 Near
static uint8_t g_sortMode = 0;
static const int8_t kDistRssi[4] = { -100, -80, -60, -40 };  // keep rssi >= threshold
static bool  g_idxTrunc = false;
static File  g_viewFile;
static char  g_viewName[20];
static bool  g_viewJl = true;  // open session is .jsonl (else legacy .csv)
static int   g_viewOffset = 0, g_detOff = 0, g_detN = 0;

// One parsed CSV data row (schema in kLogHeader).
struct LogRow {
  uint32_t epoch, ms;
  int      rssi, channel, score, tier;
  char     lat[16], lon[16], src[4], mac[18], ie[9], cid[5], uuid[33], pan[5], name[33], sig[24];
};

struct DetLine { char label[15]; char val[25]; uint16_t col; };
static DetLine g_detL[DET_MAX_LINES];

static void cpyField(char* dst, size_t cap, const char* s) {
  strncpy(dst, s, cap - 1);
  dst[cap - 1] = 0;
}

// Read one line (CR/LF stripped, over-long lines truncated to cap-1). *pos = line start.
static bool readLogLine(File& f, char* buf, size_t cap, uint32_t* pos = nullptr) {
  if (pos) *pos = f.position();
  size_t n = 0;
  bool any = false;
  while (f.available()) {
    int c = f.read();
    any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (n < cap - 1) buf[n++] = (char)c;
  }
  buf[n] = 0;
  return any;
}

// Parse one NDJSON line (ArduinoJson, transient stack doc; strings copied out immediately).
// Omitted keys read as empty/zero. lat/lon are re-formatted to the fixed text LogRow uses.
static bool parseJsonLine(char* line, LogRow& r) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return false;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  if (o.isNull() || !o.containsKey("mac")) return false;
  r.epoch   = o["epoch"] | 0UL;
  r.ms      = o["ms"] | 0UL;
  r.lat[0] = r.lon[0] = 0;
  if (o.containsKey("lat") && o.containsKey("lon")) {
    snprintf(r.lat, sizeof(r.lat), "%.6f", o["lat"].as<double>());
    snprintf(r.lon, sizeof(r.lon), "%.6f", o["lon"].as<double>());
  }
  cpyField(r.src,  sizeof(r.src),  o["src"]  | "");
  cpyField(r.mac,  sizeof(r.mac),  o["mac"]  | "");
  r.rssi    = o["rssi"] | 0;
  r.channel = o["ch"] | 0;
  cpyField(r.ie,   sizeof(r.ie),   o["ie"]   | "");
  cpyField(r.cid,  sizeof(r.cid),  o["cid"]  | "");
  cpyField(r.uuid, sizeof(r.uuid), o["uuid"] | "");
  cpyField(r.pan,  sizeof(r.pan),  o["pan"]  | "");
  cpyField(r.name, sizeof(r.name), o["name"] | "");
  r.score   = o["score"] | 0;
  const char* t = o["tier"] | "";
  r.tier    = t[0] == 's' ? 1 : t[0] == 'l' ? 2 : t[0] == 'c' ? 3 : 0;
  cpyField(r.sig,  sizeof(r.sig),  o["sig"]  | "");
  return true;
}

// Split a legacy CSV data line in place (the header line fails: it doesn't start with a digit).
static bool parseCsvLine(char* line, LogRow& r) {
  if (line[0] < '0' || line[0] > '9') return false;
  char* f[16];
  int n = 0;
  f[n++] = line;
  for (char* p = line; *p; ++p)
    if (*p == ',') { *p = 0; if (n < 16) f[n++] = p + 1; }
  if (n < 14) return false;
  // Schema v5 inserts `pan` after `uuid` (16 columns); v4 rows have 15. Normalize to v5.
  if (n < 16) {
    static char empty[] = "";
    for (int i = n; i > 11; i--) f[i] = f[i - 1];
    f[11] = empty;
    n++;
  }
  r.epoch   = strtoul(f[0], nullptr, 10);
  r.ms      = strtoul(f[1], nullptr, 10);
  cpyField(r.lat, sizeof(r.lat), f[2]);
  cpyField(r.lon, sizeof(r.lon), f[3]);
  cpyField(r.src, sizeof(r.src), f[4]);
  cpyField(r.mac, sizeof(r.mac), f[5]);
  r.rssi    = atoi(f[6]);
  r.channel = atoi(f[7]);
  cpyField(r.ie,   sizeof(r.ie),   f[8]);
  cpyField(r.cid,  sizeof(r.cid),  f[9]);
  cpyField(r.uuid, sizeof(r.uuid), f[10]);
  cpyField(r.pan,  sizeof(r.pan),  f[11]);
  cpyField(r.name, sizeof(r.name), f[12]);
  r.score   = atoi(f[13]);
  char t = f[14][0];
  r.tier    = t == 's' ? 1 : t == 'l' ? 2 : t == 'c' ? 3 : 0;
  cpyField(r.sig, sizeof(r.sig), n > 15 ? f[15] : "");
  return true;
}

// Sniff the format: a line starting with '{' is NDJSON, else old CSV.
static bool parseLogLine(char* line, LogRow& r) {
  return line[0] == '{' ? parseJsonLine(line, r) : parseCsvLine(line, r);
}

// 0=2.4, 1=5G, 2=BLE, 3=PRB, 4=154 (stored in LogIdx).
static uint8_t srcTypeOf(const char* tag) {
  if (!strcmp(tag, "BLE")) return 2;
  if (!strcmp(tag, "PRB")) return 3;
  if (!strcmp(tag, "154")) return 4;
  if (!strcmp(tag, "5G"))  return 1;
  return 0;
}
static uint8_t linkSourceOf(uint8_t st) {  // -> link_protocol::Source for tierColor()
  return st == 2 ? (uint8_t)Source::BleScan : st == 3 ? (uint8_t)Source::WifiProbe
       : st == 4 ? (uint8_t)Source::Ieee802154 : (uint8_t)Source::WifiScan;
}

// epoch (already local) -> HH:MM:SS, or "+Ns" since boot when the phone hadn't synced.
static void fmtTimecode(uint32_t epoch, uint32_t ms, char* out, size_t cap) {
  if (epoch > 0) {
    time_t t = (time_t)epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(out, cap, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(out, cap, "+%lus", (unsigned long)(ms / 1000));
  }
}

static bool readRowAt(uint32_t offset, char* line, size_t cap, LogRow& r) {
  if (!g_viewFile || !g_viewFile.seek(offset)) return false;
  readLogLine(g_viewFile, line, cap);
  return parseLogLine(line, r);
}

static bool exploreHolds(const char* path) {
  if (!g_viewFile) return false;
  char open[48];
  snprintf(open, sizeof(open), "/logs/%s.%s", g_viewName, g_viewJl ? "jsonl" : "csv");
  return strcmp(path, open) == 0;
}

static void exploreFree() {
  if (g_viewFile) g_viewFile.close();
  if (g_ex) {
    free(g_ex);
    g_ex = nullptr;
    Serial.printf("[CYD] explore freed (heap free %u)\n", (unsigned)ESP.getFreeHeap());
  }
}

// ---- session picker ----
static void sessEnumerate() {
  g_sessN = 0;
  File dir = SD.open("/logs");
  if (!dir) return;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    if (!e.isDirectory() && g_sessN < MAX_SESS) {
      const char* nm = e.name();
      const char* sl = strrchr(nm, '/');
      const char* base = sl ? sl + 1 : nm;
      size_t len = strlen(base);
      bool jl = len > 6 && !strcasecmp(base + len - 6, ".jsonl");
      size_t el = jl ? 6 : 4;  // extension length
      if (len > el && len - el < sizeof(SessEnt::name) && (jl || !strcasecmp(base + len - 4, ".csv"))) {
        SessEnt& s = g_ex->sess[g_sessN++];
        memcpy(s.name, base, len - el);
        s.name[len - el] = 0;
        s.jl = jl;
        s.wr = (uint32_t)e.getLastWrite();
        s.size = (uint32_t)e.size();
      }
    }
    e.close();
  }
  dir.close();
  std::sort(g_ex->sess, g_ex->sess + g_sessN, [](const SessEnt& a, const SessEnt& b) {
    if (a.wr != b.wr) return a.wr > b.wr;      // newest first
    return strcmp(a.name, b.name) > 0;         // tie-break: name descending
  });
}

static void drawPickLog() {
  FileItem items[MAX_SESS + 1];
  int n = g_sessN + 1;  // sessions + Back
  for (int i = 0; i < g_sessN; i++) items[i] = { g_ex->sess[i].name, (int32_t)g_ex->sess[i].size };
  items[g_sessN] = { "Back", -1 };
  g_pickOff = constrain(g_pickOff, 0, max(0, n - PICK_VIS));
  drawFileList("SESSIONS", items, n, g_pickSel, g_pickOff);
  if (n > PICK_VIS) drawScrollIcons(g_pickOff, n, PICK_VIS);
  tft.drawString("BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);  // short: clears the icons
}

static void exploreEnter() {
  if (!g_ex) g_ex = (ExploreMem*)malloc(sizeof(ExploreMem));
  g_sessN = 0;
  if (g_ex && g_sdOk) sessEnumerate();
  else if (!g_ex) Serial.println("[CYD] explore: out of memory");
  Serial.printf("[CYD] explore: %d sessions (heap free %u)\n", g_sessN, (unsigned)ESP.getFreeHeap());
  g_pickSel = 0; g_pickOff = 0;  // newest session highlighted
  g_screen = SCR_PICKLOG;
  listTouchReset();
  drawPickLog();
}

// ---- scan viewer ----
// Rebuild g_ex->view[] from the index: filter, then sort (modes: 0 oldest, 1 newest,
// 2 type+newest, 3 type+oldest, 4 closest, 5 farthest). Index order == file == time order.
static void applyView() {
  g_viewN = 0;
  for (int i = 0; i < g_idxN; i++) {
    const LogIdx& e = g_ex->idx[i];
    if (!(g_fType & (1 << e.srcType))) continue;
    if (g_fThreat && e.tier == 0) continue;
    if (e.rssi < kDistRssi[g_fDist]) continue;
    g_ex->view[g_viewN++] = (uint16_t)i;
  }
  const LogIdx* ix = g_ex->idx;
  uint16_t* v = g_ex->view;
  switch (g_sortMode) {
    case 1: std::reverse(v, v + g_viewN); break;
    case 2: std::sort(v, v + g_viewN, [ix](uint16_t a, uint16_t b) {
              if (ix[a].srcType != ix[b].srcType) return ix[a].srcType < ix[b].srcType;
              return a > b; }); break;
    case 3: std::sort(v, v + g_viewN, [ix](uint16_t a, uint16_t b) {
              if (ix[a].srcType != ix[b].srcType) return ix[a].srcType < ix[b].srcType;
              return a < b; }); break;
    case 4: std::sort(v, v + g_viewN, [ix](uint16_t a, uint16_t b) {
              if (ix[a].rssi != ix[b].rssi) return ix[a].rssi > ix[b].rssi;
              return a < b; }); break;
    case 5: std::sort(v, v + g_viewN, [ix](uint16_t a, uint16_t b) {
              if (ix[a].rssi != ix[b].rssi) return ix[a].rssi < ix[b].rssi;
              return a < b; }); break;
    default: break;
  }
}
static void viewResetFilter() { g_fType = 0x1F; g_fThreat = false; g_fDist = 0; g_sortMode = 0; }
static void drawViewerList(bool clear) {
  if (clear) tft.fillRect(0, LIST_Y0, tft.width(), tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  g_viewOffset = constrain(g_viewOffset, 0, max(0, g_viewN - vis));
  if (g_viewN == 0) {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(g_idxN ? "No rows match the filter" : "No detections logged", 4, LIST_Y0, 1);
  }
  char line[LOG_LINE];
  int rows = min(g_viewN - g_viewOffset, vis);
  for (int r = 0; r < rows; r++) {
    const LogIdx& e = g_ex->idx[g_ex->view[g_viewOffset + r]];
    LogRow lr;
    if (!readRowAt(e.offset, line, sizeof(line), lr)) continue;
    char tc[12];
    fmtTimecode(lr.epoch, lr.ms, tc, sizeof(tc));
    drawDetRow(LIST_Y0 + r * LIST_ROW_H, tierColor(lr.tier, linkSourceOf(e.srcType), lr.channel),
               lr.src, lr.name[0] ? lr.name : "<hidden>", lr.rssi, tc, lr.tier > 0, 3);
  }
  drawScrollIcons(g_viewOffset, g_viewN, vis);
}

static void drawViewer() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBarSeg("< Back", 0, 3, TFT_NAVY, TFT_CYAN);
  drawTopBarSeg("Filter", 1, 3, TFT_NAVY, TFT_CYAN);
  drawTopBarSeg("Sort",   2, 3, TFT_NAVY, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(g_viewName, 4, 46, 1);
  char b[48];
  snprintf(b, sizeof(b), g_idxTrunc ? "showing %d of %d (first %d)" : "showing %d of %d",
           g_viewN, g_idxN, MAX_LOG_ROWS);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(b, 4, 58, 1);
  drawViewerList(false);
}

// One streaming pass over the file -> compact per-row index (first MAX_LOG_ROWS rows).
static bool buildIndex(const char* name) {
  char path[48];
  snprintf(path, sizeof(path), "/logs/%s.%s", name, g_viewJl ? "jsonl" : "csv");
  if (g_viewFile) g_viewFile.close();
  g_viewFile = SD.open(path, "r");
  if (!g_viewFile) return false;
  g_idxN = 0; g_idxTrunc = false;
  char line[LOG_LINE];
  uint32_t pos;
  while (readLogLine(g_viewFile, line, sizeof(line), &pos)) {
    LogRow r;
    if (!parseLogLine(line, r)) continue;
    if (g_idxN >= MAX_LOG_ROWS) { g_idxTrunc = true; break; }
    LogIdx& e = g_ex->idx[g_idxN++];
    e.offset = pos; e.epoch = r.epoch; e.rssi = (int16_t)r.rssi;
    e.srcType = srcTypeOf(r.src); e.tier = (uint8_t)r.tier;
  }
  return true;
}

static void activatePick(int sel) {
  if (sel >= g_sessN || !g_ex) {  // Back: leave Explore, free the index block
    exploreFree();
    g_screen = SCR_SCANMENU;
    drawScanMenu();
    return;
  }
  cpyField(g_viewName, sizeof(g_viewName), g_ex->sess[sel].name);
  g_viewJl = g_ex->sess[sel].jl;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Indexing...", tft.width() / 2, tft.height() / 2, 4);
  if (!buildIndex(g_viewName)) { drawPickLog(); return; }
  Serial.printf("[CYD] explore: %s indexed %d rows%s (heap free %u)\n", g_viewName, g_idxN,
                g_idxTrunc ? " (truncated)" : "", (unsigned)ESP.getFreeHeap());
  g_viewOffset = 0;
  viewResetFilter();
  applyView();
  g_screen = SCR_SCANVIEWER;
  listTouchReset();
  drawViewer();
}

static void viewerBack() {
  if (g_viewFile) g_viewFile.close();
  g_screen = SCR_PICKLOG;
  listTouchReset();
  drawPickLog();
}

// ---- viewer Sort / Filter menus: drawListMenu windows (5 rows) + shared scroll ----
static int g_sortSel = 0, g_sortOff = 0, g_fltSel = 0, g_fltOff = 0;
static constexpr int SORT_N = 7, FILTER_N = 8;  // incl. the trailing Back row
static const char* const kSortItems[SORT_N] = { "Oldest first", "Newest first", "Type newest",
    "Type oldest", "Closest", "Farthest", "Back" };
static const char* const kDistName[4] = { "All", "Far", "Mid", "Near" };

static void drawScrollMenu(const char* title, const char* const* items, int n, int sel, int off) {
  drawListMenu(title, items + off, min(n - off, FILE_LIST_VIS), sel - off);
  if (n > FILE_LIST_VIS) drawScrollIcons(off, n, FILE_LIST_VIS);
  tft.drawString("BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// Shared nav for those menus: touch tap selects, BOOT tap = next, BOOT hold = select.
// Returns the activated row, else -1.
static int menuNav(int n, int* sel, int* off, BtnEv ev, void (*redraw)()) {
  int t = listTouch(n, off, FILE_LIST_VIS, ROW_Y0 - 5, ROW_STEP, redraw);
  if (t >= 0) { *sel = t; return t; }
  if (ev == BTN_SHORT) {
    *sel = (*sel + 1) % n;
    if (*sel < *off) *off = *sel;
    if (*sel >= *off + FILE_LIST_VIS) *off = *sel - FILE_LIST_VIS + 1;
    redraw();
  } else if (ev == BTN_LONG) return *sel;
  return -1;
}

static void drawSort() { drawScrollMenu("SORT", kSortItems, SORT_N, g_sortSel, g_sortOff); }

static void drawFilter() {
  char l[FILTER_N - 1][24];
  static const char* const kTypes[5] = { "2.4", "5G", "BLE", "PRB", "154" };
  for (int i = 0; i < 5; i++)
    snprintf(l[i], sizeof(l[i]), "%s: %s", kTypes[i], (g_fType & (1 << i)) ? "On" : "Off");
  snprintf(l[5], sizeof(l[5]), "Threats: %s", g_fThreat ? "On" : "Off");
  snprintf(l[6], sizeof(l[6]), "Dist: %s", kDistName[g_fDist]);
  const char* items[FILTER_N] = { l[0], l[1], l[2], l[3], l[4], l[5], l[6], "Back" };
  drawScrollMenu("FILTER", items, FILTER_N, g_fltSel, g_fltOff);
}

static void returnToViewer() {
  g_viewOffset = 0;
  g_screen = SCR_SCANVIEWER;
  listTouchReset();
  drawViewer();
}

static void activateSort(int sel) {
  if (sel < SORT_N - 1) { g_sortMode = (uint8_t)sel; applyView(); }  // Back leaves it unchanged
  returnToViewer();
}

static void activateFilter(int sel) {
  if (sel < 5)       g_fType ^= (1 << sel);
  else if (sel == 5) g_fThreat = !g_fThreat;
  else if (sel == 6) g_fDist = (g_fDist + 1) & 3;
  else { applyView(); returnToViewer(); return; }  // Back: apply + return
  drawFilter();
}

static void viewerOpenSort() {
  g_sortSel = g_sortMode; g_sortOff = max(0, g_sortSel - FILE_LIST_VIS + 1);
  g_screen = SCR_SORT; listTouchReset(); drawSort();
}
static void viewerOpenFilter() {
  g_fltSel = 0; g_fltOff = 0;
  g_screen = SCR_FILTER; listTouchReset(); drawFilter();
}

// ---- detail (mirrors the web app's openDetail(), minus the map) ----
// Append a label/value line; long values wrap onto continuation lines (blank label).
static void addField(const char* label, const char* val, uint16_t col = TFT_WHITE) {
  size_t len = strlen(val), pos = 0;
  bool first = true;
  do {
    if (g_detN >= DET_MAX_LINES) return;
    DetLine& d = g_detL[g_detN++];
    cpyField(d.label, sizeof(d.label), first ? label : "");
    snprintf(d.val, sizeof(d.val), "%.19s", val + pos);
    d.col = col;
    pos += 19;  // 19 chars keeps values left of the scroll icons
    first = false;
  } while (pos < len);
}

static bool allZero(const char* s) {
  for (; *s; ++s) if (*s != '0') return false;
  return true;
}

static void drawDetailList(bool clear) {
  if (clear) tft.fillRect(0, LIST_Y0, tft.width(), tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  g_detOff = constrain(g_detOff, 0, max(0, g_detN - vis));
  tft.setTextDatum(TL_DATUM);
  int rows = min(g_detN - g_detOff, vis);
  for (int r = 0; r < rows; r++) {
    const DetLine& d = g_detL[g_detOff + r];
    int y = LIST_Y0 + r * LIST_ROW_H;
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);  // dim label
    tft.drawString(d.label, 4, y, 1);
    tft.setTextColor(d.col, TFT_BLACK);         // bright value
    tft.drawString(d.val, 92, y, 1);
  }
  drawScrollIcons(g_detOff, g_detN, vis);
}

static void openDetail(int viewPos) {  // position within the filtered+sorted view
  int row = (viewPos >= 0 && viewPos < g_viewN) ? g_ex->view[viewPos] : -1;  // index into idx[]
  char line[LOG_LINE];
  LogRow r;
  if (row < 0 || row >= g_idxN || !readRowAt(g_ex->idx[row].offset, line, sizeof(line), r)) return;
  g_detN = 0; g_detOff = 0;
  char b[64];
  addField("Name", r.name[0] ? r.name : "<hidden>");
  addField("MAC", r.mac);
  addField("Source", !strcmp(r.src, "BLE") ? "BLE" : !strcmp(r.src, "PRB") ? "Probe request"
                   : !strcmp(r.src, "154") ? "802.15.4 (Zigbee/Thread)"
                   : !strcmp(r.src, "5G") ? "Wi-Fi 5 GHz" : "Wi-Fi 2.4 GHz");
  if (r.pan[0]) addField("PAN ID", r.pan);
  snprintf(b, sizeof(b), "%d dBm", r.rssi);
  addField("RSSI", b);
  if (r.channel) { snprintf(b, sizeof(b), "%d", r.channel); addField("Channel", b); }
  else addField("Channel", "-");
  static const char* const kTierName[] = { "None", "Suspect", "Likely", "Confirmed" };
  int n = snprintf(b, sizeof(b), "%s", kTierName[r.tier]);
  if (r.score) n += snprintf(b + n, sizeof(b) - n, " (score %d)", r.score);
  if (r.sig[0] && n < (int)sizeof(b)) snprintf(b + n, sizeof(b) - n, " - %s", r.sig);
  addField("Threat tier", b, tierColor(r.tier, (uint8_t)Source::WifiScan, 0));
  if (r.ie[0]   && !allZero(r.ie))   addField("IE fingerprint", r.ie);
  if (r.cid[0]  && !allZero(r.cid))  addField("BLE company ID", r.cid);
  if (r.uuid[0] && !allZero(r.uuid)) addField("Service UUID", r.uuid);
  if (r.epoch > 0) {
    time_t t = (time_t)r.epoch;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(b, sizeof(b), "unsynced (+%lus)", (unsigned long)(r.ms / 1000));
  }
  addField("Time", b);
  if (r.lat[0] && r.lon[0]) { snprintf(b, sizeof(b), "%s, %s", r.lat, r.lon); addField("GPS", b); }
  else addField("GPS", "none");

  g_screen = SCR_DETAIL;
  listTouchReset();
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("BACK", TFT_NAVY, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("DETAIL", 4, 46, 1);
  snprintf(b, sizeof(b), "row %d of %d", viewPos + 1, g_viewN);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(b, 4, 58, 1);
  drawDetailList(false);
}

// Act on a scan-menu row (touch tap or long-press select).
static void activateScanMenu(int sel) {
  if (sel == 0)      { g_screen = SCR_SCAN; g_scrollOffset = 0; }               // first cycle draws it
  else if (sel == 1) { exploreEnter(); }                                     // Explore Scan
  else if (sel == 2) { g_screen = SCR_SCANSETTINGS; g_scanSetSel = 0; drawScanSettings(); }
  else               { g_screen = SCR_MENU; drawMenu(); }                       // Back
}

// Act on a scan-settings row (touch tap or long-press select).
static void activateScanSettings(int sel) {
  if (sel == 0)      g_srcMask ^= MASK_BLE;
  else if (sel == 1) g_srcMask ^= SRC_24_BITS;
  else if (sel == 2) g_srcMask ^= MASK_WIFI5;
  else if (sel == 3) g_srcMask ^= MASK_154;
  else { g_screen = SCR_SCANMENU; drawScanMenu(); return; }  // Back
  saveSrcMask();
  drawScanSettings();
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
  buildGroups();  // once per cycle; shared by render() and pushDetections()
  setLed(!g_linkOk, g_linkOk, false);
  int newCount = logNewDetections();
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  Serial.printf("[CYD] table=%d (2.4:%d 5G:%d BLE:%d PRB:%d 154:%d) new=%d uniq=%d threats S%d/L%d/C%d db=%d\n",
                g_detCount, n24, n5, nble, nprb, n154, newCount, g_seenCount,
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
  else if (g_menuSel == 1) { g_screen = SCR_SCANMENU; g_scanSel = 0; drawScanMenu(); }   // opens sub-menu; no scan yet
  else if (g_menuSel == 2) { g_screen = SCR_SETTINGS; g_setSel = 0; drawSettings(); }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[CYD] boot: scanner + phone link + SD");
  initCommon();
  loadBrightness();
  loadSrcMask();
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
  { uint8_t m; if (phone::srcMaskRequested(&m) && m != 0) {  // web-app source toggles (ignore all-off)
      g_srcMask = m; saveSrcMask();
      if (g_screen == SCR_SCANSETTINGS) drawScanSettings();
      pushStatus();
    } }
  renameSessionOnSync();  // give this session a date-time filename once time is known

  // Phone-initiated Wi-Fi download overrides the current screen while active.
  if (phone::downloadRequested()) { exploreFree(); runDownload(); return; }
  if (webshare::active()) {  // just left download mode: tear down AP and repaint
    webshare::stop();
    g_screen = SCR_MENU; g_menuSel = 0; drawMenu(); pushStatus();
  }

  // Phone can start/stop the scan remotely (single toggle in the web app).
  if (phone::scanStartRequested()) { if (g_screen != SCR_SCAN) g_scrollOffset = 0; g_screen = SCR_SCAN; }
  // Consume the stop flag unconditionally, but only act on it while actually scanning
  // (so a stray stop sent from another screen is discarded, not buffered to fire later).
  if (phone::scanStopRequested() && g_screen == SCR_SCAN) { g_screen = SCR_SCANMENU; drawScanMenu(); pushStatus(); }

  // Explore and live scan are mutually exclusive: drop the index block as soon as we're out.
  if (g_ex && g_screen != SCR_PICKLOG && g_screen != SCR_SCANVIEWER && g_screen != SCR_DETAIL &&
      g_screen != SCR_SORT && g_screen != SCR_FILTER)
    exploreFree();

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

    case SCR_SCANMENU: {
      int t = tappedRow(SCAN_N);
      if (t >= 0)               { g_scanSel = t; activateScanMenu(t); return; }  // touch select
      if      (ev == BTN_SHORT) { g_scanSel = (g_scanSel + 1) % SCAN_N; drawScanMenu(); }
      else if (ev == BTN_LONG)  { activateScanMenu(g_scanSel); }  // "Back" row returns to HOME
      delay(20);
      return;
    }

    case SCR_SCANSETTINGS: {
      int t = tappedRow(SCANSET_N);
      if (t >= 0)               { g_scanSetSel = t; activateScanSettings(t); return; }  // touch select
      if      (ev == BTN_SHORT) { g_scanSetSel = (g_scanSetSel + 1) % SCANSET_N; drawScanSettings(); }
      else if (ev == BTN_LONG)  { activateScanSettings(g_scanSetSel); }
      delay(20);
      return;
    }

    case SCR_PICKLOG: {
      int t = listTouch(g_sessN + 1, &g_pickOff, PICK_VIS, ROW_Y0 - 5, ROW_STEP, drawPickLog);
      if (t >= 0)               { g_pickSel = t; activatePick(t); return; }  // touch select
      if (ev == BTN_SHORT) {
        g_pickSel = (g_pickSel + 1) % (g_sessN + 1);
        if (g_pickSel < g_pickOff) g_pickOff = g_pickSel;
        if (g_pickSel >= g_pickOff + PICK_VIS) g_pickOff = g_pickSel - PICK_VIS + 1;
        drawPickLog();
      } else if (ev == BTN_LONG) { activatePick(g_pickSel); }
      delay(20);
      return;
    }

    case SCR_SCANVIEWER: {
      int seg = topBarSegTapped(3);  // always called so its edge state stays current
      if (seg == 0 || ev == BTN_LONG) { viewerBack(); return; }
      if (seg == 1) { viewerOpenFilter(); return; }
      if (seg == 2) { viewerOpenSort();   return; }
      int t = listTouch(g_viewN, &g_viewOffset, visibleRows(), LIST_Y0, LIST_ROW_H,
                        []() { drawViewerList(true); });
      if (t >= 0) { openDetail(t); return; }
      if (ev == BTN_SHORT) {  // BOOT tap: page down (wraps to the top)
        int maxOff = max(0, g_viewN - visibleRows());
        g_viewOffset = (g_viewOffset >= maxOff) ? 0 : min(g_viewOffset + visibleRows(), maxOff);
        drawViewerList(true);
      }
      delay(20);
      return;
    }

    case SCR_SORT: {
      int t = menuNav(SORT_N, &g_sortSel, &g_sortOff, ev, drawSort);
      if (t >= 0) activateSort(t);
      delay(20);
      return;
    }

    case SCR_FILTER: {
      int t = menuNav(FILTER_N, &g_fltSel, &g_fltOff, ev, drawFilter);
      if (t >= 0) activateFilter(t);
      delay(20);
      return;
    }

    case SCR_DETAIL: {
      bool back = topBarTapped();
      if (back || ev != BTN_NONE) {
        g_screen = SCR_SCANVIEWER; listTouchReset(); drawViewer();
        return;
      }
      listTouch(g_detN, &g_detOff, visibleRows(), LIST_Y0, LIST_ROW_H,
                []() { drawDetailList(true); });
      delay(20);
      return;
    }

    case SCR_APPQR:
      if (ev != BTN_NONE) { g_screen = SCR_MENU; drawMenu(); }  // any press: back
      delay(20);
      return;

    case SCR_SCAN:
      // Stop via the on-screen button (touch), a long BOOT hold, or the phone.
      if (ev == BTN_LONG || topBarTapped()) { g_screen = SCR_SCANMENU; drawScanMenu(); pushStatus(); return; }
      runScanCycle();
      // Responsive ~2 s wait that also honors stop requests and pending downloads.
      {
        uint32_t t0 = millis();
        while (millis() - t0 < 2000) {
          if (buttonEvent() == BTN_LONG || topBarTapped() || phone::scanStopRequested()) {
            g_screen = SCR_SCANMENU; drawScanMenu(); pushStatus(); return;
          }
          if (phone::downloadRequested()) return;
          handleScanTouch();
          delay(20);
        }
      }
      return;
  }
}
#endif

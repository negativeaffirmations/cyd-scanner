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
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);
  tft.init();
  tft.setRotation(UI_ROTATION);
  LinkSerial.setRxBufferSize(2048);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
}

static const char* srcTag(const Detection& d) {
  if (d.source == (uint8_t)Source::BleScan) return "BLE";
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
bool               g_showAppQr = false;  // BOOT button shows the webapp QR

// URL of the hosted control web app (GitHub Pages). Scan the QR to open it.
static const char* APP_URL = "https://negativeaffirmations.github.io/cyd-scanner/webapp/";

// Rising-to-pressed edge detector for the BOOT button (active low).
static bool bootEdge() {
  static bool prevLow = false;
  bool low = (digitalRead(0) == LOW);
  bool edge = low && !prevLow;
  prevLow = low;
  return edge;
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

static void countBands(int& n24, int& n5, int& nble) {
  n24 = n5 = nble = 0;
  for (int i = 0; i < g_detCount; i++) {
    if (g_dets[i].source == (uint8_t)Source::BleScan) nble++;
    else (g_dets[i].channel > 14 ? n5 : n24)++;
  }
}

// Score every current detection against the signature DB (aligned into g_score[]).
static void computeScores() {
  for (int i = 0; i < g_detCount; i++) sigdb::score(g_dets[i], g_score[i]);
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
static constexpr uint32_t LOG_GEN = 2;

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

static void initSD() {
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  g_sdOk = SD.begin(SD_CS_PIN, sdSPI) && SD.cardType() != CARD_NONE;
  if (!g_sdOk) { Serial.println("[CYD] SD unavailable - logging disabled"); return; }
  wipeLogsIfNeeded();
  if (!SD.exists(kLogPath)) {
    File f = SD.open(kLogPath, FILE_WRITE);
    if (f) { f.println("epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,name,score,tier,signature"); f.close(); }
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
    f.printf("%lu,%lu,", (unsigned long)epoch, (unsigned long)ms);
    if (gps) f.printf("%.6f,%.6f,", phone::lat(), phone::lon());
    else     f.print(",,");
    f.printf("%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%d,%s,%d,%s,%s\n", srcTag(d),
             d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
             d.rssi, d.channel, safe, sc.score, sigdb::tierName(sc.tier), sig);
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
  cfg.sources  = MASK_WIFI | MASK_BLE;
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
  int n24, n5, nble; countBands(n24, n5, nble);
  int susp, lk, conf; countTiers(susp, lk, conf);
  char s[176];
  snprintf(s, sizeof(s),
           "link=%d;w24=%d;w5=%d;ble=%d;uniq=%d;time=%d;gps=%d;dl=%d;"
           "susp=%d;lk=%d;conf=%d;db=%d",
           g_linkOk ? 1 : 0, n24, n5, nble, g_seenCount,
           phone::hasTime() ? 1 : 0, phone::hasGps() ? 1 : 0,
           webshare::active() ? 1 : 0, susp, lk, conf, sigdb::loaded() ? 1 : 0);
  phone::setStatus(String(s));
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
  // connection state icon
  if (webshare::active())        iconWifi(2, 3, TFT_CYAN);
  else if (phone::connected())   iconBle(3, 3, TFT_BLUE);
  else                           iconNoConn(2, 3, TFT_DARKGREY);
  // time
  tft.setTextDatum(TL_DATUM);
  char t[8];
  if (phone::hasTime()) {
    uint32_t e = phone::epochNow();
    snprintf(t, sizeof(t), "%02u:%02u", (unsigned)((e / 3600) % 24), (unsigned)((e / 60) % 60));
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  } else { strcpy(t, "00:00"); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); }
  tft.drawString(t, 24, 4, 2);
  // GPS (abbreviated)
  char g[24];
  if (phone::hasGps()) {
    snprintf(g, sizeof(g), "%.2f,%.2f", phone::lat(), phone::lon());
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  } else { strcpy(g, "Lat:- Lon:-"); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); }
  tft.drawString(g, 78, 7, 1);
  tft.drawFastHLine(0, 21, W, TFT_DARKGREY);
}

static void render() {
  int n24, n5, nble; countBands(n24, n5, nble);
  int susp, lk, conf; countTiers(susp, lk, conf);
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();

  tft.setTextDatum(TL_DATUM);
  char buf[48];
  tft.setTextColor(g_linkOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
  snprintf(buf, sizeof(buf), "link:%s  SD:%s  db:%s",
           g_linkOk ? "up" : "DN", g_sdOk ? "on" : "off", sigdb::loaded() ? "on" : "fb");
  tft.drawString(buf, 4, 26, 1);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "2.4:%d 5G:%d BLE:%d uniq:%d", n24, n5, nble, g_seenCount);
  tft.drawString(buf, 4, 38, 1);

  uint16_t tcol = conf ? TFT_RED : lk ? TFT_ORANGE : susp ? TFT_YELLOW : TFT_DARKGREY;
  tft.setTextColor(tcol, TFT_BLACK);
  snprintf(buf, sizeof(buf), "threats  S:%d  L:%d  C:%d", susp, lk, conf);
  tft.drawString(buf, 4, 50, 1);

  // Sort by threat tier first, then RSSI, so flagged devices surface at the top.
  static int idx[MAX_DET];
  for (int i = 0; i < g_detCount; i++) idx[i] = i;
  std::sort(idx, idx + g_detCount, [](int a, int b) {
    if (g_score[a].tier != g_score[b].tier) return g_score[a].tier > g_score[b].tier;
    return g_dets[a].rssi > g_dets[b].rssi;
  });
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
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("LOG DOWNLOAD (Wi-Fi)", W / 2, 14, 2);
  int ty = drawCenteredQr(webshare::wifiQr().c_str(), 40) + 12;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan to join, then open", W / 2, ty, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(webshare::url(), W / 2, ty + 18, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(String("SSID ") + webshare::ssid(), W / 2, ty + 36, 1);
  tft.drawString(String("PW ") + webshare::password(), W / 2, ty + 48, 1);
}

// QR linking to the hosted web app, so the phone can open it by scanning.
static void drawAppQrScreen() {
  tft.fillScreen(TFT_BLACK);
  int W = tft.width();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("OPEN APP", W / 2, 14, 2);
  int ty = drawCenteredQr(APP_URL, 40) + 14;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan to open the app", W / 2, ty, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("BOOT again to exit", W / 2, ty + 20, 2);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[CYD] boot: scanner + phone link + SD");
  initCommon();
  initSD();
  sigdb::begin();  // load /signatures.csv (seeds it if absent) or fall back
  phone::begin(DEVICE_NAME);
  Serial.println("[CYD] ready");
}

void loop() {
  // Phone asked to reload the signature DB from SD (after an edit/push).
  if (phone::reloadRequested()) sigdb::reload();

  // Phone asked to download the log over BLE.
  if (phone::logRequested()) transferLog();

  // BOOT button shows a QR linking to the web app; press again to return.
  if (g_showAppQr) {
    if (bootEdge()) g_showAppQr = false;
    delay(20);
    return;
  }
  if (bootEdge()) { g_showAppQr = true; drawAppQrScreen(); return; }

  // Download mode: SoftAP + web server + QR on screen, no scanning meanwhile.
  if (phone::downloadRequested()) {
    if (!webshare::active()) { webshare::start(); drawDownloadScreen(); }
    webshare::handle();
    static uint32_t lastS = 0;
    if (millis() - lastS > 1000) { lastS = millis(); pushStatus(); }
    delay(2);
    return;
  }
  if (webshare::active()) webshare::stop();  // just left download mode

  requestScan();
  computeScores();
  setLed(!g_linkOk, g_linkOk, false);
  int newCount = logNewDetections();
  int n24, n5, nble; countBands(n24, n5, nble);
  int susp, lk, conf; countTiers(susp, lk, conf);
  Serial.printf("[CYD] table=%d (2.4:%d 5G:%d BLE:%d) new=%d uniq=%d threats S%d/L%d/C%d db=%d\n",
                g_detCount, n24, n5, nble, newCount, g_seenCount,
                susp, lk, conf, sigdb::loaded());
  render();
  pushStatus();

  // Responsive ~2 s wait that also enters the app-QR screen on a BOOT press.
  uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    if (bootEdge()) { g_showAppQr = true; drawAppQrScreen(); return; }
    delay(20);
  }
}
#endif

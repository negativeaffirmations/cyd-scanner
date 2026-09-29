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

static constexpr uint8_t UI_ROTATION = 1;  // landscape 320x240
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

static void initSD() {
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  g_sdOk = SD.begin(SD_CS_PIN, sdSPI) && SD.cardType() != CARD_NONE;
  if (!g_sdOk) { Serial.println("[CYD] SD unavailable - logging disabled"); return; }
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

static void render() {
  int n24, n5, nble; countBands(n24, n5, nble);
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("cyd-scanner", 6, 2, 4);

  char buf[56];
  tft.setTextColor(g_linkOk ? TFT_GREEN : TFT_RED, TFT_BLACK);
  snprintf(buf, sizeof(buf), "link:%s SD:%s BT:%s t:%s g:%s",
           g_linkOk ? "up" : "DN", g_sdOk ? "on" : "off",
           phone::connected() ? "on" : "..",
           phone::hasTime() ? "y" : "n", phone::hasGps() ? "y" : "n");
  tft.drawString(buf, 6, 34, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "2.4:%d 5G:%d BLE:%d uniq:%d", n24, n5, nble, g_seenCount);
  tft.drawString(buf, 6, 54, 2);

  int susp, lk, conf; countTiers(susp, lk, conf);
  uint16_t tcol = conf ? TFT_RED : lk ? TFT_ORANGE : susp ? TFT_YELLOW : TFT_DARKGREY;
  tft.setTextColor(tcol, TFT_BLACK);
  snprintf(buf, sizeof(buf), "threats S:%d L:%d C:%d  db:%s",
           susp, lk, conf, sigdb::loaded() ? "on" : "off");
  tft.drawString(buf, 6, 72, 2);

  // Sort by threat tier first, then RSSI, so flagged devices surface at the top.
  static int idx[MAX_DET];
  for (int i = 0; i < g_detCount; i++) idx[i] = i;
  std::sort(idx, idx + g_detCount, [](int a, int b) {
    if (g_score[a].tier != g_score[b].tier) return g_score[a].tier > g_score[b].tier;
    return g_dets[a].rssi > g_dets[b].rssi;
  });
  int rows = min(g_detCount, 6);
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
    char name[15];
    strncpy(name, d.name[0] ? d.name : "<hidden>", 14);
    name[14] = 0;
    char flag = g_score[i].tier != sigdb::Tier::None ? '!' : ' ';
    snprintf(buf, sizeof(buf), "%c%-3s %-13s %4d", flag, srcTag(d), name, d.rssi);
    tft.drawString(buf, 6, 92 + r * 16, 2);
  }
}

static void drawDownloadScreen() {
  QRCode qr;
  static uint8_t qrbuf[256];  // fits version 4
  String payload = webshare::wifiQr();
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, payload.c_str());

  tft.fillScreen(TFT_BLACK);
  const int scale = 5, qx = 14, qy = 46;
  int side = qr.size * scale;
  tft.fillRect(qx - 6, qy - 6, side + 12, side + 12, TFT_WHITE);  // quiet zone
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        tft.fillRect(qx + x * scale, qy + y * scale, scale, scale, TFT_BLACK);

  int tx = qx + side + 18;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("LOG DOWNLOAD", tx, 10, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan QR to join", tx, 40, 2);
  tft.drawString("WiFi, then open:", tx, 58, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(webshare::url(), tx, 78, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(String("SSID:") + webshare::ssid(), tx, 108, 2);
  tft.drawString(String("PW:") + webshare::password(), tx, 126, 2);
}

// QR linking to the hosted web app, so the phone can open it by scanning.
static void drawAppQrScreen() {
  QRCode qr;
  static uint8_t qrbuf[256];  // fits version 4 (URL ~55 bytes < 78 cap)
  qrcode_initText(&qr, qrbuf, 4, ECC_LOW, APP_URL);

  tft.fillScreen(TFT_BLACK);
  const int scale = 5, qx = 14, qy = 46;
  int side = qr.size * scale;
  tft.fillRect(qx - 6, qy - 6, side + 12, side + 12, TFT_WHITE);
  for (uint8_t y = 0; y < qr.size; y++)
    for (uint8_t x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        tft.fillRect(qx + x * scale, qy + y * scale, scale, scale, TFT_BLACK);

  int tx = qx + side + 18;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("OPEN APP", tx, 10, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan to open the", tx, 46, 2);
  tft.drawString("control web app", tx, 64, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("BOOT again to exit", tx, 120, 2);
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

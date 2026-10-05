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
#include "whitelist.h"
#include "touch.h"
#include "logfilter.h"

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

// --- "Following me" detector (Phase 5, device-only). Tunables; these become NVS-settable in a
// later increment. A device is FOLLOWING when it keeps showing up (persistence) while the
// phone's GPS says we moved far from where it was first seen (span) across several distinct
// places (fixes). Without GPS movement a device can only reach PERSISTENT. ---
#define FOLLOW_WINDOW_MS     15000UL   // presence-history bucket (32-bit mask -> ~8 min)
#define FOLLOW_TTL_MS        300000UL  // drop a device unseen this long (> the C5's 30 s TTL)
#define FOLLOW_SPAN_M        400.0f    // min GPS span (m) over which a device must track to FOLLOW
#define FOLLOW_FIXES         3         // ...across at least this many distinct GPS fixes
#define FOLLOW_PERSIST_FOLLOW 4        // ...and >= this many present windows (popcount)
#define FOLLOW_PERSIST_ONLY  6         // PERSISTENT (time only, no GPS needed): present windows
#define FOLLOW_PERSIST_B1    3         // persistence buckets (windows present)
#define FOLLOW_PERSIST_B2    6
#define FOLLOW_PERSIST_B3    12
#define FOLLOW_SPAN_B1       150.0f    // span buckets (metres)
#define FOLLOW_SPAN_B2       400.0f
#define FOLLOW_SPAN_B3       1000.0f
#define FOLLOW_FIXES_B1      2         // distinct-fix buckets
#define FOLLOW_FIXES_B2      4
#define FOLLOW_FIXES_B3      8
#define FOLLOW_RSSI_SPREAD   12        // max-min RSSI (dB) counted as "stable" ...
#define FOLLOW_SPAN_MIN      150.0f    // min span (m) before the RSSI-stability bonus applies
#define FOLLOW_W_PERSIST     10        // score weight per persistence bucket (max 30)
#define FOLLOW_W_SPAN        12        // per span bucket (max 36)
#define FOLLOW_W_FIXES       6         // per fixes bucket (max 18)
#define FOLLOW_W_STABLE      16        // RSSI-stability bonus
#define FOLLOW_W_THREAT      10        // bonus when the sigdb tier is above None
#define FOLLOW_QUANT         1e4f      // fix quantum: 1e-4 deg (~11 m)

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
              SCR_PICKLOG, SCR_SCANVIEWER, SCR_DETAIL, SCR_SORT, SCR_FILTER,
              SCR_FOLLOWLIST, SCR_FOLLOWACTION, SCR_FOLLOWDETAIL,
              SCR_WHITELIST, SCR_WLADD, SCR_WLRULE,
              SCR_SCANROW, SCR_SCANDETAIL, SCR_SCANCONFIRM, SCR_SCANFILTER };
static Screen g_screen  = SCR_MENU;
static int    g_menuSel = 0;
static int    g_setSel  = 0;
static int    g_scanSel = 0;
static int    g_scanSetSel = 0;
static constexpr int SCANSET_N = 7;  // 4 source tiles (BLE / 2.4 / 5G / 15.4), Background scan, Whitelist, Back
// Scan Settings geometry (shared by drawScanSettings AND scanSetTouch): 4 tiles in one row,
// then the Whitelist / Back rows on the drawListMenu row style.
static constexpr int SS_TILE_Y = 70, SS_TILE_H = 44, SS_TILE_GAP = 4, SS_MARGIN = 6;
static constexpr int SS_ROW_Y0 = 130, SS_ROW_STEP = 40, SS_ROW_H = 34;
static const char* kMenuItems[] = { "Phone Link", "Scan", "Settings" };
static constexpr int MENU_N = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
static constexpr int SET_N  = 3;  // Calibrate Touch / Brightness / Back
static const char* kScanItems[] = { "Scanner", "New Session", "Explore Scan", "Scan Settings", "Back" };
static constexpr int SCAN_N = sizeof(kScanItems) / sizeof(kScanItems[0]);

// Shared menu-row geometry (used for drawing AND touch hit-testing).
static constexpr int ROW_Y0 = 80, ROW_STEP = 40, ROW_H = 34;

// Top-bar button (full-width bar under the status bar): STOP on the live scan, BACK in the
// scan viewer / detail screens.
static constexpr int STOP_X = 4, STOP_Y = 24, STOP_H = 18;

// Scan-screen device list geometry + scrolling (rows are one MAC group each).
static constexpr int LIST_Y0 = 106, LIST_ROW_H = 13;
// Scan-screen state band (y64..75, just above the body-button band at y76): shows the scan state
// + tier counts, or the tappable follow alert when devices are flagged.
static constexpr int BANNER_Y = 64, BANNER_H = 11;
// Body action buttons (tall, in the band just above the list), with the other scan-screen layout
// constants (drawBodyButtonN below references these).
static constexpr int BODY_BTN_Y = 76, BODY_BTN_H = 26;
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
static uint8_t g_srcMask = (MASK_ALL & ~MASK_154);
static constexpr uint8_t SRC_24_BITS = MASK_WIFI24 | MASK_PROBE;
static void loadSrcMask() {
  Preferences p; p.begin("cydui", true);
  g_srcMask = p.getUChar("srcmask", (MASK_ALL & ~MASK_154));
  p.end();
}
static void saveSrcMask() {
  Preferences p; p.begin("cydui", false);
  p.putUChar("srcmask", g_srcMask);
  p.end();
}

// --- Background scan: when ON the scan cycle keeps running on any (non-SD-heavy) screen, headless,
// so detection/logging continues while the user is on the menu / Phone Link. Persisted in NVS. ---
// Default ON. BGDEF_GEN (like DB_GEN) force-flips already-provisioned units to ON exactly once: bump it
// only to re-apply a new default (it overwrites a user's saved choice).
static constexpr uint32_t BGDEF_GEN = 1;
static bool g_bgScan = true;
static void loadBgScan() {
  Preferences p; p.begin("cydui", false);
  if (p.getULong("bgdef_gen", 0) < BGDEF_GEN) {
    p.putBool("bgscan", true);
    p.putULong("bgdef_gen", BGDEF_GEN);
  }
  g_bgScan = p.getBool("bgscan", true);
  p.end();
}
static void saveBgScan() {
  Preferences p; p.begin("cydui", false);
  p.putBool("bgscan", g_bgScan);
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

// True on a fresh tap on the scan screen's follow banner (full width, just above the list).
// Call every loop iteration so its edge state stays current; the caller checks g_followN.
static bool bannerTapped() {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return false; }
  bool now = g_touch.touched();
  bool hit = false;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z) && sy >= BANNER_Y && sy <= BANNER_Y + BANNER_H) hit = true;
  }
  prev = now;
  return hit;
}

SPIClass  sdSPI(HSPI);
bool      g_sdOk = false;
// Legacy CSV header (schema v5): only used to recognize header-only old .csv files in the sweep.
static const char* kLogHeader =
    "epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,ie,cid,uuid,pan,name,score,tier,signature";
static constexpr size_t LOG_LINE = 512;  // max log line (NDJSON lines are ~100-330 B; worst case < 512)

struct SeenKey { uint8_t source; uint8_t mac[6]; uint16_t pan; };

// One log stream = one live NDJSON session file + its own first-seen dedup set. Two streams:
//   g_fg = foreground/manual sessions (the on-screen scan):  /logs/sess-NNNNN.jsonl -> /logs/YYYYMMDD-HHMMSS.jsonl
//   g_bg = headless background scan (g_screen != SCR_SCAN):  /logs/bg-NNNNN.jsonl   -> /logs/bg-YYYYMMDD-HHMMSS.jsonl
// The file is created lazily on the first row; the counter name is renamed to a date-time name once
// the phone syncs the clock (renameSessionOnSync). Each stream dedups independently (MAX_SEEN each).
struct LogStream {
  char    path[48];   // current session file (NDJSON)
  bool    named;      // true once renamed to the date-time form
  bool    started;    // true once the file is actually created (first row)
  SeenKey seen[MAX_SEEN];
  int     seenCount;
};
static LogStream g_fg;  // zero-init (.bss); path set by openSession()
static LogStream g_bg;
static char g_streamingPath[48] = "";  // file currently being streamed to a phone (BLE); never auto-deleted

// Headless background cycle = scan polling while NOT on the scanner screen (only runs when bg is on).
static bool bgCtx() { return g_bgScan && g_screen != SCR_SCAN; }
// The stream the current screen logs to / "current session" refers to.
static LogStream& activeStream() { return bgCtx() ? g_bg : g_fg; }
// Keep the Wi-Fi download server's "current session" pointed at the active stream.
static void syncSharePath() {
  static char last[48] = "";
  const char* cur = activeStream().path;
  if (strcmp(last, cur) == 0) return;
  strncpy(last, cur, sizeof(last) - 1);
  webshare::setLogPath(cur);
}

// Short-address 15.4 devices are only unique within a PAN, so key them by panId too
// (pan = 0 for every other source, which keeps their key source+MAC).
static uint16_t seenPan(const Detection& d) {
  return (d.source == (uint8_t)Source::Ieee802154 && !(d.flags & FLAG_154_EXTENDED)) ? d.panId : 0;
}
static bool seenContains(const LogStream& st, const Detection& d) {
  for (int i = 0; i < st.seenCount; i++)
    if (st.seen[i].source == d.source && memcmp(st.seen[i].mac, d.mac, 6) == 0 &&
        st.seen[i].pan == seenPan(d)) return true;
  return false;
}
static void seenAdd(LogStream& st, const Detection& d) {
  if (st.seenCount >= MAX_SEEN) return;
  st.seen[st.seenCount].source = d.source;
  st.seen[st.seenCount].pan = seenPan(d);
  memcpy(st.seen[st.seenCount].mac, d.mac, 6);
  st.seenCount++;
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

// --- C5 behavioral-detector flags (Detection.flags, protocol v6). Notable flags carry a minimum
// threat tier (applied after sigdb::score), a short on-screen label that overrides the sigdb
// signature label, and a lowercase token for the NDJSON "flags" key. iBeacon is informational
// only (no tier bump). Order = label priority when several are set. ---
struct FlagRule { uint16_t bit; sigdb::Tier floor; const char* label; const char* token; };
static const FlagRule kFlagRules[] = {
  { FLAG_BLE_FINDMY,      sigdb::Tier::Suspect, "Find My",    "findmy"     },
  { FLAG_WIFI_PWNAGOTCHI, sigdb::Tier::Likely,  "Pwnagotchi", "pwnagotchi" },
  { FLAG_WIFI_EVILTWIN,   sigdb::Tier::Likely,  "Evil Twin",  "eviltwin"   },
  { FLAG_BLE_ODID,        sigdb::Tier::Suspect, "Drone RID",  "droneid"    },
  { FLAG_BLE_IBEACON,     sigdb::Tier::None,    "iBeacon",    "ibeacon"    },
};
static constexpr uint16_t FLAGS_NOTABLE = FLAG_BLE_IBEACON | FLAG_BLE_FINDMY | FLAG_WIFI_PWNAGOTCHI |
                                          FLAG_WIFI_EVILTWIN | FLAG_BLE_ODID;
static char g_flagLabel[MAX_DET][16];  // aligned with g_dets; "" when no notable flag

// '+'-joined token list of the notable flags in `flags` ("" if none).
static void flagTokens(uint16_t flags, char* out, size_t cap) {
  size_t o = 0;
  if (cap) out[0] = 0;
  for (const FlagRule& r : kFlagRules) {
    if (!(flags & r.bit)) continue;
    int w = snprintf(out + o, cap - o, o ? "+%s" : "%s", r.token);
    if (w < 0 || (size_t)w >= cap - o) { out[o] = 0; break; }
    o += w;
  }
}

// Label to show for detection i: the flag label when one exists, else the sigdb signature label.
static const char* detLabel(int i) {
  return g_flagLabel[i][0] ? g_flagLabel[i] : sigdb::labelFor(g_score[i]);
}

// Score every current detection against the signature DB (aligned into g_score[]), then raise
// the tier to at least each set flag's floor and record the flag label.
static void computeScores() {
  for (int i = 0; i < g_detCount; i++) {
    sigdb::score(g_dets[i], g_score[i]);
    g_flagLabel[i][0] = 0;
    for (const FlagRule& r : kFlagRules) {
      if (!(g_dets[i].flags & r.bit)) continue;
      if (r.floor > g_score[i].tier) g_score[i].tier = r.floor;
      if (!g_flagLabel[i][0]) snprintf(g_flagLabel[i], sizeof(g_flagLabel[i]), "%s", r.label);
    }
  }
}

// --- Aggregate C5 events (Status.deauth_recent / evil_count / rid_count, protocol v6). The values
// are refreshed by every requestScan(); an alert is held for EVENT_HOLD_MS so a one-cycle burst
// stays visible across the ~2 s poll cadence. Receive-side only: these are counts of frames the C5
// OBSERVED, nothing is transmitted. ---
#define DEAUTH_ALERT      20        // deauth/disassoc frames in the C5's last ~1 s that raise the alert
#define EVENT_HOLD_MS     6000UL    // keep an alert up this long after the last trigger
#define EVENT_LOG_GAP_MS  30000UL   // log at most one event line per type per this interval
static uint16_t g_deauthRecent = 0;
static uint8_t  g_evilCount = 0, g_ridCount = 0;
static uint16_t g_deauthShown = 0;   // values captured when the alert was last raised (banner text)
static uint8_t  g_evilShown = 0;
static uint32_t g_deauthUntil = 0, g_evilUntil = 0;  // millis() deadlines (0 = never raised)
static bool deauthAlertActive() { return g_deauthUntil && (int32_t)(g_deauthUntil - millis()) > 0; }
static bool evilAlertActive()   { return g_evilUntil   && (int32_t)(g_evilUntil   - millis()) > 0; }
static bool eventAlertActive()  { return deauthAlertActive() || evilAlertActive(); }

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
  bool     whitelisted;  // matches a /whitelist.csv rule (muted; excluded from threat counts)
  char     tag[12];   // combined distinct source tag, e.g. "2.4+PRB"
  char     srcs[48];  // "tag:rssi,tag:rssi" list for the phone stream
  uint16_t flags;     // union of the notable C5 detector flags (FLAGS_NOTABLE) across members
  uint8_t  srcMask;   // union of srcType bits (2.4/5G/BLE/PRB/154) across members; computed at
                      // build time so the live-view/snapshot filter never depends on the transient
                      // g_dets table (which requestScan() zeroes, and an aborted scan leaves empty)
};
static int      g_sortIdx[MAX_DET];   // detections sorted tier-first then RSSI (see buildGroups)
static DevGroup g_groups[MAX_DET];
static int      g_groupCount = 0;

// --- Scanner screen (SCR_SCAN) state. The visible list is a filtered view (g_scanView) of either
// the live g_groups or, while the view is frozen (PAUSE), a snapshot (g_frozenRows). Bounded static
// buffers sized MAX_DET (no heap). ---
struct ScanRow {
  uint8_t mac[6]; char name[20]; char tag[12]; int16_t rssi;
  uint8_t tier, source, channel, srcMask; bool whitelisted;
};
static bool     g_scanActive = false;   // C5 polling on (START/STOP); independent of the screen
static bool     g_viewFrozen = false;   // list frozen on a snapshot while scanning continues
static uint8_t  g_lvType   = 0x1F;      // live filter srcType bits 2.4/5G/BLE/PRB/154
static bool     g_lvThreat = false;
static ScanRow  g_frozenRows[MAX_DET];
static int      g_frozenCount = 0;
static uint8_t  g_scanView[MAX_DET];    // filtered index into g_groups (live) OR g_frozenRows (frozen)
static int      g_scanViewN = 0;
static uint8_t  g_scanRowMac[6];        // device the row-action / detail screens target
static int      g_scanRowSel = 0, g_sfSel = 0, g_sfOff = 0, g_confirmSel = 0;
static uint32_t g_lastCycleMs = 0;
// Scan-poll cadence + link stickiness. The C5 is a continuous scanner that cannot service the link
// while its Wi-Fi scan phase runs (a multi-second busy window), so a poll landing in that window
// gets no reply. That is a BUSY MISS, not a link failure: we keep the last table/view, don't flash
// the link dot, and retry soon instead of after the full cadence. The link only counts as DOWN once
// no successful exchange has happened for LINK_STICKY_MS (covers the longest observed busy window).
static constexpr uint32_t SCAN_CYCLE_MS  = 2000;   // normal poll spacing
static constexpr uint32_t SCAN_RETRY_MS  = 400;    // quick re-poll after a busy miss (catch the C5 free)
static constexpr uint32_t SCAN_CYCLE_DOWN_MS = 5000; // headless bg-scan poll spacing when the C5 link is down
                                                     // (each poll costs ~800ms of no-reply wait; back off so menus stay snappy)
static constexpr uint32_t LINK_STICKY_MS = 8000;   // treat the link as up this long after the last reply
static uint32_t g_lastLinkOkMs = 0;                // millis() of the last successful scan exchange / ping
static const char* g_scanMsg = nullptr; static uint16_t g_scanMsgCol = TFT_GREEN;  // state-line feedback

static uint8_t srcTypeOf(const char* tag);  // fwd decl (defined below with the view helpers)

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
      g.nameIdx = -1; g.ie = 0; g.whitelisted = false; g.srcMask = 0; g.flags = 0;
    }
    DevGroup& g = g_groups[gi];
    if ((int)g_score[i].tier > g.tier) g.tier = (int)g_score[i].tier;
    if (d.rssi > g.bestRssi) { g.bestRssi = d.rssi; g.rep = i; }
    if (g.nameIdx < 0 && d.name[0]) g.nameIdx = i;
    if (!g.ie && d.ie_hash) g.ie = d.ie_hash;
    g.flags |= (uint16_t)(d.flags & FLAGS_NOTABLE);
    g.srcMask |= (uint8_t)(1u << srcTypeOf(srcTag(d)));
  }
  // Whitelist check per device (before the sort, which uses it): union of member sources,
  // first company ID / service UUID.
  static const uint8_t zeroSvc[16] = {0};
  for (int k = 0; k < n; k++) {
    DevGroup& g = g_groups[k];
    uint8_t bits = 0; uint16_t cid = 0; const uint8_t* svc = zeroSvc;
    for (int r = 0; r < g_detCount; r++) {
      const Detection& d = g_dets[g_sortIdx[r]];
      if (memcmp(d.mac, g.mac, 6) != 0) continue;
      bits |= (uint8_t)(1u << d.source);
      if (!cid) cid = d.companyId;
      if (svc == zeroSvc) for (int j = 0; j < 16; j++) if (d.svc[j]) { svc = d.svc; break; }
    }
    g.whitelisted = whitelist::match(g.mac, g.nameIdx >= 0 ? g_dets[g.nameIdx].name : nullptr,
                                     cid, svc, bits);
  }
  std::sort(g_groups, g_groups + n, [](const DevGroup& a, const DevGroup& b) {
    if (a.whitelisted != b.whitelisted) return !a.whitelisted;  // muted devices sink
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

static int countMuted() {
  int m = 0;
  for (int k = 0; k < g_groupCount; k++) if (g_groups[k].whitelisted) m++;
  return m;
}

// ---- Following-me state (one entry per MAC, bounded, static; CYD loop is single-threaded) ----
struct FollowState {
  uint8_t  mac[6];
  uint16_t sightings;
  uint32_t firstMs, lastMs;
  uint32_t windowMask;      // 1 bit per FOLLOW_WINDOW_MS, bit0 = window baseWin (newest)
  uint8_t  baseWin;         // window index (mod 256) of bit0
  int8_t   rssiMin, rssiMax;
  float    anchorLat, anchorLon;  // first GPS fix seen at (NAN until a fix)
  float    maxSpanM;        // farthest anchor->current distance seen
  int32_t  lastQLat, lastQLon;    // last counted quantized fix
  uint8_t  distinctFixes;
  uint8_t  score;           // 0..100
  uint8_t  ftier;           // 0 none / 1 PERSISTENT / 2 FOLLOWING
  uint8_t  flags;           // FF_ACTIVE | FF_HADGPS
};
static constexpr uint8_t FF_ACTIVE = 1, FF_HADGPS = 2;
static FollowState g_follow[MAX_DET];
static int         g_followCount = 0;
static int         g_followN     = 0;   // devices currently FOLLOWING

static void resetFollow() {
  memset(g_follow, 0, sizeof(g_follow));
  g_followCount = 0; g_followN = 0;
}

static const FollowState* findFollow(const uint8_t* mac) {
  for (int i = 0; i < g_followCount; i++)
    if (memcmp(g_follow[i].mac, mac, 6) == 0) return &g_follow[i];
  return nullptr;
}

static void updateFollowState() {
  uint32_t now = millis();  // millis() wrap (~49 d) ignored, as elsewhere for a handheld session
  // Expire stale entries (compact in place).
  int w = 0;
  for (int i = 0; i < g_followCount; i++)
    if (now - g_follow[i].lastMs <= FOLLOW_TTL_MS) { if (w != i) g_follow[w] = g_follow[i]; w++; }
  g_followCount = w;

  uint32_t nowWin = now / FOLLOW_WINDOW_MS;
  bool  gps = phone::hasGps();
  float lat = 0, lon = 0, cosLat = 1.0f;
  int32_t qLat = 0, qLon = 0;
  if (gps) {
    lat = phone::lat(); lon = phone::lon();
    cosLat = cosf(lat * (float)M_PI / 180.0f);  // once per cycle; lat barely changes
    qLat = (int32_t)lroundf(lat * FOLLOW_QUANT);
    qLon = (int32_t)lroundf(lon * FOLLOW_QUANT);
  }

  for (int k = 0; k < g_groupCount; k++) {
    const DevGroup& g = g_groups[k];
    FollowState* f = nullptr;
    for (int i = 0; i < g_followCount; i++)
      if (memcmp(g_follow[i].mac, g.mac, 6) == 0) { f = &g_follow[i]; break; }
    if (!f) {
      if (g_followCount >= MAX_DET) {  // full: evict oldest. Routine in busy RF (5 min TTL vs a ~2 s rebuild), so tracking caps at MAX_DET devices
        int o = 0;
        for (int i = 1; i < g_followCount; i++)
          if (now - g_follow[i].lastMs > now - g_follow[o].lastMs) o = i;
        f = &g_follow[o];
      } else f = &g_follow[g_followCount++];
      memset(f, 0, sizeof(*f));
      memcpy(f->mac, g.mac, 6);
      f->firstMs = now; f->baseWin = (uint8_t)nowWin;
      f->rssiMin = 127; f->rssiMax = -128;
      f->anchorLat = f->anchorLon = NAN;
      f->flags = FF_ACTIVE;
    }
    if (f->sightings < 0xFFFF) f->sightings++;
    f->lastMs = now;
    // Roll the presence mask forward so bit0 = the current window.
    uint8_t delta = (uint8_t)((uint8_t)nowWin - f->baseWin);
    if (delta) { f->windowMask = delta >= 32 ? 0 : (f->windowMask << delta); f->baseWin = (uint8_t)nowWin; }
    f->windowMask |= 1u;
    int r = constrain(g.bestRssi, -127, 127);
    if (r < f->rssiMin) f->rssiMin = (int8_t)r;
    if (r > f->rssiMax) f->rssiMax = (int8_t)r;
    if (gps) {
      if (isnan(f->anchorLat)) {
        f->anchorLat = lat; f->anchorLon = lon;
        f->lastQLat = qLat; f->lastQLon = qLon; f->distinctFixes = 1;
      } else {
        // Equirectangular: dx = dLon*cos(lat), dy = dLat, 1 deg ~ 111320 m.
        float dx = (lon - f->anchorLon) * cosLat * 111320.0f;
        float dy = (lat - f->anchorLat) * 111320.0f;
        float d  = sqrtf(dx * dx + dy * dy);
        if (d > f->maxSpanM) f->maxSpanM = d;
        if ((qLat != f->lastQLat || qLon != f->lastQLon) && f->distinctFixes < 255) {
          f->distinctFixes++; f->lastQLat = qLat; f->lastQLon = qLon;
        }
      }
      f->flags |= FF_HADGPS;
    }
  }
}

static void computeFollowScores() {
  g_followN = 0;
  for (int i = 0; i < g_followCount; i++) {
    FollowState& f = g_follow[i];
    int pers = __builtin_popcount(f.windowMask);
    bool gpsOk = (f.flags & FF_HADGPS) && !isnan(f.anchorLat);
    float span = gpsOk ? f.maxSpanM : 0;
    int fixes  = gpsOk ? f.distinctFixes : 0;
    int sc = FOLLOW_W_PERSIST * ((pers >= FOLLOW_PERSIST_B1) + (pers >= FOLLOW_PERSIST_B2) + (pers >= FOLLOW_PERSIST_B3))
           + FOLLOW_W_SPAN    * ((span >= FOLLOW_SPAN_B1) + (span >= FOLLOW_SPAN_B2) + (span >= FOLLOW_SPAN_B3))
           + FOLLOW_W_FIXES   * ((fixes >= FOLLOW_FIXES_B1) + (fixes >= FOLLOW_FIXES_B2) + (fixes >= FOLLOW_FIXES_B3));
    if (span >= FOLLOW_SPAN_MIN && (f.rssiMax - f.rssiMin) <= FOLLOW_RSSI_SPREAD) sc += FOLLOW_W_STABLE;
    // Threat boost: this MAC's group tier (sigdb) above None. Also capture whitelist state.
    bool whitelisted = false;
    for (int k = 0; k < g_groupCount; k++)
      if (memcmp(g_groups[k].mac, f.mac, 6) == 0) {
        if (g_groups[k].tier != (int)sigdb::Tier::None) sc += FOLLOW_W_THREAT;
        whitelisted = g_groups[k].whitelisted;
        break;
      }
    // Whitelisted (known) devices never raise the follow alert or the magenta LED.
    if (whitelisted) { f.score = 0; f.ftier = 0; continue; }
    f.score = (uint8_t)min(sc, 100);
    // PERSISTENT is time-only and capped: only GPS movement can reach FOLLOWING.
    if (gpsOk && span >= FOLLOW_SPAN_M && fixes >= FOLLOW_FIXES && pers >= FOLLOW_PERSIST_FOLLOW) f.ftier = 2;
    else if (pers >= FOLLOW_PERSIST_ONLY) f.ftier = 1;
    else f.ftier = 0;
    if (f.ftier == 2) g_followN++;
  }
}

static void countTiers(int& susp, int& lk, int& conf) {
  susp = lk = conf = 0;
  for (int i = 0; i < g_detCount; i++) {
    bool muted = false;  // known (whitelisted) devices don't count toward the alarm totals
    for (int k = 0; k < g_groupCount; k++)
      if (memcmp(g_groups[k].mac, g_dets[i].mac, 6) == 0) { muted = g_groups[k].whitelisted; break; }
    if (muted) continue;
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
// Shared by boot + "New Session" + the background stream: bump the persistent counter (monotonic
// across boots and new sessions, so names never repeat; the bg stream has its own counter), adopt
// the counter name, reset that stream's dedup, and re-arm the time-sync rename (named=false) so the
// next renameSessionOnSync() gives it a date-time name if time is known.
static void enforceLogCap();  // fwd decl (defined with the other /logs/ helpers below)
static void beginSessionNamed(LogStream& st, bool bg) {
  Preferences p; p.begin("cydscan", false);
  const char* key = bg ? "bgcnt" : "bootcnt";
  uint32_t n = p.getULong(key, 0) + 1;
  p.putULong(key, n);
  p.end();
  snprintf(st.path, sizeof(st.path), bg ? "/logs/bg-%05lu.jsonl" : "/logs/sess-%05lu.jsonl", (unsigned long)n);
  st.named     = false;   // re-arm rename-on-sync
  st.started   = false;   // file is created lazily on the first row (ensureLogFile)
  st.seenCount = 0;       // fresh per-session dedup
  Serial.printf("[CYD] %s session log (created on first detection): %s\n", bg ? "bg" : "fg", st.path);
}

static void openSession() {
  SD.mkdir("/logs");
  if (SD.exists("/scanlog.csv")) SD.remove("/scanlog.csv");  // retire the legacy single file (CSV era)
  sweepEmptySessions();
  beginSessionNamed(g_fg, false);
  resetFollow();          // a "following" judgement is per continuous trip
}

// Open the background stream's session (boot with bg on, and when bg is toggled on). A pending bg
// session that never wrote a row is reused rather than burning another counter value.
static void openBgSession() {
  if (!g_sdOk) return;
  if (g_bg.path[0] && !g_bg.started) return;
  beginSessionNamed(g_bg, true);
}

// Start a fresh session without rebooting (Scan menu / phone CMD "N"). Rotates the stream the
// current screen logs to (the Scan menu passes forceFg so its New Session always means the manual
// foreground session). The writer opens with FILE_APPEND and closes every cycle, so there is no
// handle to flush. An unused previous session left no file (lazy create), so nothing is orphaned.
static void startNewSession(bool forceFg = false) {
  if (!forceFg && bgCtx()) beginSessionNamed(g_bg, true);
  else                     beginSessionNamed(g_fg, false);
  resetFollow();
  enforceLogCap();        // rotation trigger: trim /logs/ to the cap
  syncSharePath();
}

// Create the (empty) session file the first time a row needs writing. NDJSON has no header.
// No-op once created. Returns false if SD is unavailable or can't be opened.
static bool ensureLogFile(LogStream& st) {
  if (st.started) return true;
  if (!g_sdOk || !st.path[0]) return false;
  File f = SD.open(st.path, FILE_WRITE);
  if (!f) return false;
  f.close();
  st.started = true;
  Serial.printf("[CYD] session log created: %s\n", st.path);
  return true;
}

// Once the phone provides wall-clock time, rename a boot-counter session file to a date-time name
// (fg: YYYYMMDD-HHMMSS.jsonl, bg: bg-YYYYMMDD-HHMMSS.jsonl). Runs once per session (even if the
// rename fails, it won't retry-spam).
static void renameStreamOnSync(LogStream& st, bool bg) {
  if (st.named || !st.path[0] || !g_sdOk || !phone::hasTime()) return;
  time_t t = (time_t)phone::epochNow();
  struct tm tmv;
  gmtime_r(&t, &tmv);  // epoch is stored already-localized, so gmtime gives local fields
  char nn[48];
  snprintf(nn, sizeof(nn), "/logs/%s%04d%02d%02d-%02d%02d%02d.jsonl", bg ? "bg-" : "",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  if (SD.exists(nn)) { st.named = true; return; }  // name taken; keep the current one
  if (!st.started) {
    // File not created yet (no detections logged): just adopt the date-time name so the
    // file is born correctly named when the first row is written.
    strncpy(st.path, nn, sizeof(st.path) - 1);
    st.path[sizeof(st.path) - 1] = 0;
    Serial.printf("[CYD] session log will use %s\n", st.path);
  } else if (SD.rename(st.path, nn)) {
    Serial.printf("[CYD] session log renamed %s -> %s\n", st.path, nn);
    strncpy(st.path, nn, sizeof(st.path) - 1);
    st.path[sizeof(st.path) - 1] = 0;
  }
  st.named = true;
}
static void renameSessionOnSync() {
  renameStreamOnSync(g_fg, false);
  renameStreamOnSync(g_bg, true);
  syncSharePath();
}

// Bump DB_GEN to force a one-time delete of /signatures.csv so sigdb re-seeds it from
// the firmware (e.g. after adding seed rules). This DISCARDS any on-card DB edits, so
// only bump it when that's intended.
static constexpr uint32_t DB_GEN = 2;  // 1: Phase-3 Flock GATT bleuuid rule; 2: SquachWatch roster expansion

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
  if (g_bgScan) openBgSession();  // and the background stream /logs/bg-NNNNN.jsonl
  enforceLogCap();     // rotation trigger: boot
  syncSharePath();
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
  LogStream& st = activeStream();  // headless bg cycle -> bg stream; on-screen scan -> fg stream
  if (bgCtx() && !st.path[0]) openBgSession();
  File f;  // opened lazily on the first new detection so idle boots write no file
  uint32_t ms = millis();
  uint32_t epoch = phone::epochNow();
  bool gps = phone::hasGps();
  for (int i = 0; i < g_detCount; i++) {
    const Detection& d = g_dets[i];
    if (seenContains(st, d)) continue;
    seenAdd(st, d);
    n++;
    if (!f && ensureLogFile(st)) f = SD.open(st.path, FILE_APPEND);
    if (!f) continue;
    char safe[97];  // escaped name (32 B name, worst case \uXXXX per byte truncates safely)
    jsonEscape(safe, sizeof(safe), d.name);
    const sigdb::ScoreResult& sc = g_score[i];
    char sig[49];
    jsonEscape(sig, sizeof(sig), detLabel(i));
    char flagStr[64]; flagTokens(d.flags, flagStr, sizeof(flagStr));
    char panStr[5]; panStr[0] = 0;  // hex PAN, 802.15.4 only
    if (d.source == (uint8_t)Source::Ieee802154 && d.panId) snprintf(panStr, sizeof(panStr), "%04X", d.panId);
    char uuidStr[33]; uuidStr[0] = 0;
    for (int k = 0; k < 16; k++)
      if (d.svc[k]) { for (int j = 0; j < 16; j++) sprintf(uuidStr + j * 2, "%02X", d.svc[j]); break; }
    // Whitelist state of this device, looked up per-group by MAC (buildGroups() ran this
    // cycle). Stamped into the log so downstream tools can filter muted detections.
    bool wl = false;
    for (int k = 0; k < g_groupCount; k++)
      if (memcmp(g_groups[k].mac, d.mac, 6) == 0) { wl = g_groups[k].whitelisted; break; }
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
    if (flagStr[0])  lnAppend(ln, n2, ",\"flags\":\"%s\"", flagStr);
    lnAppend(ln, n2, ",\"wl\":%d", wl ? 1 : 0);
    if (n2 > (int)LOG_LINE - 2) continue;  // can't close the object in-buffer: drop the row, never write truncated JSON
    ln[n2++] = '}'; ln[n2++] = '\n';
    f.write((const uint8_t*)ln, n2);
  }
  if (f) f.close();
  return n;
}

// Append one event line to the current session log:
//   {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"deauth"|"eviltwin","count":N}
// No "mac" key, so the Explore viewer's row parser skips it; the web Map renders it as an event pin.
static void logEvent(const char* evt, int count) {
  LogStream& st = activeStream();
  if (bgCtx() && !st.path[0]) openBgSession();
  if (!ensureLogFile(st)) return;
  File f = SD.open(st.path, FILE_APPEND);
  if (!f) return;
  char ln[160];
  int n = 0;
  lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)phone::epochNow(), (unsigned long)millis());
  if (phone::hasGps() && isfinite(phone::lat()) && isfinite(phone::lon()))
    lnAppend(ln, n, ",\"lat\":%.6f,\"lon\":%.6f", phone::lat(), phone::lon());
  lnAppend(ln, n, ",\"evt\":\"%s\",\"count\":%d}\n", evt, count);
  if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
  f.close();
}

// Raise/hold the deauth + evil-twin alerts from the latest C5 Status and log an event line for each
// type at most once per EVENT_LOG_GAP_MS (never per cycle).
static void updateEvents() {
  uint32_t now = millis();
  static uint32_t lastLog[2] = {0, 0};
  static bool     logged[2]  = {false, false};
  if (g_deauthRecent >= DEAUTH_ALERT) {
    g_deauthUntil = now + EVENT_HOLD_MS; if (!g_deauthUntil) g_deauthUntil = 1;
    g_deauthShown = g_deauthRecent;
    if (!logged[0] || now - lastLog[0] >= EVENT_LOG_GAP_MS) {
      logged[0] = true; lastLog[0] = now; logEvent("deauth", g_deauthRecent);
    }
  }
  if (g_evilCount > 0) {
    g_evilUntil = now + EVENT_HOLD_MS; if (!g_evilUntil) g_evilUntil = 1;
    g_evilShown = g_evilCount;
    if (!logged[1] || now - lastLog[1] >= EVENT_LOG_GAP_MS) {
      logged[1] = true; lastLog[1] = now; logEvent("eviltwin", g_evilCount);
    }
  }
}

// Stream an SD file to the phone over BLE: a "SIZE=<n>" header, then the raw file in
// chunks. The web app reassembles and saves it (no Wi-Fi needed).
static void streamFileOverBle(const char* path) {
  strncpy(g_streamingPath, path, sizeof(g_streamingPath) - 1);  // guard: not rotated away mid-stream
  g_streamingPath[sizeof(g_streamingPath) - 1] = 0;
  if (!g_sdOk || !SD.exists(path)) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE dl: no file %s\n", path);
    g_streamingPath[0] = 0;
    return;
  }
  File f = SD.open(path, "r");
  if (!f) { phone::logNotify((const uint8_t*)"SIZE=0", 6); g_streamingPath[0] = 0; return; }
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
  g_streamingPath[0] = 0;
}

// Time-filtered BLE export: filter ON the CYD, never stream the whole file. TWO passes over the
// file -- pass 1 sums the byte length of the lines inside the window (for the SIZE= header), pass 2
// streams only those lines. Same LineReader + predicate for both, so the totals always agree.
static void streamFilteredOverBle(const char* path, const logfilter::Cutoff& c) {
  if (!g_sdOk || !SD.exists(path)) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  File f = SD.open(path, "r");
  if (!f) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  strncpy(g_streamingPath, path, sizeof(g_streamingPath) - 1);
  g_streamingPath[sizeof(g_streamingPath) - 1] = 0;
  char line[logfilter::LINE_CAP];
  size_t len;
  uint32_t total = 0, nlines = 0;
  {
    logfilter::LineReader rd(f);
    while (rd.next(line, len)) if (logfilter::keep(line, c)) { total += len; nlines++; }
  }
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SIZE=%u", (unsigned)total);
  phone::logNotify((const uint8_t*)hdr, hn);
  if (total == 0) { f.close(); g_streamingPath[0] = 0; Serial.printf("[CYD] BLE filtered: no rows in range (%s)\n", path); return; }
  delay(30);
  f.seek(0);
  uint8_t out[180];  // chunk < MTU-3
  size_t on = 0;
  uint32_t sent = 0;
  logfilter::LineReader rd2(f);
  while (rd2.next(line, len) && phone::connected()) {
    if (!logfilter::keep(line, c)) continue;
    for (size_t off = 0; off < len; ) {
      size_t n = min(len - off, sizeof(out) - on);
      memcpy(out + on, line + off, n);
      on += n; off += n;
      if (on == sizeof(out)) { phone::logNotify(out, on); sent += on; on = 0; delay(15); }
    }
  }
  if (on) { phone::logNotify(out, on); sent += on; }
  f.close();
  g_streamingPath[0] = 0;
  Serial.printf("[CYD] BLE filtered sent %u/%u bytes (%u lines) of %s\n", (unsigned)sent, (unsigned)total,
                (unsigned)nlines, path);
}

// "L": the current session = the stream matching the current screen.
static void transferLog() { streamFileOverBle(activeStream().path); }

// "T:<mode>[:<minutes>][:<path>]": mode 0 = whole file ("since boot"), 1 = past 24 h, 2 = past
// <minutes>. Path optional (restricted to /logs/); default = the current session.
static void transferFiltered(int mode, int minutes, const char* pathIn) {
  char path[48];
  if (pathIn && pathIn[0]) {
    if (strncmp(pathIn, "/logs/", 6) != 0 || strstr(pathIn, "..")) {
      phone::logNotify((const uint8_t*)"SIZE=0", 6);
      Serial.printf("[CYD] BLE export: rejected path %s\n", pathIn);
      return;
    }
    snprintf(path, sizeof(path), "%s", pathIn);
  } else {
    snprintf(path, sizeof(path), "%s", activeStream().path);
  }
  if (mode != 1 && mode != 2) { streamFileOverBle(path); return; }
  logfilter::Cutoff c = logfilter::make(mode, minutes < 1 ? 1 : (uint32_t)minutes, phone::epochNow(), millis());
  streamFilteredOverBle(path, c);
}

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
static void wipeAllLogs();      // fwd decl ("D:*" bulk delete)
static void deleteSession(const char* path) {
  if (strcmp(path, "*") == 0 || strcmp(path, "/logs/*") == 0) {  // "D:*" = wipe all sessions
    wipeAllLogs();
    sendSessionList();
    return;
  }
  if (strncmp(path, "/logs/", 6) != 0 || strstr(path, "..")) {
    Serial.printf("[CYD] delete rejected (bad path) %s\n", path);
  } else if (strcmp(path, g_fg.path) == 0 || strcmp(path, g_bg.path) == 0) {
    Serial.println("[CYD] delete refused: that's a live session");
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

// Bulk-delete every /logs/ session except the live one and any file the Scan Viewer holds
// open (same guards as deleteSession). Two-pass per batch (collect names, then remove) since
// deleting while iterating a directory is unsafe; re-scans until a pass finds nothing left to
// delete, bounded so a huge card can't spin forever.
static void wipeAllLogs() {
  int removed = 0;
  for (int pass = 0; pass < 64; pass++) {       // up to 64 * 24 files
    File dir = SD.open("/logs");
    if (!dir) break;
    char victims[24][48];
    int nv = 0;
    for (File e = dir.openNextFile(); e && nv < 24; e = dir.openNextFile()) {
      if (!e.isDirectory()) {
        char full[48];
        snprintf(full, sizeof(full), "/logs/%s", logBase(e.name()).c_str());
        if (strcmp(full, g_fg.path) != 0 && strcmp(full, g_bg.path) != 0 && !exploreHolds(full))
          snprintf(victims[nv++], sizeof(victims[0]), "%s", full);
      }
      e.close();
    }
    dir.close();
    if (nv == 0) break;                          // only the live/held file(s) remain
    for (int i = 0; i < nv; i++)
      if (SD.remove(victims[i])) { removed++; Serial.printf("[CYD] wiped %s\n", victims[i]); }
  }
  Serial.printf("[CYD] wipe all logs: %d file(s) removed (live session kept)\n", removed);
}

// --- 512 MB /logs/ cap with oldest-first auto-rotation. Sums every /logs/ file and, while over the
// cap, deletes the OLDEST (by filesystem last-write time), never touching the live fg/bg files, a
// file the Explore viewer holds open, or one being streamed. Run only at low frequency (boot, each
// session rotation, a ~10 min loop() timer) -- never per scan cycle. Bounded per call. ---
static constexpr uint64_t LOG_CAP_BYTES = 512ULL * 1024 * 1024;
static bool logProtected(const char* full) {
  return strcmp(full, g_fg.path) == 0 || strcmp(full, g_bg.path) == 0 || exploreHolds(full) ||
         (g_streamingPath[0] && strcmp(full, g_streamingPath) == 0);
}
static void enforceLogCap() {
  if (!g_sdOk) return;
  int removed = 0;
  for (int pass = 0; pass < 64; pass++) {
    File dir = SD.open("/logs");
    if (!dir) return;
    uint64_t total = 0;
    time_t oldT = 0;
    char oldest[48] = "";
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (!e.isDirectory()) {
        total += (uint64_t)e.size();
        char full[48];
        snprintf(full, sizeof(full), "/logs/%s", logBase(e.name()).c_str());
        time_t t = e.getLastWrite();
        if (!logProtected(full) && (!oldest[0] || t < oldT)) { oldT = t; memcpy(oldest, full, sizeof(oldest)); }
      }
      e.close();
    }
    dir.close();
    if (total <= LOG_CAP_BYTES || !oldest[0]) break;
    if (!SD.remove(oldest)) break;
    removed++;
    Serial.printf("[CYD] log cap: removed oldest %s (was %llu MB total)\n", oldest, (unsigned long long)(total >> 20));
  }
  if (removed) Serial.printf("[CYD] log cap: %d file(s) removed\n", removed);
}

// Send the list of session logs to the phone: a "SESS=<n>" header then <n> bytes of
// "<path>\t<size>\t<flag>\n" lines. <flag> marks the newest file: "current" while a scan
// is running (its file is being written), else "latest"; blank for all other rows. The web
// app parses it into a picker and labels/floats the flagged row.
static void sendSessionList() {
  // The newest file is the current session when its file exists this boot; otherwise the
  // most recently written prior file (by FS timestamp).
  const LogStream& cs = activeStream();   // the stream "current"/"L" refers to right now
  String curBase = logBase(String(cs.path));
  String newestBase;
  if (cs.started) {
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
  const char* flagWord = (cs.started && (g_screen == SCR_SCAN || (g_bgScan && g_scanActive))) ? "current" : "latest";

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

// Send the active whitelist rules to the phone, same framing as the session list: a
// "WL=<bytes>" header then <bytes> bytes of "<csv rule line>\n" lines (line order = the
// index "E:<n>" removes).
static void sendWhitelist() {
  String list;
  whitelist::listAll(list);
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "WL=%u", (unsigned)list.length());
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
  Serial.printf("[CYD] whitelist sent (%d rules, %u bytes)\n", whitelist::count(), (unsigned)list.length());
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

// One scan command -> full response, read atomically. The CYD⇄C5 link is STRICTLY synchronous:
// we send ONE StartScan and must read the C5's entire burst through its closing Status(scanning=0)
// before the next cycle can send anything, or the C5 drops the next command (it flushes its RX after
// streaming) and the boards desync. So this does NOT abort mid-transaction on user input -- the C5 is
// a fast non-blocking dumper (~hundreds of ms), so a tap is serviced on the next loop iteration with
// no perceptible lag.
//
// Returns true if a response was received (full burst or at least a Status), false on a MISS. A miss
// is detected fast: the C5 answers within ~12 ms when free, so if NOTHING has arrived by noRespMs we
// bail (the C5 is in its Wi-Fi-scan busy window) instead of burning the whole timeoutMs. Once bytes
// start, we read the full burst up to timeoutMs (which must exceed a worst-case dense-table dump).
// On a miss nothing is transmitted during the window (verified on-wire), so there is no late burst to
// desync the next poll. This does NOT touch g_linkOk -- the caller owns link state (sticky).
static bool requestScan(uint32_t noRespMs = 800, uint32_t timeoutMs = 5000) {
  g_detCount = 0;
  bool sawStart = false, done = false, anyByte = false;
  uint8_t peer[6]; bool havePeer = phone::peerMac(peer);  // connected phone (dynamic)
  while (LinkSerial.available()) LinkSerial.read();
  parser.reset();
  ScanConfig cfg{};
  cfg.sources  = g_srcMask;
  cfg.dwell_ms = 0;
  sendFrame((uint8_t)Command::StartScan, &cfg, sizeof(cfg));
  uint32_t t0 = millis();
  uint32_t lastByte = millis();
  while (!done && millis() - t0 < timeoutMs) {
    if (!anyByte && millis() - t0 > noRespMs) break;  // no reply at all -> busy miss, bail fast
    bool got = false;
    while (LinkSerial.available()) {
      got = true; anyByte = true; lastByte = millis();
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
        g_deauthRecent = st->deauth_recent; g_evilCount = st->evil_count; g_ridCount = st->rid_count;
        if (st->scanning != 0) sawStart = true;
        else                   done     = true;
      }
    }
    // Idle-gap backstop: burst started but the closing Status was lost -> don't wait the full timeout.
    if ((sawStart || g_detCount > 0) && millis() - lastByte > 250) done = true;
    if (!got) delay(1);  // yield instead of a tight spin
  }
  return done || sawStart || g_detCount > 0;
}

// Session basename: the stream's path without the "/logs/" dir and ".jsonl" extension.
static void sessBase(const LogStream& st, char* out, size_t cap) {
  const char* b = strrchr(st.path, '/');
  b = b ? b + 1 : st.path;
  strncpy(out, b, cap - 1); out[cap - 1] = 0;
  char* dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

static void pushStatus() {
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  char sb[32]; sessBase(activeStream(), sb, sizeof(sb));  // sess= tracks the stream "current" downloads
  char s[320];
  snprintf(s, sizeof(s),
           "link=%d;w24=%d;w5=%d;ble=%d;prb=%d;z=%d;uniq=%d;time=%d;gps=%d;dl=%d;"
           "susp=%d;lk=%d;conf=%d;db=%d;scan=%d;bri=%d;src=%d;wl=%d;muted=%d;fol=%d;sess=%s;bg=%d;deauth=%d;evil=%d;rid=%d",
           g_linkOk ? 1 : 0, n24, n5, nble, nprb, n154, activeStream().seenCount,
           phone::hasTime() ? 1 : 0, phone::hasGps() ? 1 : 0,
           webshare::active() ? 1 : 0, susp, lk, conf, sigdb::loaded() ? 1 : 0,
           g_scanActive ? 1 : 0, g_brightness, (int)g_srcMask,
           whitelist::count(), countMuted(), g_followN, sb, g_bgScan ? 1 : 0,
           (int)g_deauthRecent, (int)g_evilCount, (int)g_ridCount);
  phone::setStatus(String(s));
}

// Stream the live detection list to the phone so its app mirrors the CYD screen.
// DETS stream v4 = "seq-tagged atomic snapshot": a "D:<seq>:<groups>" header, then one
// row per device (one MAC, merged across sources), top-of-list first:
//   <seq>\t<tier>\t<mac>\t<bestRssi>\t<ie>\t<name>\t<tag:rssi,tag:rssi,...>\t<ftier>\t<fscore>\t<muted>
// where <ftier> is the follow tier (0 none / 1 PERSISTENT / 2 FOLLOWING), <fscore> the
// 0..100 follow score, and <muted> is 1 when the device matches a whitelist rule. These
// three trailing fields were appended in v3; older web parsers that read by fixed index
// (fields 0..6) ignore them, so the change is backward-compatible. v4 appends ONE more trailing
// field, <flags>: the decimal DetFlags bits (FLAGS_NOTABLE only: iBeacon 8 / Find My 16 /
// Pwnagotchi 32 / Evil Twin 64 / Drone RID 128) OR'd across the device's members, 0 if none.
// Older parsers read fields 0..9 and ignore it.
// The app drops rows whose seq != the current header's and swaps the list in only when
// the snapshot is complete. Fallbacks considered and held in reserve if this proves
// lossy: (a) a length-prefixed blob like the log download, (b) a polled READ
// characteristic. Capped so the burst stays small on the BLE link.
static constexpr int DETS_STREAM_MAX = 12;
static void pushDetections() {
  if (!phone::connected()) return;
  if (!g_scanActive) return;  // only stream while scanning
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
    len += snprintf(row + len, sizeof(row) - len, "%s", g.srcs);  // list pre-built (de-duped) in buildGroups
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    // v3 trailing fields: follow tier / follow score / muted (whitelisted) — all CYD-computed.
    const FollowState* f = findFollow(g.mac);
    len += snprintf(row + len, sizeof(row) - len, "\t%d\t%d\t%d",
                    f ? f->ftier : 0, f ? f->score : 0, g.whitelisted ? 1 : 0);
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    snprintf(row + len, sizeof(row) - len, "\t%u", (unsigned)g.flags);  // v4 trailing: notable flag bits
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
                       const char* trailing, bool flag = false, int tagW = 9, bool follow = false,
                       bool muted = false) {
  char buf[56];
  // col 0 = '!' sigdb threat, col 1 = '>' following (orthogonal axes, so they compose).
  // A whitelisted (muted) device shows '~' and is dimmed; it never shows the threat/follow glyphs.
  char c0 = muted ? '~' : (flag ? '!' : ' ');
  char c1 = (muted || !follow) ? ' ' : '>';
  int n = snprintf(buf, sizeof(buf), "%c%c%-*.*s %-13.13s %4d", c0, c1,
                   tagW, tagW, tag, name, rssi);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(muted ? TFT_DARKGREY : color, TFT_BLACK);
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
// flicker. Rows come from the filtered view g_scanView (built by buildScanView): indices into
// the live g_groups, or into the frozen snapshot g_frozenRows while the view is paused. One row
// per MAC, tier first then strongest RSSI.
static void drawScanList(bool clear) {
  int W = tft.width();
  if (clear) tft.fillRect(0, LIST_Y0, W, tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  int maxOff = max(0, g_scanViewN - vis);
  g_scrollOffset = constrain(g_scrollOffset, 0, maxOff);  // self-corrects when the list shrinks
  int rows = min(g_scanViewN - g_scrollOffset, vis);
  for (int r = 0; r < rows; r++) {
    int idx = g_scanView[g_scrollOffset + r];
    int y = LIST_Y0 + r * LIST_ROW_H;
    if (g_viewFrozen) {
      const ScanRow& s = g_frozenRows[idx];
      const FollowState* fs = findFollow(s.mac);
      drawDetRow(y, tierColor(s.tier, s.source, s.channel), s.tag, s.name, s.rssi, "",
                 s.tier != (int)sigdb::Tier::None, 9, fs && fs->ftier == 2, s.whitelisted);
    } else {
      const DevGroup& g = g_groups[idx];
      const Detection& d = g_dets[g.rep];
      const FollowState* fs = findFollow(g.mac);
      drawDetRow(y, tierColor(g.tier, d.source, d.channel), g.tag,
                 g.nameIdx >= 0 ? g_dets[g.nameIdx].name : "<hidden>", g.bestRssi, "",
                 g.tier != (int)sigdb::Tier::None, 9, fs && fs->ftier == 2, g.whitelisted);
    }
  }
  drawScrollIcons(g_scrollOffset, g_scanViewN, vis);
}

// Touch scrolling + row tap for the scan list. Returns the tapped view position, else -1.
// Call frequently while on SCR_SCAN.
static int handleScanTouch() {
  return listTouch(g_scanViewN, &g_scrollOffset, visibleRows(), LIST_Y0, LIST_ROW_H,
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

static void drawBodyButtonN(int i, int n, const char* label, uint16_t fill, bool sel);  // fwd decl

// Body buttons (y76..101): START/STOP the scan · PAUSE/RESUME the list view · list FILTER · NEW session.
static void drawScanButtons() {
  drawBodyButtonN(0, 4, g_scanActive ? "STOP" : "START", g_scanActive ? TFT_MAROON : TFT_DARKGREEN, false);
  drawBodyButtonN(1, 4, g_viewFrozen ? "RESUME" : "PAUSE", g_viewFrozen ? TFT_BLUE : TFT_NAVY, false);
  drawBodyButtonN(2, 4, "FILTER", TFT_NAVY, g_lvType != 0x1F || g_lvThreat);
  drawBodyButtonN(3, 4, "NEW SES", TFT_NAVY, false);
}

// Info band (y44..BANNER_Y+BANNER_H): session + unique, per-band counts, scan state + tier counts.
// Clears only its own band so it can be refreshed every cycle without a full-screen repaint.
static void drawScanInfo() {
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  tft.fillRect(0, 44, tft.width(), (BANNER_Y + BANNER_H) - 44, TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  char buf[48];
  // Line 1 (y44): active session name (left) + unique-device count (right).
  char sb[32]; sessBase(g_fg, sb, sizeof(sb));  // the scanner screen always logs to the fg stream
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(buf, sizeof(buf), "Sess: %s", sb);
  tft.drawString(buf, 4, 44, 1);
  snprintf(buf, sizeof(buf), "U:%d", g_fg.seenCount);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(buf, tft.width() - 4, 44, 1);
  tft.setTextDatum(TL_DATUM);

  // Line 2 (y54): per-band counts.
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  snprintf(buf, sizeof(buf), "2.4:%d 5G:%d BLE:%d PRB:%d Z:%d", n24, n5, nble, nprb, n154);
  tft.drawString(buf, 4, 54, 1);

  // State band (y64..75, BANNER_Y/H): a tappable magenta follow alert when devices are flagged
  // (opens the drill-down), else one-shot feedback, else the scan state + tier counts. The
  // scan/pause state is also shown by the buttons. Plain '!' (the TFT font has no warning glyph).
  bool evA = eventAlertActive();
  if (g_followN > 0 || evA) {
    // Red deauth / evil-twin alert (full width alone, right half when stacked with the follow
    // banner, which keeps the left half and stays tappable anywhere on the band).
    int W = tft.width(), fw = evA ? W / 2 : W;
    tft.setTextDatum(MC_DATUM);
    if (g_followN > 0) {
      tft.fillRect(0, BANNER_Y, fw, BANNER_H, TFT_MAGENTA);
      tft.setTextColor(TFT_WHITE, TFT_MAGENTA);
      snprintf(buf, sizeof(buf), evA ? "! FOLLOW: %d" : "! FOLLOWING: %d  (tap)", g_followN);
      tft.drawString(buf, fw / 2, BANNER_Y + BANNER_H / 2 + 1, 1);
    }
    if (evA) {
      int x0 = (g_followN > 0) ? fw : 0;
      tft.fillRect(x0, BANNER_Y, W - x0, BANNER_H, TFT_RED);
      tft.setTextColor(TFT_WHITE, TFT_RED);
      char a[32];
      int an = snprintf(a, sizeof(a), "! ");
      if (deauthAlertActive()) an += snprintf(a + an, sizeof(a) - an, "DEAUTH:%d ", (int)g_deauthShown);
      if (evilAlertActive())   an += snprintf(a + an, sizeof(a) - an, "EVIL:%d", (int)g_evilShown);
      tft.drawString(a, x0 + (W - x0) / 2, BANNER_Y + BANNER_H / 2 + 1, 1);
    }
    tft.setTextDatum(TL_DATUM);
    g_scanMsg = nullptr;  // discard any pending one-shot while an alert is up
  } else if (g_scanMsg) {  // one-shot feedback (e.g. whitelist add / new session), once
    tft.setTextColor(g_scanMsgCol, TFT_BLACK);
    tft.drawString(g_scanMsg, 4, BANNER_Y + 1, 1);
    g_scanMsg = nullptr;
  } else {
    const char* st = g_viewFrozen ? "VIEW PAUSED" : g_scanActive ? "SCANNING" : "STOPPED";
    uint16_t scol = g_viewFrozen ? TFT_CYAN : g_scanActive ? TFT_WHITE : TFT_DARKGREY;
    int x = 4;
    tft.setTextColor(scol, TFT_BLACK);
    tft.drawString(st, x, BANNER_Y + 1, 1);
    x += 6 * (int)strlen(st) + 6;  // font 1 is fixed-width (6 px/char)
    const int     tc[3] = { susp, lk, conf };
    const char    tp[3] = { 'S', 'L', 'C' };
    const uint16_t tcol[3] = { TFT_YELLOW, TFT_ORANGE, TFT_RED };
    for (int i = 0; i < 3; i++) {
      snprintf(buf, sizeof(buf), "%c:%d", tp[i], tc[i]);
      tft.setTextColor(tc[i] > 0 ? tcol[i] : TFT_DARKGREY, TFT_BLACK);
      tft.drawString(buf, x, BANNER_Y + 1, 1);
      x += 6 * (int)strlen(buf) + 4;
    }
  }
}

// Per-cycle refresh: no fillScreen, no top-bar/button repaint.
static void updateScan() {
  drawStatusBar();
  drawScanInfo();
  drawScanList(true);
}

// Full redraw for screen entry / state changes.
static void render() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();

  // Full-width back button across the top: tap to leave the scanner (asks first while a scan
  // is running; a long BOOT hold does the same).
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);
  drawScanButtons();
  drawScanInfo();
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
  drawStatusBar();
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);  // full-width back (app convention)
  int W = tft.width();
  tft.setTextDatum(MC_DATUM);
  int ty = drawCenteredQr(APP_URL, 56) + 14;  // below the back bar (y24..42); +8px margin (QR quiet zone starts at qy-6)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan to open the app", W / 2, ty, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(g_touchOk ? "Tap BACK or press button" : "Press button: back",
                 W / 2, ty + 20, 2);
  tft.setTextDatum(TL_DATUM);
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
  // Item 0 reflects the live phone-link state ("Connected" once a phone is on the GATT link);
  // items 1/2 reuse the shared labels so Scan/Settings stay a single source of truth.
  const char* items[MENU_N] = { phone::connected() ? "Connected" : kMenuItems[0],
                                kMenuItems[1], kMenuItems[2] };
  drawListMenu("HOME", items, MENU_N, g_menuSel);
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

// Scan sub-menu: Start Scan / New Session / Explore Scan / Scan Settings / Back.
static void drawScanMenu() {
  drawListMenu("SCAN", kScanItems, SCAN_N, g_scanSel);
  char sb[32], line[48]; sessBase(g_fg, sb, sizeof(sb));
  snprintf(line, sizeof(line), "Session: %s", sb);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(line, tft.width() / 2, 60, 1);  // between the title (ends ~58) and first button (75)
  tft.setTextDatum(TL_DATUM);
  tft.drawString(g_touchOk ? "Tap an item, or BOOT: tap=next hold=select"
                           : "BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// Scan Settings: a row of 4 source tiles (filled = enabled) then Whitelist / Back rows.
// Focus index g_scanSetSel: 0..3 = tiles (BLE / 2.4 / 5G / 15.4), 4 = Background scan, 5 = Whitelist, 6 = Back.
static int ssTileW() { return (tft.width() - 2 * SS_MARGIN - 3 * SS_TILE_GAP) / 4; }
static void drawScanSettings() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  int W = tft.width();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("SCAN SETTINGS", W / 2, 32, 4);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Sources", SS_MARGIN, SS_TILE_Y - 12, 1);
  static const char* const kTileLbl[4] = { "BLE", "2.4", "5G", "15.4" };
  const uint8_t tileBits[4] = { MASK_BLE, MASK_WIFI24, MASK_WIFI5, MASK_154 };
  int tw = ssTileW();
  for (int i = 0; i < 4; i++) {
    int x = SS_MARGIN + i * (tw + SS_TILE_GAP);
    bool on = (g_srcMask & tileBits[i]) != 0;
    if (on) tft.fillRoundRect(x, SS_TILE_Y, tw, SS_TILE_H, 6, TFT_DARKGREEN);
    else    tft.drawRoundRect(x, SS_TILE_Y, tw, SS_TILE_H, 6, TFT_DARKGREY);
    if (g_scanSetSel == i) tft.drawRect(x - 2, SS_TILE_Y - 2, tw + 4, SS_TILE_H + 4, TFT_CYAN);  // focus
    tft.setTextColor(on ? TFT_WHITE : TFT_DARKGREY, on ? TFT_DARKGREEN : TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(kTileLbl[i], x + tw / 2, SS_TILE_Y + SS_TILE_H / 2, 2);
  }
  char bgLbl[24];
  snprintf(bgLbl, sizeof(bgLbl), "Background: %s", g_bgScan ? "ON" : "OFF");
  const char* const kRowLbl[3] = { bgLbl, "Whitelist", "Back" };
  for (int i = 0; i < 3; i++) {
    bool s = (g_scanSetSel == 4 + i);
    int  y = SS_ROW_Y0 + i * SS_ROW_STEP;
    if (s) tft.fillRoundRect(6, y - 5, W - 12, SS_ROW_H, 6, TFT_NAVY);
    else   tft.drawRoundRect(6, y - 5, W - 12, SS_ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(s ? TFT_WHITE : TFT_LIGHTGREY, s ? TFT_NAVY : TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(kRowLbl[i], W / 2, y - 5 + SS_ROW_H / 2, 4);
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(g_touchOk ? "Tap an item, or BOOT: tap=next hold=select"
                           : "BOOT: tap=next  hold=select", 10, tft.height() - 18, 1);
}

// Touch hit-test for the Scan Settings mixed geometry (tiles + rows). On a fresh touch-down
// edge returns the focus index 0..6, else -1. No-op until touch is calibrated.
static int scanSetTouch() {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return -1; }
  bool now = g_touch.touched();
  int  hit = -1;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z)) {
      int tw = ssTileW();
      if (sy >= SS_TILE_Y - 2 && sy <= SS_TILE_Y + SS_TILE_H + 2)
        for (int i = 0; i < 4; i++) {
          int x = SS_MARGIN + i * (tw + SS_TILE_GAP);
          if (sx >= x - 2 && sx <= x + tw + 2) { hit = i; break; }
        }
      for (int i = 0; i < 3 && hit < 0; i++) {
        int y = SS_ROW_Y0 + i * SS_ROW_STEP;
        if (sx >= 6 && sx <= tft.width() - 6 && sy >= y - 5 && sy <= y - 5 + SS_ROW_H) hit = 4 + i;
      }
    }
  }
  prev = now;
  return hit;
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
  bool     wl;  // device was whitelisted (muted) when logged; absent in old logs -> false
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
  r.wl      = (o["wl"] | 0) != 0;  // omitted in older logs -> not whitelisted
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
  r.wl = false;  // legacy CSV logs predate the whitelist flag
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

// ---- Scanner view (SCR_SCAN): filtered index list over the live groups or the frozen snapshot ----
// Rebuild g_scanView from the live g_groups (or g_frozenRows while paused) through the live
// source/threat filter (g_lvType / g_lvThreat). Whitelisted devices never satisfy "Threats".
static void buildScanView() {
  g_scanViewN = 0;
  if (g_viewFrozen) {
    for (int k = 0; k < g_frozenCount && g_scanViewN < MAX_DET; k++) {
      const ScanRow& s = g_frozenRows[k];
      if ((g_lvType & s.srcMask) && (!g_lvThreat || (s.tier && !s.whitelisted)))
        g_scanView[g_scanViewN++] = (uint8_t)k;
    }
  } else {
    for (int k = 0; k < g_groupCount && g_scanViewN < MAX_DET; k++) {
      const DevGroup& g = g_groups[k];
      if ((g_lvType & g.srcMask) && (!g_lvThreat || (g.tier && !g.whitelisted)))
        g_scanView[g_scanViewN++] = (uint8_t)k;
    }
  }
  g_scrollOffset = constrain(g_scrollOffset, 0, max(0, g_scanViewN - visibleRows()));
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

// ---- Body action buttons: tall buttons in the band just above the list (LIST_Y0) ----
// App screens put their primary actions here instead of in the top bar (full-width "< BACK").
// One button (whitelist +ADD) or several side by side (viewer Filter/Sort), split into n equal
// slots across [6 .. width-6]. The hit-test shares listTouch's arm state so the tap that
// opened the screen can't leak in; one call per loop iteration (like topBarSegTapped).
// (BODY_BTN_Y / BODY_BTN_H are declared up with the other layout constants.)
static void drawBodyButtonN(int i, int n, const char* label, uint16_t fill, bool sel) {
  int W = tft.width(), span = W - 12, pad = (n > 1) ? 3 : 0;
  int x = 6 + span * i / n + (i > 0 ? pad : 0);
  int w = (6 + span * (i + 1) / n) - x - (i < n - 1 ? pad : 0);
  tft.fillRoundRect(x, BODY_BTN_Y, w, BODY_BTN_H, 6, fill);
  tft.drawRoundRect(x, BODY_BTN_Y, w, BODY_BTN_H, 6, sel ? TFT_CYAN : fill);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawString(label, x + w / 2, BODY_BTN_Y + BODY_BTN_H / 2, w >= 64 ? 2 : 1);  // font 1 for narrow (4+) buttons
  tft.setTextDatum(TL_DATUM);
}
// Freshly-tapped slot (0..n-1) in the n-button body band, else -1.
static int bodyButtonTapped(int n) {
  static bool prev = false;
  if (!g_touchOk || !g_ltArmed) { prev = false; return -1; }
  bool now = g_touch.touched();
  int hit = -1;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z) && sy >= BODY_BTN_Y && sy <= BODY_BTN_Y + BODY_BTN_H &&
        sx >= 6 && sx <= tft.width() - 6)
      hit = min(n - 1, (sx - 6) * n / (tft.width() - 12));
  }
  prev = now;
  return hit;
}

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
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);  // full-width back (app convention)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(g_viewName, 4, 44, 1);
  char b[48];
  snprintf(b, sizeof(b), g_idxTrunc ? "showing %d of %d (first %d)" : "showing %d of %d",
           g_viewN, g_idxN, MAX_LOG_ROWS);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(b, 4, 54, 1);
  drawBodyButtonN(0, 2, "Filter", TFT_NAVY, false);   // two body actions, right above the list
  drawBodyButtonN(1, 2, "Sort",   TFT_NAVY, false);
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
  if (r.wl) addField("Whitelisted", "yes (muted)", TFT_DARKGREY);
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

// ---- Follow drill-down: banner -> flagged list -> action menu -> details ----
// Entered from the scan-screen banner. A stable snapshot of the flagged g_follow indexes is
// taken on entry (no scan cycle runs while drilling, so g_follow/g_groups stay put); returning
// to SCR_SCAN resumes the live view.
static uint8_t g_flIdx[MAX_DET];  // snapshot: g_follow indexes with ftier >= 1 (FOLLOWING first, then score)
static int     g_flN = 0, g_flSel = 0, g_flOff = 0, g_flActSel = 0;
static const char* g_flMsg = nullptr;  // brief list-header confirmation (cleared on entry from scan)
static uint16_t    g_flMsgCol = TFT_GREEN;
static const char* const kFollowActItems[] = { "See details", "Add to whitelist", "Back" };
static constexpr int FOLLOWACT_N = 3;

static const DevGroup* groupByMac(const uint8_t* mac) {
  for (int k = 0; k < g_groupCount; k++)
    if (memcmp(g_groups[k].mac, mac, 6) == 0) return &g_groups[k];
  return nullptr;
}

static const char* groupName(const DevGroup* g) {
  return (g && g->nameIdx >= 0) ? g_dets[g->nameIdx].name : "<hidden>";
}

static void drawFollowRows(bool clear) {
  if (clear) tft.fillRect(0, LIST_Y0, tft.width(), tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  g_flOff = constrain(g_flOff, 0, max(0, g_flN - vis));
  int rows = min(g_flN - g_flOff, vis);
  for (int r = 0; r < rows; r++) {
    int i = g_flOff + r;
    const FollowState& f = g_follow[g_flIdx[i]];
    const DevGroup* g = groupByMac(f.mac);
    int y = LIST_Y0 + r * LIST_ROW_H;
    if (g) {
      const Detection& d = g_dets[g->rep];
      drawDetRow(y, tierColor(g->tier, d.source, d.channel), g->tag, groupName(g), g->bestRssi,
                 f.ftier == 2 ? "F" : "P", g->tier != (int)sigdb::Tier::None, 9, true, false);
    } else {
      drawDetRow(y, TFT_DARKGREY, "?", "<gone>", f.rssiMax, "", false, 9, true, false);
    }
    if (i == g_flSel) tft.drawRect(0, y - 2, ICON_GUTTER_X, LIST_ROW_H, TFT_CYAN);  // BOOT-nav highlight
  }
  drawScrollIcons(g_flOff, g_flN, vis);
}

static void drawFollowList() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  char b[40];
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(b, sizeof(b), "FLAGGED DEVICES: %d", g_flN);
  tft.drawString(b, 4, 46, 1);
  if (g_flMsg) tft.setTextColor(g_flMsgCol, TFT_BLACK); else tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(g_flMsg ? g_flMsg : "F=following P=persistent; tap a row", 4, 58, 1);
  drawFollowRows(false);
}

// Snapshot the flagged devices and open the list (from the scan banner).
static void openFollowList() {
  g_flN = 0;
  for (int i = 0; i < g_followCount && g_flN < MAX_DET; i++)
    if (g_follow[i].ftier >= 1) g_flIdx[g_flN++] = (uint8_t)i;
  std::sort(g_flIdx, g_flIdx + g_flN, [](uint8_t a, uint8_t b) {
    if (g_follow[a].ftier != g_follow[b].ftier) return g_follow[a].ftier > g_follow[b].ftier;
    return g_follow[a].score > g_follow[b].score;
  });
  g_flSel = 0; g_flOff = 0; g_flMsg = nullptr;
  g_screen = SCR_FOLLOWLIST;
  listTouchReset();
  drawFollowList();
}

static void followBackToList() {
  g_screen = SCR_FOLLOWLIST;
  listTouchReset();
  drawFollowList();
}

static void showScan();  // fwd decl (defined after the follow drill-down)
static void followBackToScan() {
  showScan();   // the SCAN case no longer repaints on entry, so showScan() does it
}

static void drawFollowAction() {
  const DevGroup* g = groupByMac(g_follow[g_flIdx[g_flSel]].mac);
  char title[16];
  snprintf(title, sizeof(title), "%.14s", groupName(g));
  drawListMenu(title, kFollowActItems, FOLLOWACT_N, g_flActSel);
}

static void openFollowAction() {
  g_flActSel = 0;
  g_screen = SCR_FOLLOWACTION;
  listTouchReset();
  drawFollowAction();
}

// Fill g_detL[] from the live FollowState + its DevGroup/Detections (mirrors openDetail).
static void openFollowDetail() {
  const FollowState& f = g_follow[g_flIdx[g_flSel]];
  const DevGroup* g = groupByMac(f.mac);
  g_detN = 0; g_detOff = 0;
  char b[64];
  addField("Name", groupName(g));
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5]);
  addField("MAC", b);
  addField("Source", g ? g->tag : "-");
  snprintf(b, sizeof(b), "%d (%d..%d)", g ? g->bestRssi : (int)f.rssiMax, f.rssiMin, f.rssiMax);
  addField("RSSI dBm", b);
  if (g && g_dets[g->rep].channel) { snprintf(b, sizeof(b), "%d", g_dets[g->rep].channel); addField("Channel", b); }
  else addField("Channel", "-");
  // sigdb: strongest-scoring member detection of this MAC.
  int best = -1;
  for (int i = 0; i < g_detCount; i++)
    if (memcmp(g_dets[i].mac, f.mac, 6) == 0 && (best < 0 || g_score[i].score > g_score[best].score)) best = i;
  if (best >= 0) {
    snprintf(b, sizeof(b), "%s (%d)", sigdb::tierName(g_score[best].tier), g_score[best].score);
    addField("Threat tier", b, tierColor((int)g_score[best].tier, (uint8_t)Source::WifiScan, 0));
    const char* lbl = detLabel(best);
    if (lbl && lbl[0]) addField("Signature", lbl);
    char ft[64]; flagTokens(g_dets[best].flags, ft, sizeof(ft));
    if (ft[0]) addField("Flags", ft);
  } else addField("Threat tier", "none");
  snprintf(b, sizeof(b), "%d / 100", f.score);
  addField("Follow score", b);
  addField("Follow tier", f.ftier == 2 ? "FOLLOWING" : "PERSISTENT", f.ftier == 2 ? TFT_RED : TFT_ORANGE);
  snprintf(b, sizeof(b), "%d windows", __builtin_popcount(f.windowMask));
  addField("Persistence", b);
  bool gpsOk = (f.flags & FF_HADGPS) && !isnan(f.anchorLat);
  snprintf(b, sizeof(b), "%d", gpsOk ? f.distinctFixes : 0);
  addField("GPS fixes", b);
  snprintf(b, sizeof(b), "%d m", gpsOk ? (int)f.maxSpanM : 0);
  addField("Max span", b);
  snprintf(b, sizeof(b), "%u", (unsigned)f.sightings);
  addField("Sightings", b);
  uint32_t nowMs = millis();
  for (int k = 0; k < 2; k++) {
    uint32_t ms = k ? f.lastMs : f.firstMs;
    if (phone::hasTime()) {
      time_t t = (time_t)(phone::epochNow() - (nowMs - ms) / 1000);
      struct tm tmv;
      gmtime_r(&t, &tmv);
      snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
               tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    } else {
      snprintf(b, sizeof(b), "+%lus since boot", (unsigned long)(ms / 1000));
    }
    addField(k ? "Last seen" : "First seen", b);
  }
  if (gpsOk) { snprintf(b, sizeof(b), "%.5f,%.5f", f.anchorLat, f.anchorLon); addField("GPS anchor", b); }
  else addField("GPS anchor", "-");

  g_screen = SCR_FOLLOWDETAIL;
  listTouchReset();
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("FOLLOW DETAIL", 4, 46, 1);
  drawDetailList(false);
}

// Compose ONE whitelist rule for the selected device, most durable identifier first:
// name -> BLE company ID -> BLE service UUID -> MAC. Label is always "follow".
// Keyed purely off the MAC (shared by the follow flow and the Whitelist add-picker).
static void whitelistLineForMac(const uint8_t mac[6], char* out, size_t cap) {
  const DevGroup* g = groupByMac(mac);
  uint8_t bits = 0; uint16_t cid = 0; const uint8_t* svc = nullptr;
  for (int i = 0; i < g_detCount; i++) {
    const Detection& d = g_dets[i];
    if (memcmp(d.mac, mac, 6) != 0) continue;
    bits |= (uint8_t)(1u << d.source);
    if (!cid) cid = d.companyId;
    if (!svc) for (int j = 0; j < 16; j++) if (d.svc[j]) { svc = d.svc; break; }
  }
  const uint8_t bleB = 1u << (uint8_t)Source::BleScan;
  const uint8_t wifiB = (1u << (uint8_t)Source::WifiScan) | (1u << (uint8_t)Source::WifiProbe);
  const uint8_t zigB = 1u << (uint8_t)Source::Ieee802154;
  char sm = 'A';  // single radio family -> that family, else any
  if (bits && (bits & ~bleB) == 0) sm = 'B';
  else if (bits && (bits & ~wifiB) == 0) sm = 'W';
  else if (bits && (bits & ~zigB) == 0) sm = '4';

  char nm[24]; nm[0] = 0;
  if (g && g->nameIdx >= 0) {
    int n = 0;
    for (const char* p = g_dets[g->nameIdx].name; *p && n < 23; p++) {
      if (*p == ',' || (unsigned char)*p < 0x20) break;  // rule fields are comma-delimited
      nm[n++] = *p;
    }
    while (n > 0 && nm[n - 1] == ' ') n--;
    nm[n] = 0;
  }
  if (nm[0]) { snprintf(out, cap, "ncontains,%s,%c,follow", nm, sm); return; }
  if (cid)   { snprintf(out, cap, "blecid,%04X,B,follow", cid); return; }
  if (svc) {
    char u[33];
    for (int j = 0; j < 16; j++) sprintf(u + j * 2, "%02X", svc[j]);
    snprintf(out, cap, "bleuuid,%s,B,follow", u);
    return;
  }
  snprintf(out, cap, "mac,%02X:%02X:%02X:%02X:%02X:%02X,%c,follow",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], sm);
}

static void followWhitelistLine(const FollowState& f, char* out, size_t cap) {
  whitelistLineForMac(f.mac, out, cap);
}

static void activateFollowAction(int sel) {
  if (sel == 0) { openFollowDetail(); return; }
  if (sel == 2) { followBackToList(); return; }
  char line[96];
  followWhitelistLine(g_follow[g_flIdx[g_flSel]], line, sizeof(line));
  bool ok = whitelist::add(line);
  Serial.printf("[CYD] follow whitelist add %s: %s\n", ok ? "ok" : "FAILED", line);
  if (ok) {  // device mutes on the next scan cycle; drop it from the snapshot now
    for (int i = g_flSel; i < g_flN - 1; i++) g_flIdx[i] = g_flIdx[i + 1];
    g_flN--;
    g_flSel = constrain(g_flSel, 0, max(0, g_flN - 1));
    if (g_flN == 0) { followBackToScan(); return; }
  }
  g_flMsg = ok ? "Added to whitelist" : "Whitelist add FAILED";
  g_flMsgCol = ok ? TFT_GREEN : TFT_RED;
  followBackToList();
}

// ---- Scanner screen actions (SCR_SCAN + SCR_SCANROW / SCANDETAIL / SCANCONFIRM / SCANFILTER) ----
// SCR_SCAN is a stateful screen: g_scanActive (C5 polling, START/STOP) and g_viewFrozen (PAUSE: the
// list shows a snapshot while scanning continues) are independent of which screen is up. Leaving
// while a scan runs asks first (SCR_SCANCONFIRM). A row tap opens a per-device action menu.
static void showScan() {
  g_screen = SCR_SCAN;
  listTouchReset();
  buildScanView();
  render();
}

// Copy the current groups into the frozen snapshot (bounded, MAX_DET rows).
static void snapshotFrozen() {
  int n = min(g_groupCount, MAX_DET);
  for (int k = 0; k < n; k++) {
    const DevGroup& g = g_groups[k];
    ScanRow& s = g_frozenRows[k];
    memcpy(s.mac, g.mac, 6);
    snprintf(s.name, sizeof(s.name), "%.19s", g.nameIdx >= 0 ? g_dets[g.nameIdx].name : "<hidden>");
    snprintf(s.tag, sizeof(s.tag), "%s", g.tag);
    s.rssi = (int16_t)g.bestRssi;
    s.tier = (uint8_t)g.tier;
    s.whitelisted = g.whitelisted;
    s.source = g_dets[g.rep].source;
    s.channel = g_dets[g.rep].channel;
    s.srcMask = g.srcMask;
  }
  g_frozenCount = n;
}

static void toggleViewFrozen() {
  if (!g_viewFrozen) { snapshotFrozen(); g_viewFrozen = true; }
  else               g_viewFrozen = false;
  g_scrollOffset = 0;
  buildScanView();
  render();
}

// -- stop-scan confirmation (leaving the scanner while it is running) --
static const char* const kConfirmItems[] = { "Stop scan", "Cancel" };
static constexpr int CONFIRM_N = 2;

static void drawScanConfirm() {
  drawListMenu("STOP SCAN?", kConfirmItems, CONFIRM_N, g_confirmSel);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Leaving ends the scan", tft.width() / 2, 60, 1);  // between the title and first button
  tft.setTextDatum(TL_DATUM);
}

static void openScanConfirm() {
  g_confirmSel = 0;
  g_screen = SCR_SCANCONFIRM;
  listTouchReset();
  drawScanConfirm();
}

static void backFromScan() {
  if (g_scanActive && !g_bgScan) { openScanConfirm(); return; }  // background scan keeps running: no confirm
  setLed(!g_linkOk, g_linkOk, false);
  g_screen = SCR_SCANMENU;
  drawScanMenu();
  pushStatus();
}

static void activateScanConfirm(int sel) {
  if (sel == 0) {  // Stop scan -> Scan menu
    g_scanActive = false;
    g_viewFrozen = false;
    setLed(!g_linkOk, g_linkOk, false);
    g_screen = SCR_SCANMENU;
    drawScanMenu();
    pushStatus();
  } else {         // Cancel -> back to the running scanner
    showScan();
  }
}

// -- per-row action menu (See details / Add to whitelist / Back), keyed off a captured MAC --
static void drawScanRow() {
  const DevGroup* g = groupByMac(g_scanRowMac);
  char title[16];
  snprintf(title, sizeof(title), "%.14s", g ? groupName(g) : "<gone>");
  drawListMenu(title, kFollowActItems, FOLLOWACT_N, g_scanRowSel);
}

static void openScanRow(int viewPos) {
  if (viewPos < 0 || viewPos >= g_scanViewN) return;
  int idx = g_scanView[viewPos];
  memcpy(g_scanRowMac, g_viewFrozen ? g_frozenRows[idx].mac : g_groups[idx].mac, 6);
  g_scanRowSel = 0;
  g_screen = SCR_SCANROW;
  listTouchReset();
  drawScanRow();
}

// Live device detail (mirrors openFollowDetail minus the follow-only fields); Back returns to
// the row menu. Built from the live groups/detections by MAC.
static void openScanDetail(const uint8_t* mac) {
  const DevGroup* g = groupByMac(mac);
  g_detN = 0; g_detOff = 0;
  char b[64];
  addField("Name", g ? groupName(g) : "<gone>");
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  addField("MAC", b);
  if (g) {
    addField("Source", g->tag);
    snprintf(b, sizeof(b), "%d dBm", g->bestRssi);
    addField("RSSI", b);
    if (g_dets[g->rep].channel) { snprintf(b, sizeof(b), "%d", g_dets[g->rep].channel); addField("Channel", b); }
    else addField("Channel", "-");
    // sigdb: strongest-scoring member detection of this MAC.
    int best = -1;
    for (int i = 0; i < g_detCount; i++)
      if (memcmp(g_dets[i].mac, mac, 6) == 0 && (best < 0 || g_score[i].score > g_score[best].score)) best = i;
    if (best >= 0) {
      snprintf(b, sizeof(b), "%s (%d)", sigdb::tierName(g_score[best].tier), g_score[best].score);
      addField("Threat tier", b, tierColor((int)g_score[best].tier, (uint8_t)Source::WifiScan, 0));
      const char* lbl = detLabel(best);
      if (lbl && lbl[0]) addField("Signature", lbl);
      if (g->flags) { char ft[64]; flagTokens(g->flags, ft, sizeof(ft)); addField("Flags", ft); }
    } else addField("Threat tier", "none");
    if (g->whitelisted) addField("Whitelisted", "yes (muted)", TFT_DARKGREY);
    uint16_t cid = 0; const uint8_t* svc = nullptr;
    for (int i = 0; i < g_detCount; i++) {
      const Detection& d = g_dets[i];
      if (memcmp(d.mac, mac, 6) != 0) continue;
      if (!cid) cid = d.companyId;
      if (!svc) for (int j = 0; j < 16; j++) if (d.svc[j]) { svc = d.svc; break; }
    }
    if (g->ie) { snprintf(b, sizeof(b), "%08lX", (unsigned long)g->ie); addField("IE fingerprint", b); }
    if (cid)   { snprintf(b, sizeof(b), "%04X", cid); addField("BLE company ID", b); }
    if (svc) {
      char u[33];
      for (int j = 0; j < 16; j++) sprintf(u + j * 2, "%02X", svc[j]);
      addField("Service UUID", u);
    }
  }

  g_screen = SCR_SCANDETAIL;
  listTouchReset();
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("DETAIL", 4, 46, 1);
  drawDetailList(false);
}

static void activateScanRow(int sel) {
  if (sel == 0) { openScanDetail(g_scanRowMac); return; }
  if (sel == 2) { showScan(); return; }
  char line[96];
  whitelistLineForMac(g_scanRowMac, line, sizeof(line));
  bool ok = whitelist::add(line);
  Serial.printf("[CYD] scan whitelist add %s: %s\n", ok ? "ok" : "FAILED", line);
  sendWhitelist(); pushStatus();  // same as the whitelist manager's add: refresh the phone
  if (ok)  // a paused view keeps its snapshot; mute the row now (live rows mute on the next cycle)
    for (int k = 0; k < g_frozenCount; k++)
      if (memcmp(g_frozenRows[k].mac, g_scanRowMac, 6) == 0) g_frozenRows[k].whitelisted = true;
  g_scanMsg = ok ? "Added to whitelist" : "Whitelist add FAILED";
  g_scanMsgCol = ok ? TFT_GREEN : TFT_RED;
  showScan();
}

// -- live on-screen source / threat filter (separate from the Scan Viewer's g_fType/g_fThreat) --
static constexpr int SCANFILTER_N = 7;  // 5 source toggles, Threats, Back
static void drawScanFilter() {
  char l[SCANFILTER_N - 1][24];
  static const char* const kTypes[5] = { "2.4", "5G", "BLE", "PRB", "154" };
  for (int i = 0; i < 5; i++)
    snprintf(l[i], sizeof(l[i]), "%s: %s", kTypes[i], (g_lvType & (1 << i)) ? "On" : "Off");
  snprintf(l[5], sizeof(l[5]), "Threats: %s", g_lvThreat ? "On" : "Off");
  const char* items[SCANFILTER_N] = { l[0], l[1], l[2], l[3], l[4], l[5], "Back" };
  drawScrollMenu("FILTER", items, SCANFILTER_N, g_sfSel, g_sfOff);
}

static void activateScanFilter(int sel) {
  if (sel < 5)       g_lvType ^= (uint8_t)(1 << sel);
  else if (sel == 5) g_lvThreat = !g_lvThreat;
  else { g_scrollOffset = 0; buildScanView(); showScan(); return; }  // Back: apply + return
  drawScanFilter();
}

static void openScanFilter() {
  g_sfSel = 0; g_sfOff = 0;
  g_screen = SCR_SCANFILTER;
  listTouchReset();
  drawScanFilter();
}

// ---- On-device whitelist manager (Scan Settings -> Whitelist) ----
// SCR_WHITELIST: full-width "< BACK" top bar, a tall in-body "+ ADD" button right above the
// rule list, then the rules. BOOT cursor: g_wlSel 0 = ADD, 1..count = rules. SCR_WLADD picks
// a seen device from a snapshot of g_groups and composes a rule with whitelistLineForMac()
// (full-width "< BACK" exits). SCR_WLRULE is the per-rule action menu (Remove / Back).
// Add/remove mirror the phone's GATT handlers: whitelist op -> sendWhitelist -> pushStatus.
static int g_wlSel = 0, g_wlOff = 0;        // whitelist list cursor/scroll
static int g_wlRuleSel = 0;                 // rule index the action screen targets
static int g_wlActSel = 0;                  // action-menu cursor
static const char* g_wlMsg = nullptr; static uint16_t g_wlMsgCol = TFT_GREEN;  // list-header feedback
static uint8_t g_wlPickMac[MAX_DET][6];     // add-picker snapshot of g_groups MACs
static int     g_wlPickN = 0, g_wlPickSel = 0, g_wlPickOff = 0;
static const char* const kWlActItems[] = { "Remove rule", "Back" };
static constexpr int WLACT_N = 2;

static void drawWhitelist();
static void drawWlAdd();
static void openWlRule();
static void wlAddEnter();

// "kind pattern (label)" for the idx-th rule (CSV kind,pattern,srcmask,label; label may hold commas).
static void wlRuleText(int idx, char* out, size_t cap) {
  char buf[96];
  out[0] = 0;
  if (!whitelist::lineAt(idx, buf, sizeof(buf))) return;
  buf[strcspn(buf, "\r\n")] = 0;
  char* pat = strchr(buf, ',');
  if (!pat) { snprintf(out, cap, "%s", buf); return; }
  *pat++ = 0;
  char* sm = strchr(pat, ',');
  const char* lbl = "";
  if (sm) {
    *sm++ = 0;
    char* l = strchr(sm, ',');
    if (l) lbl = l + 1;
  }
  if (lbl[0]) snprintf(out, cap, "%s %s (%s)", buf, pat, lbl);
  else        snprintf(out, cap, "%s %s", buf, pat);
}

static void drawWlTextRow(int y, const char* text, uint16_t color) {
  char t[56];
  int room = (ICON_GUTTER_X - 4) / 6;  // font-1 chars that fit before the scroll icons
  snprintf(t, sizeof(t), "%.*s", room < 55 ? room : 55, text);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(t, 4, y, 1);
}

// The whitelist "+ ADD" uses the shared single body button (g_wlSel 0 = ADD, 1..count = rules).
static void drawWhitelistRows(bool clear) {
  if (clear) tft.fillRect(0, BODY_BTN_Y, tft.width(), tft.height() - BODY_BTN_Y, TFT_BLACK);
  drawBodyButtonN(0, 1, "+ ADD seen device", TFT_DARKGREEN, g_wlSel == 0);
  int vis = visibleRows();
  int n = whitelist::count();  // body list rows are the rules; g_wlSel 1..n map to them
  g_wlOff = constrain(g_wlOff, 0, max(0, n - vis));
  int rows = min(n - g_wlOff, vis);
  for (int r = 0; r < rows; r++) {
    int i = g_wlOff + r;  // rule index
    int y = LIST_Y0 + r * LIST_ROW_H;
    char t[96];
    wlRuleText(i, t, sizeof(t));
    drawWlTextRow(y, t, TFT_WHITE);
    if (g_wlSel == i + 1) tft.drawRect(0, y - 2, ICON_GUTTER_X, LIST_ROW_H, TFT_CYAN);  // BOOT-nav highlight
  }
  if (n == 0) drawWlTextRow(LIST_Y0, "(no rules yet)", TFT_DARKGREY);
  drawScrollIcons(g_wlOff, n, vis);
}

static void drawWhitelist() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);  // full-width back (app convention)
  tft.setTextDatum(TL_DATUM);
  char b[40];
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(b, sizeof(b), "WHITELIST: %d rules", whitelist::count());
  tft.drawString(b, 4, 44, 1);
  if (g_wlMsg) tft.setTextColor(g_wlMsgCol, TFT_BLACK); else tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(g_wlMsg ? g_wlMsg : "tap a rule to remove it", 4, 54, 1);
  drawWhitelistRows(false);
}

static void wlEnter() {
  g_wlSel = 0; g_wlOff = 0; g_wlMsg = nullptr;
  g_screen = SCR_WHITELIST;
  listTouchReset();
  drawWhitelist();
}

static void wlBackToList() {
  g_screen = SCR_WHITELIST;
  listTouchReset();
  drawWhitelist();
}

// A tapped/selected body row is a rule index -> open its Remove/Back action menu.
static void wlActivateRow(int sel) {
  if (sel < 0 || sel >= whitelist::count()) return;
  g_wlRuleSel = sel;
  openWlRule();
}

static void drawWlAddRows(bool clear) {
  if (clear) tft.fillRect(0, LIST_Y0, tft.width(), tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  int total = g_wlPickN;  // devices only (top-bar "< BACK" exits)
  g_wlPickOff = constrain(g_wlPickOff, 0, max(0, total - vis));
  int rows = min(total - g_wlPickOff, vis);
  for (int r = 0; r < rows; r++) {
    int i = g_wlPickOff + r;
    int y = LIST_Y0 + r * LIST_ROW_H;
    const uint8_t* m = g_wlPickMac[i];
    const DevGroup* g = groupByMac(m);
    if (g) {
      const Detection& d = g_dets[g->rep];
      drawDetRow(y, tierColor(g->tier, d.source, d.channel), g->tag, groupName(g), g->bestRssi,
                 "", g->tier != (int)sigdb::Tier::None, 9, false, g->whitelisted);
    } else {
      char mb[16];
      snprintf(mb, sizeof(mb), "%02X:%02X:%02X:%02X", m[2], m[3], m[4], m[5]);
      drawDetRow(y, TFT_DARKGREY, "?", mb, 0, "", false, 9, false, false);
    }
    if (i == g_wlPickSel) tft.drawRect(0, y - 2, ICON_GUTTER_X, LIST_ROW_H, TFT_CYAN);  // BOOT-nav highlight
  }
  drawScrollIcons(g_wlPickOff, total, vis);
}

static void drawWlAdd() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", TFT_NAVY, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  char b[40];
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(b, sizeof(b), "ADD FROM SEEN: %d", g_wlPickN);
  tft.drawString(b, 4, 46, 1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("tap a device to whitelist it", 4, 58, 1);
  if (g_wlPickN == 0) {
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("No devices seen yet -", tft.width() / 2, 130, 2);
    tft.drawString("run a scan first", tft.width() / 2, 150, 2);
    tft.setTextDatum(TL_DATUM);
  }
  drawWlAddRows(false);
}

// Snapshot the last scan's devices and open the picker.
static void wlAddEnter() {
  g_wlPickN = 0;
  for (int k = 0; k < g_groupCount && g_wlPickN < MAX_DET; k++)
    memcpy(g_wlPickMac[g_wlPickN++], g_groups[k].mac, 6);
  g_wlPickSel = 0; g_wlPickOff = 0;
  g_screen = SCR_WLADD;
  listTouchReset();
  drawWlAdd();
}

static void wlAddSelected(int sel) {
  if (sel < 0 || sel >= g_wlPickN) { wlBackToList(); return; }  // Back row
  char line[96];
  whitelistLineForMac(g_wlPickMac[sel], line, sizeof(line));
  bool ok = whitelist::add(line);
  Serial.printf("[CYD] whitelist add %s: %s\n", ok ? "ok" : "FAILED", line);
  sendWhitelist(); pushStatus();
  g_wlMsg = ok ? "Added to whitelist" : "Whitelist add FAILED";
  g_wlMsgCol = ok ? TFT_GREEN : TFT_RED;
  g_wlSel = 0; g_wlOff = 0;
  wlBackToList();
}

static void drawWlRule() {
  drawListMenu("RULE", kWlActItems, WLACT_N, g_wlActSel);
  char t[96];
  wlRuleText(g_wlRuleSel, t, sizeof(t));
  tft.setTextDatum(TC_DATUM);
  tft.drawString(t, tft.width() / 2, 60, 1);  // between the title and first button
  tft.setTextDatum(TL_DATUM);
}

static void openWlRule() {
  g_wlActSel = 0;
  g_screen = SCR_WLRULE;
  listTouchReset();
  drawWlRule();
}

static void activateWlRule(int sel) {
  if (sel == 0) {  // Remove rule
    bool ok = whitelist::removeAt(g_wlRuleSel);
    Serial.printf("[CYD] whitelist remove %d: %s\n", g_wlRuleSel, ok ? "ok" : "FAILED");
    sendWhitelist(); pushStatus();
    g_wlMsg = ok ? "Rule removed" : "Remove FAILED";
    g_wlMsgCol = ok ? TFT_GREEN : TFT_RED;
    g_wlSel = constrain(g_wlSel, 0, whitelist::count());  // 0 = ADD button, 1..count = rules
  } else {
    g_wlMsg = nullptr;
  }
  wlBackToList();
}

// Act on a scan-menu row (touch tap or long-press select).
static void activateScanMenu(int sel) {
  if (sel == 0)      { g_scrollOffset = 0; resetFollow(); if (!g_bgScan) g_scanActive = false; g_viewFrozen = false;
                       g_frozenCount = 0; showScan(); }                                       // Scanner (idle until START)
  else if (sel == 1) {                                                       // New Session
    startNewSession(true);  // manual = foreground stream
    drawScanMenu();
    const char* nm = strrchr(g_fg.path, '/');
    char msg[40];
    snprintf(msg, sizeof(msg), "New session: %s", nm ? nm + 1 : g_fg.path);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString(msg, 10, tft.height() - 32, 1);
    pushStatus();
  }
  else if (sel == 2) { exploreEnter(); }                                     // Explore Scan
  else if (sel == 3) { g_screen = SCR_SCANSETTINGS; g_scanSetSel = 0; drawScanSettings(); }
  else               { g_screen = SCR_MENU; drawMenu(); }                       // Back
}

// Act on a scan-settings row (touch tap or long-press select).
static void activateScanSettings(int sel) {
  if (sel == 0)      g_srcMask ^= MASK_BLE;
  else if (sel == 1) g_srcMask ^= SRC_24_BITS;
  else if (sel == 2) g_srcMask ^= MASK_WIFI5;
  else if (sel == 3) g_srcMask ^= MASK_154;
  else if (sel == 4) {                                                           // Background scan
    g_bgScan = !g_bgScan;
    saveBgScan();
    if (g_bgScan) { openBgSession(); syncSharePath(); g_scanActive = true; g_lastCycleMs = 0; }  // start now; first cycle right away
    else          { g_scanActive = false; g_viewFrozen = false; setLed(!g_linkOk, g_linkOk, false); }
    drawScanSettings();
    pushStatus();
    return;
  }
  else if (sel == 5) { wlEnter(); return; }                                      // Whitelist
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

// Scan-cycle LED (works headless). Event alert (deauth / evil twin) blinks red each cycle, alternating
// with magenta when a follow alert is also up, else with the link colour; follow alone = solid
// magenta; otherwise link green/red.
static void applyScanLed() {
  static bool phase = false;
  phase = !phase;
  if (eventAlertActive()) {
    if (phase) setLed(true, false, false);
    else if (g_followN > 0) setLed(true, false, true);
    else setLed(!g_linkOk, g_linkOk, false);
  } else if (g_followN > 0) setLed(true, false, true);
  else setLed(!g_linkOk, g_linkOk, false);
}

// One scan → score → log → render cycle, also mirrored to the phone app.
static void runScanCycle() {
  if (!requestScan()) {
    // BUSY MISS: the C5 didn't answer (it's in its Wi-Fi-scan busy window). This is not a link
    // failure. Keep the last table/view (requestScan zeroed g_detCount but we skip the pipeline, so
    // g_groups/g_scanView and the on-screen list stay as they were), don't push the empty table to
    // the phone, and keep the link dot/LED green until LINK_STICKY_MS of total silence. Re-poll soon
    // so we catch the C5 the moment it frees up, instead of waiting the full cadence.
    g_linkOk = (millis() - g_lastLinkOkMs < LINK_STICKY_MS);
    if (!g_linkOk) { g_deauthRecent = 0; g_evilCount = 0; g_ridCount = 0; }  // no stale C5 events
    applyScanLed();
    if (g_screen == SCR_SCAN) drawStatusBar();  // link dot + clock only (never touches the list region); headless in background
    g_lastCycleMs = millis() - (SCAN_CYCLE_MS - SCAN_RETRY_MS);  // fire the next poll in ~SCAN_RETRY_MS
    return;
  }
  g_linkOk = true; g_lastLinkOkMs = millis();
  computeScores();
  buildGroups();  // once per cycle; shared by render() and pushDetections()
  updateFollowState();
  computeFollowScores();
  if (!g_viewFrozen) buildScanView();  // a paused view keeps its snapshot while scanning continues
  updateEvents();   // deauth / evil-twin alert hold + rate-limited event log lines
  applyScanLed();   // alert / follow / link LED (works headless)
  if (bgCtx() && g_bg.seenCount >= MAX_SEEN - 32) {
    // The bg stream never ends on its own, and once its dedup set is full new devices would be
    // re-logged every cycle: roll to a fresh bg session (also a cap-rotation trigger).
    beginSessionNamed(g_bg, true);
    enforceLogCap();
  }
  int newCount = logNewDetections();
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  Serial.printf("[CYD] table=%d (2.4:%d 5G:%d BLE:%d PRB:%d 154:%d) new=%d uniq=%d threats S%d/L%d/C%d db=%d\n",
                g_detCount, n24, n5, nble, nprb, n154, newCount, activeStream().seenCount,
                susp, lk, conf, sigdb::loaded());
  if (g_screen == SCR_SCAN)
    updateScan();  // per-cycle partial repaint (status bar + info band + list); chrome persists from entry
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
  loadBgScan();
  g_scanActive = g_bgScan;  // background scan resumes at boot (first cycle runs right away)
  applyBrightness();
  initSD();
  sigdb::begin();  // load /signatures.csv (seeds it if absent) or fall back
  whitelist::begin();  // load /whitelist.csv (seeds a commented header if absent)
  g_touch.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN, TOUCH_IRQ_PIN);
  g_touchOk = g_touch.loadCal();
  phone::begin(DEVICE_NAME);
  g_haveOwnMac = phone::ownMac(g_ownMac);  // for self-detection filtering
  g_linkOk = pingC5();  // check the C5 link so the menu dot is correct before any scan
  if (g_linkOk) g_lastLinkOkMs = millis();  // seed link stickiness for the first scan
  Serial.printf("[CYD] ready (link=%s, touch=%s, bright=%d%%)\n",
                g_linkOk ? "up" : "down", g_touchOk ? "cal" : "uncal", g_brightness);
  drawMenu();      // start on the home menu
}

void loop() {
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
      g_brightness = (uint8_t)constrain(b, 10, 100);
      applyBrightness(); saveBrightness();
    } }
  { uint8_t m; if (phone::srcMaskRequested(&m) && m != 0) {  // web-app source toggles (ignore all-off)
      g_srcMask = m; saveSrcMask();
      if (g_screen == SCR_SCANSETTINGS) drawScanSettings();
      pushStatus();
    } }
  if (phone::newSessionRequested()) { startNewSession(); pushStatus(); }  // web-app "New Session"
  renameSessionOnSync();  // give each live session a date-time filename once time is known (also syncs the share path)
  syncSharePath();        // the screen context (fg/bg) may have changed
  {  // keep the Scan-menu session label live (phone "N", or the time-sync rename)
    static char lastSess[48] = "";
    if (strcmp(lastSess, g_fg.path) != 0) {
      strncpy(lastSess, g_fg.path, sizeof(lastSess) - 1);
      if (g_screen == SCR_SCANMENU) drawScanMenu();
    }
  }

  // Phone-initiated Wi-Fi download overrides the current screen while active.
  if (phone::downloadRequested()) { exploreFree(); runDownload(); return; }
  if (webshare::active()) {  // just left download mode: tear down AP and repaint
    webshare::stop();
    g_scanActive = g_bgScan; g_viewFrozen = false;  // the download interrupted any running scan (bg scan resumes)
    g_screen = SCR_MENU; g_menuSel = 0; drawMenu(); pushStatus();
  }

  // Low-frequency /logs/ cap check (~10 min). Never while a Wi-Fi/phone download is active (both
  // returned above) and not on the SD-reading Explore screens (avoid contending for the card).
  {
    static uint32_t lastCap = 0;
    if (g_sdOk && millis() - lastCap >= 600000UL &&
        g_screen != SCR_PICKLOG && g_screen != SCR_SCANVIEWER && g_screen != SCR_DETAIL &&
        g_screen != SCR_SORT && g_screen != SCR_FILTER) {
      lastCap = millis();
      enforceLogCap();
    }
  }

  // Phone can start/stop the scan remotely (single toggle in the web app). Ignore a start while
  // drilling the follow or whitelist screens so a stray phone toggle can't reset follow state /
  // abandon the snapshot mid-view; scanStartRequested() is consume-on-read, so short-circuiting
  // leaves it pending until we return to SCR_SCAN.
  bool inDrill = (g_screen == SCR_FOLLOWLIST || g_screen == SCR_FOLLOWACTION || g_screen == SCR_FOLLOWDETAIL ||
                  g_screen == SCR_WHITELIST  || g_screen == SCR_WLADD       || g_screen == SCR_WLRULE ||
                  g_screen == SCR_SCANROW    || g_screen == SCR_SCANDETAIL  || g_screen == SCR_SCANCONFIRM ||
                  g_screen == SCR_SCANFILTER);
  if (!inDrill && phone::scanStartRequested()) {
    g_scanActive = true;
    g_lastCycleMs = 0;  // run the first cycle immediately
    if (g_screen != SCR_SCAN) { g_scrollOffset = 0; resetFollow(); g_viewFrozen = false; showScan(); }
    else render();      // already on the scanner: repaint the START->STOP state
    pushStatus();
  }
  // Consume the stop flag unconditionally. Stop ends the scan but stays on the scanner screen
  // (it just goes idle), so the user can review the list or restart.
  if (phone::scanStopRequested()) {
    g_scanActive = false;
    setLed(!g_linkOk, g_linkOk, false);
    if (g_screen == SCR_SCAN) render();
    pushStatus();
  }

  // Explore and live scan are mutually exclusive: drop the index block as soon as we're out.
  if (g_ex && g_screen != SCR_PICKLOG && g_screen != SCR_SCANVIEWER && g_screen != SCR_DETAIL &&
      g_screen != SCR_SORT && g_screen != SCR_FILTER)
    exploreFree();

  // Background scan: keep the data pipeline (poll -> score -> follow -> log -> phone push) running,
  // headless, on any screen. SCR_SCAN runs its own cycle in its case below. Suppressed on the
  // SD-reading Explore screens (the log write would contend for the card) and on the drill-down
  // screens (they hold indices into the live tables, which a cycle would rebuild). webshare/download
  // already returned above.
  // When the C5 link is down, each headless poll burns ~800ms waiting for a reply the C5 won't send,
  // which would make menus feel laggy; back the cadence off until the link is back (SCR_SCAN is unaffected).
  if (g_bgScan && g_scanActive && g_screen != SCR_SCAN && !inDrill &&
      g_screen != SCR_PICKLOG && g_screen != SCR_SCANVIEWER && g_screen != SCR_DETAIL &&
      g_screen != SCR_SORT && g_screen != SCR_FILTER &&
      !webshare::active() &&
      millis() - g_lastCycleMs >= (g_linkOk ? SCAN_CYCLE_MS : SCAN_CYCLE_DOWN_MS)) {
    g_lastCycleMs = millis();
    runScanCycle();
  }

  BtnEv ev = buttonEvent();

  switch (g_screen) {
    case SCR_MENU: {
      int t = tappedRow(MENU_N);
      if (t >= 0)               { g_menuSel = t; activateMenu(); return; }  // touch select
      if      (ev == BTN_SHORT) { g_menuSel = (g_menuSel + 1) % MENU_N; drawMenu(); }
      else if (ev == BTN_LONG)  { activateMenu(); }
      else {  // idle: re-check the C5 link and refresh the status bar (dot + clock)
        static uint32_t lastPing = 0;
        static bool lastConn = false;
        if (phone::connected() != lastConn) {  // phone link came up/down: refresh the "Phone Link"/"Connected" row
          lastConn = phone::connected();
          drawMenu();  // repaints status bar too
        }
        if (millis() - lastPing > 2000) {
          lastPing = millis();
          g_linkOk = pingC5();
          if (g_linkOk) g_lastLinkOkMs = millis();  // keep link stickiness fresh from the menu
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
      int t = scanSetTouch();
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
      if (topBarTapped() || ev == BTN_LONG) { viewerBack(); return; }  // full-width back
      int bb = bodyButtonTapped(2);               // in-body Filter / Sort buttons
      if (bb == 0) { viewerOpenFilter(); return; }
      if (bb == 1) { viewerOpenSort();   return; }
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

    case SCR_FOLLOWLIST: {
      if (topBarTapped()) { followBackToScan(); return; }
      int t = listTouch(g_flN, &g_flOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawFollowRows(true); });
      if (t >= 0) { g_flSel = t; openFollowAction(); return; }
      if (ev == BTN_SHORT) {  // BOOT tap: move the highlight (wraps)
        g_flSel = (g_flSel + 1) % g_flN;
        if (g_flSel < g_flOff) g_flOff = g_flSel;
        if (g_flSel >= g_flOff + visibleRows()) g_flOff = g_flSel - visibleRows() + 1;
        drawFollowRows(true);
      } else if (ev == BTN_LONG) { openFollowAction(); }
      delay(20);
      return;
    }

    case SCR_FOLLOWACTION: {
      int t = tappedRow(FOLLOWACT_N);
      if (t >= 0)               { g_flActSel = t; activateFollowAction(t); return; }
      if      (ev == BTN_SHORT) { g_flActSel = (g_flActSel + 1) % FOLLOWACT_N; drawFollowAction(); }
      else if (ev == BTN_LONG)  { activateFollowAction(g_flActSel); }
      delay(20);
      return;
    }

    case SCR_FOLLOWDETAIL: {
      if (topBarTapped() || ev == BTN_LONG) { followBackToList(); return; }
      if (ev == BTN_SHORT) {  // BOOT tap: page down (wraps to the top)
        int maxOff = max(0, g_detN - visibleRows());
        g_detOff = (g_detOff >= maxOff) ? 0 : min(g_detOff + visibleRows(), maxOff);
        drawDetailList(true);
      }
      listTouch(g_detN, &g_detOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawDetailList(true); });
      delay(20);
      return;
    }

    case SCR_WHITELIST: {
      int n = whitelist::count();           // body rows are the rules; g_wlSel 0 = ADD button
      int total = n + 1;                     // ADD + rules (BOOT cursor span)
      if (topBarTapped()) { g_screen = SCR_SCANSETTINGS; g_scanSetSel = 5; drawScanSettings(); return; }  // < BACK
      if (bodyButtonTapped(1) == 0) { wlAddEnter(); return; }                                              // + ADD
      int t = listTouch(n, &g_wlOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawWhitelistRows(true); });
      if (t >= 0) { g_wlSel = t + 1; wlActivateRow(t); return; }  // tapped rule index -> cursor 1..n
      if (ev == BTN_SHORT) {  // BOOT tap: move the highlight (ADD=0, rules=1..n; wraps)
        g_wlSel = (g_wlSel + 1) % total;
        int rule = g_wlSel - 1;  // <0 while on the ADD button
        if (rule >= 0) {
          if (rule < g_wlOff) g_wlOff = rule;
          if (rule >= g_wlOff + visibleRows()) g_wlOff = rule - visibleRows() + 1;
        }
        drawWhitelistRows(true);
      } else if (ev == BTN_LONG) {
        if (g_wlSel == 0) wlAddEnter();
        else              wlActivateRow(g_wlSel - 1);
      }
      delay(20);
      return;
    }

    case SCR_WLADD: {
      int total = g_wlPickN;  // devices only (top-bar "< BACK" exits)
      if (topBarTapped()) { wlBackToList(); return; }
      int t = listTouch(total, &g_wlPickOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawWlAddRows(true); });
      if (t >= 0) { g_wlPickSel = t; wlAddSelected(t); return; }
      if (total > 0 && ev == BTN_SHORT) {  // BOOT tap: move the highlight (wraps)
        g_wlPickSel = (g_wlPickSel + 1) % total;
        if (g_wlPickSel < g_wlPickOff) g_wlPickOff = g_wlPickSel;
        if (g_wlPickSel >= g_wlPickOff + visibleRows()) g_wlPickOff = g_wlPickSel - visibleRows() + 1;
        drawWlAddRows(true);
      } else if (total > 0 && ev == BTN_LONG) { wlAddSelected(g_wlPickSel); }
      delay(20);
      return;
    }

    case SCR_WLRULE: {
      int t = tappedRow(WLACT_N);
      if (t >= 0)               { g_wlActSel = t; activateWlRule(t); return; }
      if      (ev == BTN_SHORT) { g_wlActSel = (g_wlActSel + 1) % WLACT_N; drawWlRule(); }
      else if (ev == BTN_LONG)  { activateWlRule(g_wlActSel); }
      delay(20);
      return;
    }

    case SCR_APPQR:
      if (topBarTapped()) { g_screen = SCR_MENU; drawMenu(); return; }  // tap < BACK
      if (ev != BTN_NONE) { g_screen = SCR_MENU; drawMenu(); }          // any button: back
      delay(20);
      return;

    case SCR_SCANROW: {
      int t = tappedRow(FOLLOWACT_N);
      if (t >= 0)               { g_scanRowSel = t; activateScanRow(t); return; }
      if      (ev == BTN_SHORT) { g_scanRowSel = (g_scanRowSel + 1) % FOLLOWACT_N; drawScanRow(); }
      else if (ev == BTN_LONG)  { activateScanRow(g_scanRowSel); }
      delay(20);
      return;
    }

    case SCR_SCANDETAIL: {
      if (topBarTapped() || ev == BTN_LONG) { g_screen = SCR_SCANROW; listTouchReset(); drawScanRow(); return; }
      if (ev == BTN_SHORT) {  // BOOT tap: page down (wraps to the top)
        int maxOff = max(0, g_detN - visibleRows());
        g_detOff = (g_detOff >= maxOff) ? 0 : min(g_detOff + visibleRows(), maxOff);
        drawDetailList(true);
      }
      listTouch(g_detN, &g_detOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawDetailList(true); });
      delay(20);
      return;
    }

    case SCR_SCANCONFIRM: {
      int t = tappedRow(CONFIRM_N);
      if (t >= 0)               { g_confirmSel = t; activateScanConfirm(t); return; }
      if      (ev == BTN_SHORT) { g_confirmSel = (g_confirmSel + 1) % CONFIRM_N; drawScanConfirm(); }
      else if (ev == BTN_LONG)  { activateScanConfirm(g_confirmSel); }
      delay(20);
      return;
    }

    case SCR_SCANFILTER: {
      int t = menuNav(SCANFILTER_N, &g_sfSel, &g_sfOff, ev, drawScanFilter);
      if (t >= 0) activateScanFilter(t);
      delay(20);
      return;
    }

    case SCR_SCAN: {
      // Single pass per loop iteration (timestamp-paced scan cycle) so touch/BOOT stay responsive.
      // Pending downloads and phone start/stop are handled at the top of loop().
      int bb = bodyButtonTapped(4);  // START/STOP | PAUSE/RESUME | FILTER | NEW SESSION
      if (bb == 0) {
        g_scanActive = !g_scanActive;
        if (g_scanActive) g_lastCycleMs = 0;  // first cycle runs right away
        else              setLed(!g_linkOk, g_linkOk, false);
        render(); pushStatus();
        delay(20);
        return;
      }
      if (bb == 1) { toggleViewFrozen(); delay(20); return; }
      if (bb == 2) { openScanFilter(); return; }
      if (bb == 3) {  // NEW SESSION: fresh log file + reset dedup, without leaving the scanner
        startNewSession();
        g_scanMsg = "New session started"; g_scanMsgCol = TFT_GREEN;
        render(); pushStatus();
        delay(20);
        return;
      }
      { bool bn = bannerTapped();  // always called so its edge state stays current
        if (bn && g_followN > 0 && ev != BTN_LONG) { openFollowList(); return; } }
      // BOOT short-press opens the follow drill-down (so it's reachable without touch).
      if (ev == BTN_SHORT && g_followN > 0) { openFollowList(); return; }
      // Back via the on-screen button (touch) or a long BOOT hold; asks first while scanning.
      if (ev == BTN_LONG || topBarTapped()) { backFromScan(); return; }
      int rt = handleScanTouch();  // scroll / row tap
      if (rt >= 0) { openScanRow(rt); return; }
      if (g_scanActive && millis() - g_lastCycleMs >= SCAN_CYCLE_MS) { g_lastCycleMs = millis(); runScanCycle(); }
      delay(20);
      return;
    }
  }
}
#endif

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
#include "../host_common/phone.h"
#include "../host_common/webshare.h"
#include "../host_common/sigdb.h"
#include "../host_common/whitelist.h"
#include "touch.h"
#include "../host_common/logfilter.h"
#include "../host_common/power.h"
#include "../host_common/host_link.h"
#include "../host_common/host_core.h"
#include "../host_common/host_log.h"
#include "../host_common/host_status.h"
#include "../host_common/host_phone_cmds.h"
#include "../host_common/host_board.h"
#include "../host_common/host_ui.h"
#include "logo.h"

using namespace link_protocol;

// >>> Set to 1 to run the link connection monitor, 0 for the normal scanner. <<<
#define LINK_MONITOR 0

static constexpr uint8_t UI_ROTATION = 0;  // portrait 240x320 (90 CCW from landscape)
// UI accent: the logo magenta #AA00AA in RGB565 ((0xAA>>3)<<11 | 0 | (0xAA>>3)). Used for selected
// menu-item/button fills and the "< BACK" bar, replacing the old UI_ACCENT blue.
static constexpr uint16_t UI_ACCENT = 0xA815;  // #AA00AA

TFT_eSPI       tft = TFT_eSPI();

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
  tft.init();
  tft.setRotation(UI_ROTATION);
  // Claim the backlight for PWM AFTER tft.init(): TFT_eSPI is built with TFT_BL=21/TFT_BACKLIGHT_ON,
  // so init() drives GPIO21 as a plain HIGH digital output — doing this before init() leaves the pin
  // detached from LEDC and the backlight stuck at full (brightness slider did nothing). Attaching
  // here re-takes the pin as an 8-bit PWM channel so applyBrightness() actually dims it.
  ledcAttach(TFT_BL_PIN, 5000, 8);
  ledcWrite(TFT_BL_PIN, 255);        // full brightness until the saved level is applied

  hostlink::begin(LINK_RX_PIN, LINK_TX_PIN);
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
              SCR_SCANROW, SCR_SCANDETAIL, SCR_SCANCONFIRM, SCR_SCANFILTER,
              SCR_MENUCAT, SCR_MENUCATDETAIL };
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
static const char* kMenuItems[] = { "Scan" };  // Phone Link + Settings are now title-bar icons
static constexpr int MENU_N = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
// HOME title-bar icon buttons: a cog (left = Settings) and a phone (right = Phone Link), on the
// "HOME" line. They sit in the BOOT nav right after the list buttons (so they're reachable without
// touch), then the threat chips follow. Selection index: cog = MENU_N, phone = MENU_N+1.
static constexpr int HOME_ICONS = 2;
static constexpr int HOME_ICON_R = 11, HOME_ICON_CY = 44;  // radius + vertical centre on the title line
                                                           // (down from the status-bar line at y=21 for padding)
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
static constexpr int HOME_ALERT_Y = 120;  // HOME alert band: free strip between the Scan button (ends ~109) and the threat caption (176)
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
// Per-category tile colours (RGB565), approximating the web app's CAT_COL palette (webapp/js/config.js)
// so the HOME threat readout reads the same hue per category on both the device and the phone — the
// project's "reuse CAT_COL anywhere categories appear" consistency directive. The TFT's 16-bit gamut
// can't match the sRGB hex exactly; these are the nearest RGB565. Index = Category enum.
static const uint16_t kCatColor[CAT_N] = {
  0x0000,  // NONE (unused)
  0xFA89,  // Flock   #f85149
  0xFCC0,  // Axon    #ff9800
  0xFE87,  // ALPR    #ffd33d
  0x5D3F,  // Cam     #58a6ff
  0x7E1F,  // Ring    #79c0ff
  0xA39E,  // Raven   #a371f7
  0xDB16,  // Glass   #db61b7
  0x3E39,  // Tracker #39c5cf
  0x3DCA,  // Drone   #3fb950
  0xFBCE,  // Deauth  #ff7b72
  0xFD2A,  // Flipper #ffa657
  0xE588,  // Skim    #e3b341
  0x8410,  // Other   (grey; not tiled)
};
// One-line "what signatures match this category" blurb, shown atop the category device list.
static const char* catDescription(int c) {
  switch (c) {
    case CAT_FLOCK:   return "Flock OUIs, SoftAP SSID, GATT UUID";
    case CAT_AXON:    return "Axon OUIs + name prefixes (AB#/AXON)";
    case CAT_ALPR:    return "ALPR OUIs (Motorola / Genetec)";
    case CAT_CAM:     return "Camera OUIs (Wyze/Hik/Axis/Arlo)";
    case CAT_RING:    return "Ring camera/doorbell OUIs";
    case CAT_RAVEN:   return "Raven BLE UUIDs (0x3100-0x3500)";
    case CAT_GLASS:   return "Smart-glasses BLE IDs (Meta/Snap)";
    case CAT_TRACKER: return "AirTag/Tile/Tag - only if following you";
    case CAT_DRONE:   return "OpenDroneID (BLE 0xFFFA / Wi-Fi)";
    case CAT_DEAUTH:  return "Deauth / evil-twin / pwnagotchi";
    case CAT_FLIPPER: return "Flipper OUI / BLE UUID / name";
    case CAT_SKIM:    return "Card-skimmer BLE modules";
    case CAT_OTHER:   return "Suspected; no category match";
    default:          return "";
  }
}

// --- Scanner screen (SCR_SCAN) state. The visible list is a filtered view (g_scanView) of either
// the live g_groups or, while the view is frozen (PAUSE), a snapshot (g_frozenRows). Bounded static
// buffers sized MAX_DET (no heap). ---
struct ScanRow {
  uint8_t mac[6]; char name[20]; char tag[12]; int16_t rssi;
  uint8_t tier, source, channel, srcMask; bool whitelisted;
};
static bool     g_viewFrozen = false;   // list frozen on a snapshot while scanning continues
static uint8_t  g_lvType   = 0x1F;      // live filter srcType bits 2.4/5G/BLE/PRB/154
static bool     g_lvThreat = false;
static ScanRow  g_frozenRows[MAX_DET];
static int      g_frozenCount = 0;
static uint8_t  g_scanView[MAX_DET];    // filtered index into g_groups (live) OR g_frozenRows (frozen)
static int      g_scanViewN = 0;
static uint8_t  g_scanRowMac[6];        // device the row-action / detail screens target
static int      g_scanRowSel = 0, g_sfSel = 0, g_sfOff = 0, g_confirmSel = 0;
static const char* g_scanMsg = nullptr; static uint16_t g_scanMsgCol = TFT_GREEN;  // state-line feedback
static bool        g_followFromHome = false;  // follow drill-down opened from the HOME alert band (Back -> HOME)

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

// ---- Touch lock (bag-safe input lock) ----------------------------------------------------------
// A small padlock in the status bar; 2 quick taps on it toggle the lock. While locked, ALL input
// (touch + BOOT) is ignored except that same 2-tap gesture, so the device can ride in a bag without
// walking the menus. Discrete taps (not a hold/swipe) resist the constant pressure of a bag. The
// background scan keeps running the whole time. No physical button needed.
static int  lockCx()      { return tft.width() - 42; }     // padlock glyph center; battery sits 30px to its
                                                           // left, so this evens the battery/lock/phone/dot
                                                           // spacing (clear of phone icon/dot/GPS)
static constexpr int      LOCK_TAPS         = 2;            // taps to toggle (2: resistive touch merges fast taps)
static constexpr uint32_t LOCK_TAP_WINDOW_MS = 1500;       // ...that must land within rolling windows this wide

static void drawLockGlyph(bool locked) {
  int cx = lockCx(), top = 3;
  uint16_t c = locked ? TFT_YELLOW : 0x4208;  // yellow when locked, dim grey when not
  tft.drawFastHLine(cx - 3, top, 6, c);        // shackle arch
  tft.drawFastVLine(cx - 3, top, 4, c);
  tft.drawFastVLine(cx + 2, top, 4, c);
  tft.fillRect(cx - 5, top + 4, 10, 7, c);     // body
  tft.drawPixel(cx, top + 7, TFT_BLACK);       // keyhole
}

// Battery glyph, left of the padlock: outline + level-proportional fill, coloured by charge
// (green >50 %, yellow 20-50 %, red <20 %). Hollow dim-grey when no battery voltage is present
// (rig on USB / battery switch off). No number here — the 240 px bar is tight; the exact % goes
// to the phone/web app via STATUS bat=.
static void drawBatteryGlyph() {
  const int bw = 16, bh = 9, top = 6;
  int bx = lockCx() - 30;                       // body left edge, clear of GPS (left) + padlock (right)
  int pct = power::percent();
  uint16_t c;
  if      (pct < 0)   c = 0x4208;               // dim grey: no battery present
  else if (pct > 50)  c = TFT_GREEN;
  else if (pct >= 20) c = TFT_YELLOW;
  else                c = TFT_RED;
  tft.drawRect(bx, top, bw, bh, c);             // body outline
  tft.fillRect(bx + bw, top + 2, 2, bh - 4, c); // + terminal nub
  if (pct >= 0) {
    int fillW = (bw - 2) * pct / 100;           // inner fill proportional to %
    if (fillW > 0) tft.fillRect(bx + 1, top + 1, fillW, bh - 2, c);
  }
}

// GPS glyph, left of the battery: a map-pin plus a tiny source badge so the fix source is readable
// at a glance — a small PHONE handset when the fix comes from the linked phone, a little CHIP (IC)
// when it comes from the onboard NEO-6M module. Dim-grey pin with no badge when there's no fix.
// Colour also tracks source (white = phone, cyan = module) to match the old text. Tapping it opens
// the GPS modal (serviceGpsModal). Footprint sits ~6 px left of the battery so the right cluster
// (GPS / battery / lock / phone / link-dot) stays evenly spaced. An orange pin with no badge = no live
// fix, the last known position is being held (logged rows carry "fix_age").
static int gpsX() { return (lockCx() - 30) - 22; }   // left edge of the GPS glyph footprint
static void drawGpsGlyph() {
  int xo = gpsX();
  tft.fillRect(xo - 2, 2, 20, 17, TFT_BLACK);        // clear the footprint (source badge changes)
  int src = posSource();                             // 0 none / 1 phone / 2 module / 3 held
  uint16_t c = src == 0 ? 0x4208 : src == 3 ? TFT_ORANGE : (src == 2 ? TFT_CYAN : TFT_WHITE);
  int cx = xo + 4, cy = 7, r = 3;                    // map-pin head + point
  tft.fillCircle(cx, cy, r, c);
  tft.fillTriangle(cx - r, cy + 1, cx + r, cy + 1, cx, cy + 8, c);
  tft.fillCircle(cx, cy, 1, TFT_BLACK);              // hole
  int sx = xo + 10;                                  // source badge, right of the pin
  if (src == 1) {                                    // phone: upright handset
    tft.drawRoundRect(sx, 3, 6, 12, 1, c);
    tft.drawFastHLine(sx + 2, 5, 2, c);              // earpiece
    tft.drawFastHLine(sx + 1, 12, 4, c);             // button divider
  } else if (src == 2) {                             // module: little IC with legs
    tft.fillRect(sx, 5, 7, 8, c);
    tft.drawFastVLine(sx, 5, 8, TFT_BLACK);          // notch the left face
    tft.drawPixel(sx + 2, 7, TFT_BLACK);             // pin-1 dot
    for (int ly = 6; ly <= 11; ly += 2) {            // leg stubs both sides
      tft.drawFastHLine(sx - 2, ly, 2, c);
      tft.drawFastHLine(sx + 7, ly, 2, c);
    }
  }
}

static void drawStatusBar() {
  int W = tft.width();
  tft.fillRect(0, 0, W, 21, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  // time (left) — local (phone tz). White = phone-synced, cyan = kept by the onboard GPS clock.
  char t[8];
  if (timeHasSync()) {
    uint32_t e = epochLocal();
    snprintf(t, sizeof(t), "%02u:%02u", (unsigned)((e / 3600) % 24), (unsigned)((e / 60) % 60));
    tft.setTextColor(timeSource() == 2 ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
  } else { strcpy(t, "00:00"); tft.setTextColor(TFT_DARKGREY, TFT_BLACK); }
  tft.drawString(t, 4, 4, 2);
  // phone connection icon (right) + C5-link status dot to its right
  int ix = W - 30;
  if (webshare::active())      iconWifi(ix, 3, TFT_CYAN);
  else if (phone::connected()) iconBle(ix + 2, 3, TFT_BLUE);
  else                         iconNoConn(ix, 3, TFT_DARKGREY);
  tft.fillCircle(W - 8, 10, 4, g_linkOk ? TFT_GREEN : TFT_RED);  // C5 link up/down
  drawGpsGlyph();                                              // location pin + source badge (tap -> modal)
  drawBatteryGlyph();                                           // battery level (left of padlock)
  drawLockGlyph(g_touchLocked);                                 // padlock (2-tap toggle)
  tft.drawFastHLine(0, 21, W, TFT_DARKGREY);
}

// Input-lock gate: returns true when input should be swallowed this iteration (i.e. the device is
// locked, or this touch was part of the lock toggle). Call it right after buttonEvent(), before the
// per-screen switch, and `return` from loop() when it's true. LOCK_TAPS rising-edge taps in the
// padlock region (within rolling LOCK_TAP_WINDOW_MS windows) flip g_touchLocked; a held/continuous
// press is a single edge, so bag pressure can't rack up a count.
static bool serviceTouchLock() {
  static bool     prev = false;
  static int      taps = 0;
  static uint32_t lastTap = 0;
  bool now = g_touchOk && g_touch.touched();
  bool regionTap = false;
  if (now && !prev) {                 // rising edge only
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z) &&
        sx >= lockCx() - 16 && sx <= lockCx() + 16 && sy >= 0 && sy <= 21) regionTap = true;
  }
  prev = now;
  if (regionTap) {
    uint32_t t = millis();
    if (t - lastTap > LOCK_TAP_WINDOW_MS) taps = 0;  // stale: restart the count
    lastTap = t;
    if (++taps >= LOCK_TAPS) {
      taps = 0;
      g_touchLocked = !g_touchLocked;
      drawStatusBar();                 // repaint the padlock in its new state (immediate feedback)
      Serial.printf("[CYD] touch %s\n", g_touchLocked ? "LOCKED (2-tap to unlock)" : "unlocked");
    }
    return true;                       // consume the toggle tap (never passes to the screen)
  }
  return g_touchLocked;                 // while locked, swallow every other touch + BOOT event
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
                 groupName(&g), g.bestRssi, "",
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
  drawBodyButtonN(1, 4, g_viewFrozen ? "RESUME" : "PAUSE", g_viewFrozen ? TFT_BLUE : UI_ACCENT, false);
  drawBodyButtonN(2, 4, "FILTER", UI_ACCENT, g_lvType != 0x1F || g_lvThreat);
  drawBodyButtonN(3, 4, "NEW SES", UI_ACCENT, false);
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
  snprintf(buf, sizeof(buf), "U:%lu", (unsigned long)g_fg.uniqCount);
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
  if (followAlertN() > 0 || evA) {
    // Red deauth / evil-twin alert (full width alone, right half when stacked with the follow
    // banner, which keeps the left half and stays tappable anywhere on the band).
    int W = tft.width(), fw = evA ? W / 2 : W;
    tft.setTextDatum(MC_DATUM);
    if (followAlertN() > 0) {
      tft.fillRect(0, BANNER_Y, fw, BANNER_H, TFT_MAGENTA);
      tft.setTextColor(TFT_WHITE, TFT_MAGENTA);
      snprintf(buf, sizeof(buf), evA ? "! FOLLOW: %d" : "! FOLLOWING: %d  (tap)", followAlertN());
      tft.drawString(buf, fw / 2, BANNER_Y + BANNER_H / 2 + 1, 1);
    }
    if (evA) {
      int x0 = (followAlertN() > 0) ? fw : 0;
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
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);
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
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);  // full-width back (app convention)
  int W = tft.width();
  tft.setTextDatum(MC_DATUM);
  int ty = drawCenteredQr(APP_URL, 56) + 14;  // below the back bar (y24..42); +8px margin (QR quiet zone starts at qy-6)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Scan to open the app", W / 2, ty, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(scannerName(), W / 2, ty + 20, 2);  // pick THIS name in the phone's Bluetooth list
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(g_touchOk ? "Tap BACK or press button" : "Press button: back",
                 W / 2, ty + 40, 2);
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
    if (s) tft.fillRoundRect(6, y - 5, W - 12, ROW_H, 6, UI_ACCENT);
    else   tft.drawRoundRect(6, y - 5, W - 12, ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(s ? TFT_WHITE : TFT_LIGHTGREY, s ? UI_ACCENT : TFT_BLACK);
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
    if (s) tft.fillRoundRect(6, rt, W - 12, ROW_H, 6, UI_ACCENT);
    else   tft.drawRoundRect(6, rt, W - 12, ROW_H, 6, TFT_DARKGREY);
    uint16_t bg = s ? UI_ACCENT : TFT_BLACK;

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

// Background-scan threat readout at the bottom of HOME (above the hint line). Shows the FULL fixed
// category list as "NAME: n" tiles, always (0 included) so the user can see every type at a glance
// (SquachWatch-style). A non-zero tile is coloured by that category's worst tier; zero tiles are
// dim. Counts come from countCategories() over g_groups (kept fresh by the headless bg cycle). Each
// tile is tappable (-> that category's device list); BOOT nav walks the HOME items then the tiles
// (g_menuSel MENU_N..MENU_N+N-1). Repaints only its own band so the menu rows above don't flicker.
static constexpr int MENU_HINT_Y   = 292;                       // first of the two-line hint at the bottom
static constexpr int MENU_THREAT_Y = 176;                       // "Background threats" caption (raised)
static constexpr int MENU_THREAT_H = MENU_HINT_Y - MENU_THREAT_Y - 2;  // down to just above the hint
// 3 columns x 4 rows holds the 12 tiled categories (CAT_FLOCK..CAT_SKIM; OTHER is not tiled);
// tile text is font 1 (smallest).
static constexpr int CHIP_Y0 = 190, CHIP_STEP = 20, CHIP_H = 18, CHIP_COLS = 3;
static constexpr int MENU_CAT_MAX = CAT_N - 1;  // array capacity (>= number of tiled categories)
static uint8_t g_menuCat[MENU_CAT_MAX];  // category ids in tile order (fixed), for input dispatch
static int     g_menuCatN = 0;
// First g_menuSel index that maps to a threat chip (after the list buttons + the two title icons).
static constexpr int MENU_CHIP0 = MENU_N + HOME_ICONS;

// Geometry of threat tile i (of n total): top-left x,y and width w. A partial last row is centred
// (e.g. the lone "OTHER" tile sits in the middle instead of hugging the left column).
static void chipRect(int i, int n, int& x, int& y, int& w) {
  int cellW = tft.width() / CHIP_COLS;
  int rowg = i / CHIP_COLS, col = i % CHIP_COLS;
  int inRow = min(CHIP_COLS, n - rowg * CHIP_COLS);     // tiles on this row
  int startX = (tft.width() - inRow * cellW) / 2;        // centre a short row
  x = startX + col * cellW; y = CHIP_Y0 + rowg * CHIP_STEP; w = cellW;
}

static void drawMenuThreats() {
  int cnt[CAT_N], worst[CAT_N], scoreMax[CAT_N];
  countCategories(cnt, worst, scoreMax);
  g_menuCatN = 0;                                   // every category except OTHER (the catch-all is not tiled)
  for (int c = 1; c < CAT_N; c++) if (c != CAT_OTHER) g_menuCat[g_menuCatN++] = (uint8_t)c;
  if (g_menuSel >= MENU_CHIP0 + g_menuCatN) g_menuSel = 0;

  int W = tft.width();
  tft.fillRect(0, MENU_THREAT_Y, W, MENU_THREAT_H, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Background threats", W / 2, MENU_THREAT_Y + 1, 1);
  for (int i = 0; i < g_menuCatN; i++) {
    int c = g_menuCat[i], x, y, w; chipRect(i, g_menuCatN, x, y, w);
    bool sel = (g_menuSel == MENU_CHIP0 + i);
    bool hot = cnt[c] > 0;
    // Outline + label take the per-category hue (matches the web app's CAT_COL). A non-zero tile
    // also shows a tier-coloured threat-score badge (max sigdb score) on the right, so severity is
    // still visible now that the tile hue encodes category rather than tier.
    uint16_t catCol  = hot ? kCatColor[c] : TFT_DARKGREY;
    uint16_t outline = sel ? TFT_CYAN : (hot ? catCol : 0x2104);
    tft.drawRoundRect(x + 1, y, w - 2, CHIP_H, 3, outline);
    char b[20];
    snprintf(b, sizeof(b), "%s:%d", kCatName[c], cnt[c]);
    for (char* p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;  // FLOCK:0 style
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(catCol, TFT_BLACK);
    tft.drawString(b, x + 5, y + CHIP_H / 2, 1);
    if (hot) {                                       // tier-coloured threat score, right-aligned:
      // the signature score when the DB scored it, else the tier level (1-3) so a behavioral-flag-only
      // threat (e.g. an OpenDroneID emitter with no OUI/name/UUID match) reads its tier, not 0.
      int sv = scoreMax[c] > 0 ? min(scoreMax[c], 99) : worst[c];
      char sb[4]; snprintf(sb, sizeof(sb), "%d", sv);
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(tierColor(worst[c], 0, 0), TFT_BLACK);
      tft.drawString(sb, x + w - 4, y + CHIP_H / 2, 1);
    }
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);  // restore the menu hint-line colour state
}

// HOME alert band (HOME_ALERT_Y, BANNER_H): same magenta follow / red deauth+evil banner as the scan
// screen (drawScanInfo), so a follower or flood is visible without opening Scan. Always clears its
// strip first, so it vanishes cleanly when the condition drops.
static void drawHomeAlerts() {
  int W = tft.width();
  tft.fillRect(0, HOME_ALERT_Y, W, BANNER_H, TFT_BLACK);
  bool evA = eventAlertActive();
  if (followAlertN() <= 0 && !evA) return;
  int fw = evA ? W / 2 : W;
  char buf[32];
  tft.setTextDatum(MC_DATUM);
  if (followAlertN() > 0) {
    tft.fillRect(0, HOME_ALERT_Y, fw, BANNER_H, TFT_MAGENTA);
    tft.setTextColor(TFT_WHITE, TFT_MAGENTA);
    snprintf(buf, sizeof(buf), evA ? "! FOLLOW: %d" : "! FOLLOWING: %d  (tap)", followAlertN());
    tft.drawString(buf, fw / 2, HOME_ALERT_Y + BANNER_H / 2 + 1, 1);
  }
  if (evA) {
    int x0 = (followAlertN() > 0) ? fw : 0;
    tft.fillRect(x0, HOME_ALERT_Y, W - x0, BANNER_H, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    int an = snprintf(buf, sizeof(buf), "! ");
    if (deauthAlertActive()) an += snprintf(buf + an, sizeof(buf) - an, "DEAUTH:%d ", (int)g_deauthShown);
    if (evilAlertActive())   an += snprintf(buf + an, sizeof(buf) - an, "EVIL:%d", (int)g_evilShown);
    tft.drawString(buf, x0 + (W - x0) / 2, HOME_ALERT_Y + BANNER_H / 2 + 1, 1);
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);  // restore the menu hint-line colour state
}

// Fresh tap on the HOME alert band. Call every loop iteration (keeps edge state); caller checks g_followN.
static bool homeAlertTapped() {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return false; }
  bool now = g_touch.touched();
  bool hit = false;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z) && sy >= HOME_ALERT_Y && sy <= HOME_ALERT_Y + BANNER_H) hit = true;
  }
  prev = now;
  return hit;
}

// Fresh tap on a HOME threat tile -> tile index (0..g_menuCatN-1), else -1. Mirrors tappedRow()'s
// edge detection; its hit region (y >= CHIP_Y0) is disjoint from the menu rows so both can be
// polled each loop without stealing each other's taps.
static int menuChipTapped() {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return -1; }
  bool now = g_touch.touched();
  int  hit = -1;
  if (now && !prev && g_menuCatN > 0) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z)) {
      for (int i = 0; i < g_menuCatN; i++) {
        int x, y, w; chipRect(i, g_menuCatN, x, y, w);
        if (sx >= x && sx < x + w && sy >= y && sy < y + CHIP_H) { hit = i; break; }
      }
    }
  }
  prev = now;
  return hit;
}

// --- HOME title-bar icon buttons: cog (Settings, left) + phone (Phone Link, right). Drawn on the
// "HOME" line; the phone is grey when no phone is on the GATT link, green when connected. ---
static int homeCogCx()   { return HOME_ICON_R + 6; }
static int homePhoneCx() { return tft.width() - HOME_ICON_R - 6; }

static void drawCogIcon(int cx, int cy, int r, uint16_t color, bool sel) {
  if (sel) tft.drawCircle(cx, cy, r + 2, TFT_CYAN);
  for (int k = 0; k < 8; k++) {                 // 8 teeth
    float a = k * (float)PI / 4.0f;
    int tx = cx + (int)(cosf(a) * (r - 1)), ty = cy + (int)(sinf(a) * (r - 1));
    tft.fillRect(tx - 1, ty - 1, 3, 3, color);
  }
  tft.drawCircle(cx, cy, r - 3, color);          // gear body
  tft.fillCircle(cx, cy, 2, color);              // hub
}

static void drawPhoneIcon(int cx, int cy, int r, uint16_t color, bool sel) {
  if (sel) tft.drawCircle(cx, cy, r + 2, TFT_CYAN);
  tft.drawCircle(cx, cy, r, color);              // circular badge
  const int bw = 8, bh = 13;                     // smartphone body
  tft.drawRoundRect(cx - bw / 2, cy - bh / 2, bw, bh, 2, color);
  tft.drawFastHLine(cx - 2, cy - bh / 2 + 2, 4, color);  // earpiece
  tft.fillCircle(cx, cy + bh / 2 - 2, 1, color);         // home button
}

static void drawHomeIcons() {
  bool conn = phone::connected();
  drawCogIcon(homeCogCx(), HOME_ICON_CY, HOME_ICON_R, TFT_LIGHTGREY, g_menuSel == MENU_N);
  drawPhoneIcon(homePhoneCx(), HOME_ICON_CY, HOME_ICON_R, conn ? TFT_GREEN : TFT_DARKGREY,
                g_menuSel == MENU_N + 1);
}

// Fresh tap on a HOME title icon: 0 = cog (Settings), 1 = phone (Phone Link), else -1.
static int homeIconTapped() {
  static bool prev = false;
  if (!g_touchOk) { prev = false; return -1; }
  bool now = g_touch.touched();
  int  hit = -1;
  if (now && !prev) {
    int16_t sx, sy, z;
    if (g_touch.getScreen(tft, sx, sy, z)) {
      int d = HOME_ICON_R + 3;
      if (abs(sy - HOME_ICON_CY) <= d) {
        if      (abs(sx - homeCogCx())   <= d) hit = 0;
        else if (abs(sx - homePhoneCx()) <= d) hit = 1;
      }
    }
  }
  prev = now;
  return hit;
}

// Home menu. Reuses the status bar (time / GPS / connection / link dot) up top. Phone Link and
// Settings are the two title-bar icons (phone right, cog left); the list holds just Scan.
static void drawMenu() {
  drawListMenu("COINTEL-FLO", kMenuItems, MENU_N, g_menuSel);
  // Recolor the HOME title to the logo magenta (#AA00AA); overdrawn at the same pos/font so the
  // opaque-background redraw cleanly replaces drawListMenu's default cyan. HOME only.
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(UI_ACCENT, TFT_BLACK);
  tft.drawString("COINTEL-FLO", tft.width() / 2, 32, 4);
  tft.setTextDatum(TL_DATUM);
  drawHomeIcons();  // cog (Settings) left + phone (Phone Link) right, on the HOME line
  // Two-line hint (the single line overran 240 px); kept just below the threat readout.
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(g_touchOk ? "Tap an item, or use BOOT:" : "Navigate with the BOOT button:", 10, MENU_HINT_Y, 1);
  tft.drawString("tap = next,  hold = select", 10, MENU_HINT_Y + 10, 1);
  drawMenuThreats();
  drawHomeAlerts();
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
    if (s) tft.fillRoundRect(6, y - 5, W - 12, SS_ROW_H, 6, UI_ACCENT);
    else   tft.drawRoundRect(6, y - 5, W - 12, SS_ROW_H, 6, TFT_DARKGREY);
    tft.setTextColor(s ? TFT_WHITE : TFT_LIGHTGREY, s ? UI_ACCENT : TFT_BLACK);
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
static constexpr int MAX_LOG_ROWS = 512;  // 8 KB index of DEVICES; devices past this are not indexed (truncated)
static constexpr int MAX_SESS     = 48;
static constexpr int PICK_VIS     = FILE_LIST_VIS;  // picker rows on screen (ROW_STEP geometry)
static constexpr int DET_MAX_LINES = 28;

// One entry per DEVICE (src+MAC): the log re-logs a device every few minutes, so the index keeps the
// newest row's offset/epoch, the strongest RSSI and the highest tier. key = FNV-1a of src+MAC.
struct LogIdx { uint32_t offset; uint32_t epoch; uint32_t key; int16_t rssi; uint8_t srcType; uint8_t tier; };  // 16 B
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
               lr.src, lr.name[0] ? lr.name : lr.mac, lr.rssi, tc, lr.tier > 0, 3);
  }
  drawScrollIcons(g_viewOffset, g_viewN, vis);
}

static void drawViewer() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);  // full-width back (app convention)
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(g_viewName, 4, 44, 1);
  char b[48];
  snprintf(b, sizeof(b), g_idxTrunc ? "%d of %d devices (first %d)" : "%d of %d devices",
           g_viewN, g_idxN, MAX_LOG_ROWS);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(b, 4, 54, 1);
  drawBodyButtonN(0, 2, "Filter", UI_ACCENT, false);   // two body actions, right above the list
  drawBodyButtonN(1, 2, "Sort",   UI_ACCENT, false);
  drawViewerList(false);
}

// One streaming pass over the file -> compact per-DEVICE index (first MAX_LOG_ROWS devices; repeat
// rows of an indexed device keep updating it, so a long session never hides its later sightings).
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
    uint32_t key = 2166136261u;
    for (const char* q = r.src; *q; q++) key = (key ^ (uint8_t)*q) * 16777619u;
    for (const char* q = r.mac; *q; q++) key = (key ^ (uint8_t)*q) * 16777619u;
    int k = -1;
    for (int i = g_idxN - 1; i >= 0; i--) if (g_ex->idx[i].key == key) { k = i; break; }
    if (k >= 0) {  // repeat sighting: newest row wins, keep the strongest RSSI / highest tier
      LogIdx& e = g_ex->idx[k];
      e.offset = pos; e.epoch = r.epoch;
      if (r.rssi > e.rssi) e.rssi = (int16_t)r.rssi;
      if (r.tier > e.tier) e.tier = (uint8_t)r.tier;
      continue;
    }
    if (g_idxN >= MAX_LOG_ROWS) { g_idxTrunc = true; continue; }  // keep scanning: indexed devices still update
    LogIdx& e = g_ex->idx[g_idxN++];
    e.offset = pos; e.epoch = r.epoch; e.key = key; e.rssi = (int16_t)r.rssi;
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
  addField("Name", r.name[0] ? r.name : r.mac);  // fall back to MAC when nameless (still unique)
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
  drawTopBar("BACK", UI_ACCENT, TFT_CYAN);
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
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);
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
  if (g_followFromHome) {  // opened from the HOME alert band: return to HOME, not the scan screen
    g_followFromHome = false;
    g_screen = SCR_MENU; g_menuSel = 0; drawMenu();
    return;
  }
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
    if (timeHasSync()) {
      time_t t = (time_t)(epochLocal() - (nowMs - ms) / 1000);
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
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);
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
    snprintf(s.name, sizeof(s.name), "%.19s", groupName(&g));
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
// Fill the detail field list (g_detL via addField) for a device by MAC. Shared by the scanner
// row detail and the HOME-menu category drill-down; does not touch the screen.
static void buildDeviceDetail(const uint8_t* mac) {
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
      Category cat = groupCategory(*g);
      if (cat != CAT_NONE && cat != CAT_OTHER) addField("Category", kCatName[cat]);
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
    if (cid) {
      const char* mk = sigdb::cidName(cid);
      if (mk && mk[0]) snprintf(b, sizeof(b), "%s (%04X)", mk, cid);  // maker name + raw ID
      else             snprintf(b, sizeof(b), "%04X", cid);
      addField("BLE company", b);
    }
    if (svc) {
      char u[33];
      for (int j = 0; j < 16; j++) sprintf(u + j * 2, "%02X", svc[j]);
      addField("Service UUID", u);
    }
    // Extra 16-bit UUIDs from the active scan (deduped across this device's member detections).
    uint16_t more[SVC16_MAX * 2]; int nm = 0;
    for (int i = 0; i < g_detCount && nm < (int)(sizeof(more) / sizeof(more[0])); i++) {
      if (memcmp(g_dets[i].mac, mac, 6) != 0) continue;
      for (int s = 0; s < SVC16_MAX; s++) {
        uint16_t v = g_dets[i].svc16[s];
        if (!v) continue;
        bool dup = false;
        for (int k = 0; k < nm; k++) if (more[k] == v) { dup = true; break; }
        if (!dup && nm < (int)(sizeof(more) / sizeof(more[0]))) more[nm++] = v;
      }
    }
    if (nm > 0) {
      char u[40]; int o = 0;
      for (int k = 0; k < nm; k++)
        o += snprintf(u + o, sizeof(u) - o, k ? " %04X" : "%04X", more[k]);
      addField("More UUIDs", u);
    }
    const RemoteId* rid = ridByMac(mac);
    if (rid) {
      addField("-- Remote-ID --", "", TFT_ORANGE);
      if (rid->uasId[0]) addField("Drone ID", rid->uasId);
      if (rid->droneLat != RID_LL_UNK || rid->droneLon != RID_LL_UNK) {
        // Separate lat/lon rows: a combined "lat, lon" (~21 chars) overflows addField's 19-char line.
        snprintf(b, sizeof(b), "%.6f", rid->droneLat / 1e7); addField("Drone lat", b);
        snprintf(b, sizeof(b), "%.6f", rid->droneLon / 1e7); addField("Drone lon", b);
      }
      if (rid->heightM != RID_ALT_UNK) { snprintf(b, sizeof(b), "%d m", rid->heightM); addField("Height", b); }
      if (rid->altGeoM != RID_ALT_UNK) { snprintf(b, sizeof(b), "%d m", rid->altGeoM); addField("Geo alt", b); }
      if (rid->headingDeg != RID_HDG_UNK) {  // speed + heading both come from the Location message
        snprintf(b, sizeof(b), "%.1f m/s", rid->speedCms / 100.0);
        addField("Speed", b);
        snprintf(b, sizeof(b), "%u deg", (unsigned)rid->headingDeg);  // plain text: glyph not guaranteed in the font
        addField("Heading", b);
      }
      if (rid->operatorId[0]) addField("Operator", rid->operatorId);
      if (rid->operatorLat != RID_LL_UNK || rid->operatorLon != RID_LL_UNK) {
        snprintf(b, sizeof(b), "%.6f", rid->operatorLat / 1e7); addField("Oper lat", b);
        snprintf(b, sizeof(b), "%.6f", rid->operatorLon / 1e7); addField("Oper lon", b);
      }
    }
  }
}

// Build + paint the shared device-detail screen. `scr` records where we came from so the input
// handler's "< BACK" returns to the right list (scanner row menu vs category list).
static void showDeviceDetail(const uint8_t* mac, Screen scr) {
  buildDeviceDetail(mac);
  g_screen = scr;
  listTouchReset();
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("DETAIL", 4, 46, 1);
  drawDetailList(false);
}

static void openScanDetail(const uint8_t* mac) { showDeviceDetail(mac, SCR_SCANDETAIL); }

// ---- HOME-menu category drill-down: SCR_MENUCAT (devices in one category) -> SCR_MENUCATDETAIL ----
// Opened by tapping a HOME threat chip. Snapshots the matching device MACs on entry; the bg scan
// cycle is frozen while on these screens (SCR_MENUCAT/DETAIL are in the inDrill set), so g_groups /
// g_dets stay put and groupByMac() stays valid, exactly like the follow/whitelist drill-downs.
static Category g_mcCat = CAT_NONE;
static uint8_t  g_mcMac[MAX_DET][6];
static int      g_mcN = 0, g_mcSel = 0, g_mcOff = 0;

// Signature/flag label of a device's strongest-scoring member (row trailing text).
static const char* catRowLabel(const DevGroup* g) {
  int best = -1;
  for (int i = 0; i < g_detCount; i++)
    if (memcmp(g_dets[i].mac, g->mac, 6) == 0 && (best < 0 || g_score[i].score > g_score[best].score)) best = i;
  return best >= 0 ? detLabel(best) : "";
}

static void drawMenuCatRows(bool clear) {
  if (clear) tft.fillRect(0, LIST_Y0, tft.width(), tft.height() - LIST_Y0, TFT_BLACK);
  int vis = visibleRows();
  g_mcOff = constrain(g_mcOff, 0, max(0, g_mcN - vis));
  int rows = min(g_mcN - g_mcOff, vis);
  for (int r = 0; r < rows; r++) {
    int i = g_mcOff + r;
    const DevGroup* g = groupByMac(g_mcMac[i]);
    int y = LIST_Y0 + r * LIST_ROW_H;
    if (g) {
      const Detection& d = g_dets[g->rep];
      drawDetRow(y, tierColor(g->tier, d.source, d.channel), g->tag, groupName(g), g->bestRssi,
                 catRowLabel(g), g->tier != (int)sigdb::Tier::None, 9, false, g->whitelisted);
    } else {
      drawDetRow(y, TFT_DARKGREY, "?", "<gone>", 0, "", false, 9, false, false);
    }
    if (i == g_mcSel) tft.drawRect(0, y - 2, ICON_GUTTER_X, LIST_ROW_H, TFT_CYAN);  // BOOT-nav highlight
  }
  drawScrollIcons(g_mcOff, g_mcN, vis);
}

static void drawMenuCat() {
  tft.fillScreen(TFT_BLACK);
  drawStatusBar();
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  char b[40];
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  snprintf(b, sizeof(b), "%s: %d", kCatName[g_mcCat], g_mcN);
  tft.drawString(b, 4, 46, 1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(catDescription(g_mcCat), 4, 58, 1);   // what signatures this category matches
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("Tap a device for details", 4, 72, 1);
  drawMenuCatRows(false);
}

// Snapshot the live threat devices in category c (same filter as the readout) and open the list.
static void openMenuCat(Category c) {
  g_mcCat = c;
  int idx[MAX_DET]; int n = 0;
  for (int k = 0; k < g_groupCount && n < MAX_DET; k++) {
    const DevGroup& g = g_groups[k];
    if (g.whitelisted || g.tier < (int)sigdb::Tier::Suspect) continue;
    if (groupCategory(g) != c) continue;
    idx[n++] = k;
  }
  std::sort(idx, idx + n, [](int a, int b) {
    if (g_groups[a].tier != g_groups[b].tier) return g_groups[a].tier > g_groups[b].tier;
    return g_groups[a].bestRssi > g_groups[b].bestRssi;
  });
  for (int i = 0; i < n; i++) memcpy(g_mcMac[i], g_groups[idx[i]].mac, 6);
  g_mcN = n; g_mcSel = 0; g_mcOff = 0;
  g_screen = SCR_MENUCAT;
  listTouchReset();
  drawMenuCat();
}

static void openMenuCatDetail(const uint8_t* mac) { showDeviceDetail(mac, SCR_MENUCATDETAIL); }

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
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);  // full-width back (app convention)
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
  drawTopBar("< BACK", UI_ACCENT, TFT_CYAN);
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

// Act on the highlighted menu item (touch tap or long-press select). g_menuSel >= MENU_N selects a
// threat chip (index g_menuSel - MENU_CHIP0 into g_menuCat) -> that category's device list.
// Layout: 0 = Scan (list), MENU_N = cog/Settings, MENU_N+1 = phone/Phone Link, then the chips.
static void activateMenu() {
  if      (g_menuSel < MENU_N)        { g_screen = SCR_SCANMENU; g_scanSel = 0; drawScanMenu(); }  // Scan
  else if (g_menuSel == MENU_N)       { g_screen = SCR_SETTINGS; g_setSel = 0; drawSettings(); }   // cog
  else if (g_menuSel == MENU_N + 1)   { g_screen = SCR_APPQR;    drawAppQrScreen(); }               // phone
  else {
    int ci = g_menuSel - MENU_CHIP0;
    if (ci >= 0 && ci < g_menuCatN) openMenuCat((Category)g_menuCat[ci]);
  }
}

// Boot splash: the COINTEL-FLO wordmark (baked from resources/logo/logo.png into logo.h as an
// RGB565 blit — the TFT has no PNG decoder) over black, with a one-line tagline. Shown while the
// rest of setup() initialises; setup() holds it for a minimum SPLASH_MS before the home menu.
static constexpr uint32_t SPLASH_MS = 3000;
static void drawSplash() {
  tft.fillScreen(TFT_BLACK);
  int y = (tft.height() - LOGO_H) / 2 - 12;            // wordmark a touch above centre
  tft.setSwapBytes(true);                              // RGB565 array is big-endian for pushImage
  tft.pushImage((tft.width() - LOGO_W) / 2, y, LOGO_W, LOGO_H, COINTEL_LOGO);
  tft.setSwapBytes(false);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.drawString("counter-surveillance scanner", tft.width() / 2, y + LOGO_H + 16, 2);
  tft.setTextDatum(TL_DATUM);
}

// ---- GPS info modal (tap the status-bar GPS glyph) ---------------------------------------------
static bool g_gpsModal = false;   // the GPS popup is up, overlaying the current screen

// Repaint the current screen from its globals — used to restore the screen under the GPS modal on
// dismiss. Covers every screen with a clean no-arg redraw; the stateful device-detail screens are
// gated out of opening the modal (gpsModalAllowed) so they never reach the default here.
static void redrawScreen() {
  switch (g_screen) {
    case SCR_SCAN:         render();           break;
    case SCR_APPQR:        drawAppQrScreen();  break;
    case SCR_SETTINGS:     drawSettings();     break;
    case SCR_SCANMENU:     drawScanMenu();     break;
    case SCR_SCANSETTINGS: drawScanSettings(); break;
    case SCR_PICKLOG:      drawPickLog();      break;
    case SCR_SCANVIEWER:   drawViewer();       break;
    case SCR_SORT:         drawSort();         break;
    case SCR_FILTER:       drawFilter();       break;
    case SCR_FOLLOWLIST:   drawFollowList();   break;
    case SCR_FOLLOWACTION: drawFollowAction(); break;
    case SCR_WHITELIST:    drawWhitelist();    break;
    case SCR_WLADD:        drawWlAdd();         break;
    case SCR_WLRULE:       drawWlRule();        break;
    case SCR_SCANROW:      drawScanRow();      break;
    case SCR_SCANCONFIRM:  drawScanConfirm();  break;
    case SCR_SCANFILTER:   drawScanFilter();   break;
    case SCR_MENUCAT:      drawMenuCat();      break;
    case SCR_MENU:
    default:               drawMenu();         break;
  }
}

// The device-detail screens render from transient state (an Explore row / a built field list) with
// no clean no-arg repaint, so the GPS modal isn't offered there (the glyph tap is ignored).
static bool gpsModalAllowed() {
  switch (g_screen) {
    case SCR_DETAIL: case SCR_SCANDETAIL: case SCR_FOLLOWDETAIL: case SCR_MENUCATDETAIL: return false;
    default: return true;
  }
}

// Centered popup: GPS source (phone / onboard module / none) + the current coordinates + sat count.
static void drawGpsModal() {
  const int bw = 196, bh = 124, bx = (tft.width() - bw) / 2, by = 80;
  tft.fillRoundRect(bx, by, bw, bh, 8, TFT_BLACK);
  tft.drawRoundRect(bx,     by,     bw,     bh,     8, UI_ACCENT);
  tft.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 8, UI_ACCENT);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(UI_ACCENT, TFT_BLACK);
  tft.drawString("GPS", bx + bw / 2, by + 8, 4);
  int src = posSource();
  const char* srcName = src == 3 ? "Last known" : src == 2 ? "Module (NEO-6M)" : src == 1 ? "Phone (linked)" : "No fix";
  uint16_t    sc      = src == 3 ? TFT_ORANGE   : src == 2 ? TFT_CYAN          : src == 1 ? TFT_WHITE         : TFT_DARKGREY;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Source:", bx + 12, by + 44, 2);
  tft.setTextColor(sc, TFT_BLACK);
  tft.drawString(srcName, bx + 74, by + 44, 2);
  char b[32];
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  if (posHasFix()) {
    snprintf(b, sizeof(b), "Lat: %.6f", posLat()); tft.drawString(b, bx + 12, by + 68, 2);
    snprintf(b, sizeof(b), "Lon: %.6f", posLon()); tft.drawString(b, bx + 12, by + 88, 2);
    if (src == 2) { snprintf(b, sizeof(b), "Satellites: %u", (unsigned)g_onbSats);
                    tft.drawString(b, bx + 12, by + 106, 1); }
    if (src == 3) { snprintf(b, sizeof(b), "No live fix - held %lus", (unsigned long)posFixAgeS());
                    tft.drawString(b, bx + 12, by + 106, 1); }
  } else {
    tft.drawString("No position fix yet.", bx + 12, by + 72, 2);
    if (g_onbSats) { snprintf(b, sizeof(b), "Module sats in view: %u", (unsigned)g_onbSats);
                     tft.drawString(b, bx + 12, by + 94, 1); }
  }
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("tap to close", bx + bw / 2, by + bh - 4, 1);
  tft.setTextDatum(TL_DATUM);
}

// Open on a fresh tap of the GPS glyph; dismiss on the next tap or any BOOT event. Returns true
// while the modal owns input (so loop() consumes the event and skips the per-screen handler). Call
// right after serviceTouchLock() (which already swallows everything while the display is locked).
static bool serviceGpsModal(BtnEv ev) {
  static bool prev = false;
  bool now  = g_touchOk && g_touch.touched();
  bool rise = now && !prev;
  prev = now;
  if (g_gpsModal) {                       // open: any fresh tap or BOOT closes it
    if (rise || ev != BTN_NONE) { g_gpsModal = false; redrawScreen(); }
    return true;                          // own input until closed
  }
  if (rise && gpsModalAllowed()) {        // closed: did the tap land on the GPS glyph?
    int16_t sx, sy, z;
    int xo = gpsX();
    if (g_touch.getScreen(tft, sx, sy, z) && sy >= 0 && sy <= 21 && sx >= xo - 5 && sx <= xo + 20) {
      g_gpsModal = true;
      drawGpsModal();
      return true;
    }
  }
  return false;
}

// ---- CYD implementation of the host_common board/UI interfaces (host_board.h / host_ui.h) --------
// Screens that hold indices into the live tables (follow / whitelist / scan-row / category drill-downs)
// versus the SD-reading Explore screens: the headless cycle and the phone scan-start gate on these.
static bool screenIsDrill() {
  return g_screen == SCR_FOLLOWLIST || g_screen == SCR_FOLLOWACTION || g_screen == SCR_FOLLOWDETAIL ||
         g_screen == SCR_WHITELIST  || g_screen == SCR_WLADD       || g_screen == SCR_WLRULE ||
         g_screen == SCR_SCANROW    || g_screen == SCR_SCANDETAIL  || g_screen == SCR_SCANCONFIRM ||
         g_screen == SCR_SCANFILTER || g_screen == SCR_MENUCAT     || g_screen == SCR_MENUCATDETAIL;
}
static bool screenIsExplore() {
  return g_screen == SCR_PICKLOG || g_screen == SCR_SCANVIEWER || g_screen == SCR_DETAIL ||
         g_screen == SCR_SORT    || g_screen == SCR_FILTER;
}

void hostboard::begin() {
  initCommon();       // LED, TFT + backlight PWM, link UART
  power::init(BAT_ADC_PIN);  // battery monitor on GPIO35 (voltage-only)
  power::sample();    // seed the reading so the status bar shows a level immediately
}
void hostboard::sample() { power::sample(); }
bool hostboard::sdBegin() {
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  return SD.begin(SD_CS_PIN, sdSPI) && SD.cardType() != CARD_NONE;
}
void    hostboard::setScanLed(bool r, bool g, bool b) { setLed(r, g, b); }
int     hostboard::batteryPercent() { return power::percent(); }
uint8_t hostboard::brightness()     { return g_brightness; }
bool    hostboard::rtcPresent()     { return false; }  // no DS3231 on the CYD
bool    hostboard::foregroundScan() { return g_screen == SCR_SCAN; }
const char* hostboard::boardTag() { return "CYD"; }
bool    hostboard::pipelineAllowed() { return !screenIsDrill() && !screenIsExplore(); }

void hostui::onScanCycle(const ScanCycleResult&) {
  if (!g_viewFrozen) buildScanView();  // a paused view keeps its snapshot while scanning continues
  if (g_screen == SCR_SCAN)
    updateScan();  // per-cycle partial repaint (status bar + info band + list); chrome persists from entry
}
void hostui::onBusyMiss() {
  if (g_screen == SCR_SCAN) drawStatusBar();  // link dot + clock only (never touches the list region)
}
void hostui::onDownloadState(bool, bool) { drawDownloadScreen(); }  // swap the QR (runDownload() does this directly)
void hostui::onStatusPushed() {}
bool hostui::exploreHolds(const char* path) { return ::exploreHolds(path); }

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[CYD] boot: scanner + phone link + SD");
  hostboard::begin(); // initCommon() + battery monitor, seeded so the status bar shows a level immediately
  loadBrightness();
  loadSrcMask();
  loadBgScan();
  g_scanActive = g_bgScan;  // background scan resumes at boot (first cycle runs right away)
  applyBrightness();
  drawSplash();                      // show the logo while the slow init below runs
  uint32_t splashStart = millis();
  initSD();
  sigdb::begin();  // load /signatures.csv (seeds it if absent) or fall back
  whitelist::begin();  // load /whitelist.csv (seeds a commented header if absent)
  g_touch.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN, TOUCH_IRQ_PIN);
  g_touchOk = g_touch.loadCal();
  phone::begin(scannerName());  // CFLO-CYD-<word>, unique per unit (see scannerName)
  g_haveOwnMac = phone::ownMac(g_ownMac);  // for self-detection filtering
  g_linkOk = pingC5();  // check the C5 link so the menu dot is correct before any scan
  if (g_linkOk) g_lastLinkOkMs = millis();  // seed link stickiness for the first scan
  Serial.printf("[CYD] ready (link=%s, touch=%s, bright=%d%%)\n",
                g_linkOk ? "up" : "down", g_touchOk ? "cal" : "uncal", g_brightness);
  while (millis() - splashStart < SPLASH_MS) delay(10);  // keep the splash up a beat (init often outlasts it)
  drawMenu();      // start on the home menu
}

void loop() {
  // Phone commands are honored from any screen. The board-agnostic dispatch (BLE transfers, whitelist,
  // session/time-sync rename, ...) lives in host_common; below are the CYD's UI reactions to it.
  PhoneCmdFlags pf;
  core::servicePhoneCommands(pf);
  if (pf.brightnessChanged) {                              // web-app brightness slider
    g_brightness = (uint8_t)constrain(pf.brightnessPct, 10, 100);
    applyBrightness(); saveBrightness();
  }
  if (pf.lockChanged) {                                    // web-app lock / unlock the display
    drawStatusBar();                                       // repaint the padlock immediately
    pushStatus();                                          // reflect lock= back to the phone
  }
  if (pf.srcMaskChanged) {                                 // web-app source toggles (all-off ignored in core)
    if (g_screen == SCR_SCANSETTINGS) drawScanSettings();
    pushStatus();
  }
  if (pf.newSession) pushStatus();                         // web-app "New Session"
  if (pf.sessionChanged && g_screen == SCR_SCANMENU) drawScanMenu();  // keep the Scan-menu session label live

  // Phone-initiated Wi-Fi download overrides the current screen while active.
  if (pf.downloadEnter) { exploreFree(); runDownload(); return; }
  if (pf.downloadExit) {  // just left download mode (core tore down the AP + restored the bg scan): repaint
    g_viewFrozen = false;
    g_screen = SCR_MENU; g_menuSel = 0; drawMenu(); pushStatus();
  }

  // Low-frequency /logs/ cap check (~10 min). Never while a Wi-Fi/phone download is active (both
  // returned above) and not on the SD-reading Explore screens (avoid contending for the card).
  core::tickLogCap(screenIsExplore());

  // Phone can start/stop the scan remotely (single toggle in the web app). Ignore a start while
  // drilling the follow or whitelist screens so a stray phone toggle can't reset follow state /
  // abandon the snapshot mid-view; scanStartRequested() is consume-on-read, so deferring it
  // leaves it pending until we return to SCR_SCAN.
  pf.acceptScanStart = !screenIsDrill();
  core::servicePhoneScan(pf);
  if (pf.scanStarted) {
    if (g_screen != SCR_SCAN) { g_scrollOffset = 0; resetFollow(); g_viewFrozen = false; showScan(); }
    else render();      // already on the scanner: repaint the START->STOP state
    pushStatus();
  }
  // Stop ends the scan but stays on the scanner screen (it just goes idle), so the user can review
  // the list or restart.
  if (pf.scanStopped) {
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
  if (g_bgScan && g_scanActive && !hostboard::foregroundScan() && hostboard::pipelineAllowed() &&
      !webshare::active() &&
      millis() - g_lastCycleMs >= (g_linkOk ? SCAN_CYCLE_MS : SCAN_CYCLE_DOWN_MS)) {
    g_lastCycleMs = millis();
    core::runScanCycle();
  }

  BtnEv ev = buttonEvent();

  // Bag-safe input lock: 2 taps on the status-bar padlock toggle it; while locked, swallow all
  // input (touch + BOOT) so a bagged device can't navigate itself. The background scan already ran
  // above for non-SCR_SCAN screens; keep the live SCAN screen polling too so a bagged scan keeps
  // capturing + logging (its cycle normally runs inside the SCR_SCAN case, which we now skip).
  if (serviceTouchLock()) {
    if (g_screen == SCR_SCAN && g_scanActive && millis() - g_lastCycleMs >= SCAN_CYCLE_MS) {
      g_lastCycleMs = millis();
      core::runScanCycle();   // renders via updateScan()
    } else if (g_screen == SCR_MENU) {
      // HOME's live readout is normally repainted by the SCR_MENU idle branch, which the early
      // return above skips while locked — so the tiles froze on screen even though the headless
      // bg cycle kept the data current. Refresh the status bar + threat tiles on the idle cadence
      // (the bg cycle already pushed status + rebuilt g_groups; this just repaints from them).
      static uint32_t lastLockedHome = 0;
      if (millis() - lastLockedHome > 2000) {
        lastLockedHome = millis();
        drawStatusBar();
        drawMenuThreats();
      }
    }
    delay(10);
    return;
  }

  // GPS info modal: tap the status-bar GPS glyph to open it, tap/BOOT to close. While it's up it
  // owns input. The scan cycle is NOT run underneath (updateScan()'s list repaint would paint over
  // the popup); it's a brief tap-to-peek, and capture resumes the moment it's dismissed.
  if (serviceGpsModal(ev)) { delay(10); return; }

  switch (g_screen) {
    case SCR_MENU: {
      int t = tappedRow(MENU_N);
      if (t >= 0)               { g_menuSel = t; activateMenu(); return; }  // touch select (Scan button)
      int it = homeIconTapped();
      if (it >= 0)              { g_menuSel = MENU_N + it; activateMenu(); return; }  // cog / phone icon
      int ct = menuChipTapped();
      if (ct >= 0)              { g_menuSel = MENU_CHIP0 + ct; activateMenu(); return; }  // touch select (threat chip)
      if (homeAlertTapped() && followAlertN() > 0) { g_followFromHome = true; openFollowList(); return; }  // alert band -> follow drill-down
      int navN = MENU_CHIP0 + g_menuCatN;  // BOOT walks Scan, the two icons, then the threat chips
      if      (ev == BTN_SHORT) { g_menuSel = (g_menuSel + 1) % navN; drawMenu(); }
      else if (ev == BTN_LONG)  { activateMenu(); }
      else {  // idle: re-check the C5 link and refresh the status bar (dot + clock)
        static uint32_t lastPing = 0;
        static bool lastConn = false;
        if (phone::connected() != lastConn) {  // phone link came up/down: recolour the phone icon
          lastConn = phone::connected();
          drawStatusBar(); drawHomeIcons();  // no full-screen flicker
        }
        if (millis() - lastPing > 2000) {
          lastPing = millis();
          // The background-scan cycle (requestScan) already refreshes g_linkOk *stickily* every
          // ~2 s while scanning, so an extra ping here is redundant — and, being non-sticky, it
          // used to flicker the link dot red whenever it collided with the C5's scan-stream busy
          // window (the "~2 s link drop"). Only ping when the bg scan is idle, and apply the same
          // LINK_STICKY_MS grace so a single missed reply can't false-drop the link.
          if (!(g_bgScan && g_scanActive)) {
            if (pingC5()) g_lastLinkOkMs = millis();
            g_linkOk = (millis() - g_lastLinkOkMs < LINK_STICKY_MS);
          }
          drawStatusBar();  // just the top bar — no full-screen flicker
          drawMenuThreats();  // live bg-scan tier counts (own band only)
          drawHomeAlerts();   // follow / deauth / evil-twin banner (own band; clears itself)
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

    case SCR_MENUCAT: {  // devices in one threat category (from a HOME chip)
      if (topBarTapped()) { g_screen = SCR_MENU; g_menuSel = 0; drawMenu(); return; }  // < BACK
      int t = listTouch(g_mcN, &g_mcOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawMenuCatRows(true); });
      if (t >= 0) { g_mcSel = t; openMenuCatDetail(g_mcMac[t]); return; }
      if (g_mcN > 0 && ev == BTN_SHORT) {  // BOOT tap: move the highlight (wraps)
        g_mcSel = (g_mcSel + 1) % g_mcN;
        if (g_mcSel < g_mcOff) g_mcOff = g_mcSel;
        if (g_mcSel >= g_mcOff + visibleRows()) g_mcOff = g_mcSel - visibleRows() + 1;
        drawMenuCatRows(true);
      } else if (ev == BTN_LONG) {
        if (g_mcN > 0) openMenuCatDetail(g_mcMac[g_mcSel]);
        else { g_screen = SCR_MENU; g_menuSel = 0; drawMenu(); }
      }
      delay(20);
      return;
    }

    case SCR_MENUCATDETAIL: {
      if (topBarTapped() || ev == BTN_LONG) { g_screen = SCR_MENUCAT; listTouchReset(); drawMenuCat(); return; }
      if (ev == BTN_SHORT) {  // BOOT tap: page down (wraps to the top)
        int maxOff = max(0, g_detN - visibleRows());
        g_detOff = (g_detOff >= maxOff) ? 0 : min(g_detOff + visibleRows(), maxOff);
        drawDetailList(true);
      }
      listTouch(g_detN, &g_detOff, visibleRows(), LIST_Y0, LIST_ROW_H, []() { drawDetailList(true); });
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
        if (bn && followAlertN() > 0 && ev != BTN_LONG) { g_followFromHome = false; openFollowList(); return; } }
      // BOOT short-press opens the follow drill-down (so it's reachable without touch).
      if (ev == BTN_SHORT && followAlertN() > 0) { g_followFromHome = false; openFollowList(); return; }
      // Back via the on-screen button (touch) or a long BOOT hold; asks first while scanning.
      if (ev == BTN_LONG || topBarTapped()) { backFromScan(); return; }
      int rt = handleScanTouch();  // scroll / row tap
      if (rt >= 0) { openScanRow(rt); return; }
      if (g_scanActive && millis() - g_lastCycleMs >= SCAN_CYCLE_MS) { g_lastCycleMs = millis(); core::runScanCycle(); }
      delay(20);
      return;
    }
  }
}
#endif

// main.cpp — mini-build BRING-UP TEST for the XIAO ESP32-S3 host (env:xiao_s3_bringup).
//
// Flash this to the S3 instead of the real host firmware while wiring the mini build one module at a
// time. The XIAO-C5 keeps its NORMAL scanner firmware (env:xiao_c5), so the real link is exercised.
// Watch the S3's USB console (pio device monitor -e xiao_s3_bringup); every few seconds it prints one
// line per stage and a PASS / WAIT / FAIL verdict:
//
//   [LINK] S3 <-> C5 UART: Ping/Pong health, corrupt-frame count, a full detection-burst check,
//          and C5 reset detection (its uptime going backwards = brownout / power glitch).
//   [RTC ] DS3231 on I2C D4/D5: bus scan, time, oscillator-stop flag, die temperature.
//   [GPS ] NEO-6M via the C5's Status frame: fix / sats / position / UTC time.
//   [PWR ] S3 reset reason (BROWNOUT is flagged loudly) + optional radio stress load.
//
// Serial commands (type + Enter):  h help · c clear counters · i I2C rescan ·
//   s <utc-epoch> set the RTC · x toggle S3 Wi-Fi stress (worst-case current on the shared 3V3).
//
// Receive-only on the RF side except the optional stress mode, which is ordinary active Wi-Fi
// scanning by our own radio (in scope — see CLAUDE.md). Never part of the shipped firmware.
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>
#include "../xiao_s3/pins.h"
#include "link_protocol.h"
#include "../host_common/rtc.h"

using namespace link_protocol;

static HardwareSerial LinkSerial(1);
static FrameParser    parser;

// --- Link stats ---------------------------------------------------------------------------------
static uint32_t pingOk = 0, pingMiss = 0, pingCorrupt = 0, rttMax = 0, rttSum = 0;
static uint32_t lastPongMs = 0;
static uint32_t burstRuns = 0, burstBad = 0, lastBurstExpect = 0, lastBurstGot = 0;
static uint32_t c5Resets = 0, c5LastUptime = 0;
static bool     haveStatus = false;
static Status   lastSt{};
static uint32_t lastStatusMs = 0;

// --- RTC / I2C -----------------------------------------------------------------------------------
static bool    rtcPresent = false;
static uint8_t i2cFound[16];
static int     i2cCount = 0;

// --- Power / stress ------------------------------------------------------------------------------
static esp_reset_reason_t bootReason;
static bool stress = false;

static void sendFrame(uint8_t type, const void* payload, uint16_t len) {
  uint8_t out[MAX_FRAME];
  uint16_t n = encodeFrame(type, (const uint8_t*)payload, len, out);
  LinkSerial.write(out, n);
  LinkSerial.flush();
}

static void drainLink() {
  while (LinkSerial.available()) LinkSerial.read();
  parser.reset();
}

static void noteStatus(const Status& st) {
  if (haveStatus && st.uptime_ms + 2000 < c5LastUptime) c5Resets++;  // uptime went backwards: C5 rebooted
  c5LastUptime = st.uptime_ms;
  lastSt = st; haveStatus = true; lastStatusMs = millis();
}

// Ping: a MISS (no byte at all) is usually the C5's brief Wi-Fi-scan busy window, so occasional misses
// are normal. CORRUPT (bytes arrived but never a valid Pong) points at wiring/noise and should stay 0.
static void doPing() {
  drainLink();
  uint32_t t0 = millis();
  sendFrame((uint8_t)Command::Ping, nullptr, 0);
  bool anyByte = false;
  while (millis() - t0 < 150) {
    while (LinkSerial.available()) {
      anyByte = true;
      if (parser.feed(LinkSerial.read()) && parser.type() == (uint8_t)Reply::Pong) {
        uint32_t rtt = millis() - t0;
        pingOk++; rttSum += rtt; if (rtt > rttMax) rttMax = rtt;
        lastPongMs = millis();
        return;
      }
    }
    delay(1);
  }
  if (anyByte) pingCorrupt++; else pingMiss++;
}

static void doGetStatus() {
  drainLink();
  sendFrame((uint8_t)Command::GetStatus, nullptr, 0);
  uint32_t t0 = millis();
  while (millis() - t0 < 150) {
    while (LinkSerial.available())
      if (parser.feed(LinkSerial.read()) && parser.type() == (uint8_t)Reply::Status &&
          parser.length() == sizeof(Status)) {
        Status st; memcpy(&st, parser.payload(), sizeof(st));
        noteStatus(st);
        return;
      }
    delay(1);
  }
}

// Burst: StartScan makes the C5 dump its whole detection table — the large, back-to-back frames that
// expose a marginal wire (small Pings can pass while bursts drop). The opening Status(scanning=1)
// announces how many Detection frames follow, so a short count means frames were lost on the wire.
static void doBurst() {
  drainLink();
  ScanConfig cfg{};
  cfg.sources  = MASK_WIFI24 | MASK_WIFI5 | MASK_BLE | MASK_PROBE;  // C5 defaults (15.4 stays opt-in)
  cfg.dwell_ms = 0;
  sendFrame((uint8_t)Command::StartScan, &cfg, sizeof(cfg));
  uint32_t t0 = millis(), expect = 0, got = 0;
  bool started = false, anyByte = false;
  while (millis() - t0 < 4000) {
    while (LinkSerial.available()) {
      anyByte = true;
      if (!parser.feed(LinkSerial.read())) continue;
      if (parser.type() == (uint8_t)Reply::Status && parser.length() == sizeof(Status)) {
        Status st; memcpy(&st, parser.payload(), sizeof(st));
        noteStatus(st);
        if (st.scanning) { started = true; expect = st.seen_total; }
        else if (started) {
          burstRuns++;
          lastBurstExpect = expect; lastBurstGot = got;
          if (got != expect) burstBad++;
          return;
        }
      } else if (parser.type() == (uint8_t)Reply::Detection) {
        got++;
      }
    }
    if (!anyByte && millis() - t0 > 300) return;  // C5 busy window / no link: skip, try next time
    delay(1);
  }
  if (started) { burstRuns++; burstBad++; lastBurstExpect = expect; lastBurstGot = got; }  // never closed
}

// Pin-level probe of the link wires, independent of the UART protocol. A running C5 holds its TX
// (our RX, D7) idle HIGH, so reading D7 with a pull-down then a pull-up tells driven-high (C5 alive +
// wire good) from floating (open wire / wrong pad / C5 not running) from driven-low (C5 held in reset,
// or the line shorted to GND). Also checks D6/D7 aren't bridged to each other.
static void probePins() {
  LinkSerial.end();
  // Sample for ~3 s: a C5 stuck in a brownout/reboot loop alternates driven-high (app running) and
  // floating (in reset / ROM boot), which a single read can't see.
  uint32_t nHigh = 0, nFloat = 0, nLow = 0;
  int withDown = 0, withUp = 0;
  for (uint32_t t0 = millis(); millis() - t0 < 3000;) {
    pinMode(LINK_RX_PIN, INPUT_PULLDOWN); delayMicroseconds(300);
    withDown = digitalRead(LINK_RX_PIN);
    pinMode(LINK_RX_PIN, INPUT_PULLUP);   delayMicroseconds(300);
    withUp = digitalRead(LINK_RX_PIN);
    if (withDown && withUp) nHigh++; else if (!withDown && !withUp) nLow++; else nFloat++;
    delayMicroseconds(400);
  }
  bool bridged = false;
  if (nFloat && nHigh) {  // mixed: the C5 is rebooting
    pinMode(LINK_RX_PIN, INPUT);
    LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
    drainLink();
    Serial.printf("[PROBE] D7 over 3 s: driven-high %lu%%, floating %lu%%, low %lu%%\n"
                  "        => the C5 is REBOOTING in a loop (its TX keeps dropping out). Almost certainly a\n"
                  "           brownout: the shared 3V3 sags when the C5 starts its radios.\n",
                  nHigh * 100 / (nHigh + nFloat + nLow), nFloat * 100 / (nHigh + nFloat + nLow),
                  nLow * 100 / (nHigh + nFloat + nLow));
    return;
  }
  withDown = nHigh ? 1 : 0; withUp = nLow ? 0 : 1;  // steady state for the verdicts below
  if (!withDown && withUp) {  // floating: safe to drive D6 and see if it shows up on D7
    pinMode(LINK_TX_PIN, OUTPUT);
    digitalWrite(LINK_TX_PIN, LOW);  delay(2); bool lo = digitalRead(LINK_RX_PIN) == 0;
    digitalWrite(LINK_TX_PIN, HIGH); delay(2); bool hi = digitalRead(LINK_RX_PIN) == 1;
    bridged = lo && hi;
  }
  pinMode(LINK_RX_PIN, INPUT);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  drainLink();

  Serial.print("[PROBE] D7 (S3 RX <- C5 GPIO23 TX): ");
  if (withDown && withUp)
    Serial.println("driven HIGH = OK. The C5 is running and its TX wire reaches D7.\n"
                   "        If pings still fail, the S3 -> C5 direction (D6 -> C5 GPIO24/pad D5) is the suspect.");
  else if (!withDown && !withUp)
    Serial.println("driven LOW. The C5 is held in reset/brownout, or D7 is shorted to GND.");
  else if (bridged)
    Serial.println("FLOATING, and D6 and D7 are connected to EACH OTHER: solder bridge, or both wires on the same C5 pad.");
  else
    Serial.println("FLOATING = nothing drives it. Either the C5 isn't running (no 3V3 at its pad, or not flashed\n"
                   "        with xiao_c5), or the D7 wire lands on the wrong C5 pad (must be GPIO23 = pad D4, silk 'SDA').");
}

// --- RTC -----------------------------------------------------------------------------------------
static bool readReg(uint8_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(0x68);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)0x68, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static void i2cScan() {
  i2cCount = 0;
  for (uint8_t a = 1; a < 127 && i2cCount < 16; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) i2cFound[i2cCount++] = a;
  }
  rtcPresent = rtc::begin(Wire);
}

static void fmtUtc(uint32_t ep, char* out, size_t n) {
  if (!ep) { snprintf(out, n, "(unset)"); return; }
  time_t t = (time_t)ep; struct tm tm; gmtime_r(&t, &tm);
  strftime(out, n, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

// --- Report --------------------------------------------------------------------------------------
static const char* reasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "usb";
    default:               return "other";
  }
}

static void report() {
  uint32_t now = millis();
  uint32_t tries = pingOk + pingMiss + pingCorrupt;

  // LINK
  const char* lv;
  if (pingOk == 0 && tries >= 5)                       lv = "FAIL (no Pong at all - power, TX/RX swapped, or open wire)";
  else if (pingCorrupt || burstBad || c5Resets)        lv = "FAIL (see corrupt/burst/resets)";
  else if (pingOk && now - lastPongMs > 10000)         lv = "FAIL (link went silent)";
  else if (pingOk && burstRuns >= 3)                   lv = "PASS";
  else                                                 lv = "WAIT";
  Serial.printf("[LINK] %s | pong %lu  miss %lu  corrupt %lu  rtt avg %lums max %lums | burst %lu/%lu ok (last %lu/%lu) | C5 up %lus resets %lu\n",
                lv, pingOk, pingMiss, pingCorrupt, pingOk ? rttSum / pingOk : 0, rttMax,
                burstRuns - burstBad, burstRuns, lastBurstGot, lastBurstExpect,
                haveStatus ? lastSt.uptime_ms / 1000 : 0, c5Resets);

  // RTC
  char addrs[80] = ""; size_t p = 0;
  for (int i = 0; i < i2cCount && p < sizeof(addrs) - 6; i++) p += snprintf(addrs + p, sizeof(addrs) - p, "0x%02X ", i2cFound[i]);
  if (!rtcPresent) {
    Serial.printf("[RTC ] %s | i2c: %s\n", i2cCount ? "FAIL (devices on bus but none at 0x68)" : "WAIT (nothing on I2C - not wired yet?)",
                  i2cCount ? addrs : "none");
  } else {
    uint8_t st = 0, t[2] = {0, 0};
    bool ok = readReg(0x0F, &st, 1) && readReg(0x11, t, 2);
    float tempC = (int8_t)t[0] + (t[1] >> 6) * 0.25f;
    char ts[32]; fmtUtc(rtc::readUtc(), ts, sizeof(ts));
    bool osf = st & 0x80;
    const char* v = !ok ? "FAIL (0x68 answers but register read failed)"
                  : (tempC < -10 || tempC > 60) ? "FAIL (implausible temperature - bad read)"
                  : osf ? "PASS (time not set - use: s <epoch>)" : "PASS";
    Serial.printf("[RTC ] %s | %s  temp %.2fC  osf=%d | i2c: %s\n", v, ts, tempC, osf ? 1 : 0, addrs);
  }

  // GPS (via the C5's Status). No fix indoors is normal; the C5 console's "[C5] gps:" line proves NMEA traffic.
  if (!haveStatus || now - lastStatusMs > 10000) {
    Serial.println("[GPS ] WAIT (no Status from C5 - fix the link first)");
  } else if (lastSt.gpsFix) {
    char ts[32]; fmtUtc(lastSt.gpsEpoch, ts, sizeof(ts));
    Serial.printf("[GPS ] PASS | %s fix  sats %u  %.6f, %.6f  alt %dm  %s\n", lastSt.gpsFix == 2 ? "3D" : "2D",
                  lastSt.gpsSats, lastSt.gpsLat / 1e7, lastSt.gpsLon / 1e7, lastSt.gpsAltM, ts);
  } else {
    Serial.println("[GPS ] WAIT (no fix yet - go near a window/outside; cold start can take 1-5 min)");
  }

  Serial.printf("[PWR ] S3 boot reason: %s%s | S3 Wi-Fi stress: %s | heap %lu\n\n", reasonStr(bootReason),
                bootReason == ESP_RST_BROWNOUT ? "  <-- 3V3/battery sagged!" : "",
                stress ? "ON" : "off", (unsigned long)ESP.getFreeHeap());
}

static void help() {
  Serial.println(
      "\nCommands: h help | c clear counters | i I2C rescan | p probe link pins | s <utc-epoch> set RTC |\n"
      "          x toggle S3 Wi-Fi stress\n"
      "  (get the epoch on the PC: PowerShell  [DateTimeOffset]::UtcNow.ToUnixTimeSeconds() )\n");
}

static void serviceSerial() {
  static char buf[32]; static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (n < sizeof(buf) - 1) buf[n++] = c; continue; }
    if (!n) continue;
    buf[n] = 0; n = 0;
    switch (buf[0]) {
      case 'h': help(); break;
      case 'c':
        pingOk = pingMiss = pingCorrupt = rttMax = rttSum = 0;
        burstRuns = burstBad = lastBurstExpect = lastBurstGot = c5Resets = 0;
        Serial.println("counters cleared");
        break;
      case 'i': i2cScan(); Serial.println("I2C rescanned"); break;
      case 'p': probePins(); break;
      case 's': {
        uint32_t ep = strtoul(buf + 1, nullptr, 10);
        if (!rtcPresent) Serial.println("no RTC at 0x68");
        else if (ep < 1700000000UL) Serial.println("usage: s <utc-epoch>, e.g. s 1792500000");
        else { rtc::writeUtc(ep); Serial.println("RTC set"); }
        break;
      }
      case 'x':
        stress = !stress;
        if (stress) { WiFi.mode(WIFI_STA); WiFi.scanNetworks(true); }
        else { WiFi.scanDelete(); WiFi.mode(WIFI_OFF); }
        Serial.printf("S3 Wi-Fi stress %s\n", stress ? "ON (continuous active scan)" : "off");
        break;
      default: help(); break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);  // give the USB console a moment; never blocks for good
  bootReason = esp_reset_reason();
  Serial.printf("\n=== mini-build bring-up test (S3 host) — boot reason: %s ===\n", reasonStr(bootReason));
  if (bootReason == ESP_RST_BROWNOUT) Serial.println("!!! S3 last reset was a BROWNOUT: the supply sagged !!!");

  LinkSerial.setRxBufferSize(2048);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  i2cScan();
  WiFi.mode(WIFI_OFF);
  help();
}

void loop() {
  static uint32_t lastPing = 0, lastStat = 0, lastBurst = 0, lastRep = 0;
  uint32_t now = millis();
  serviceSerial();
  // One link transaction at a time (the protocol is strictly request/response).
  if (now - lastBurst >= 10000)      { lastBurst = now; doBurst(); }
  else if (now - lastStat >= 2000)   { lastStat = now;  doGetStatus(); }
  else if (now - lastPing >= 500)    { lastPing = now;  doPing(); }
  if (stress && WiFi.scanComplete() != WIFI_SCAN_RUNNING) { WiFi.scanDelete(); WiFi.scanNetworks(true); }
  if (now - lastRep >= 3000) {
    lastRep = now; report();
    static uint32_t lastProbe = 0;  // link dead: run the pin probe automatically every ~15 s
    if (pingOk == 0 && pingMiss + pingCorrupt >= 5 && now - lastProbe >= 15000) { lastProbe = now; probePins(); Serial.println(); }
  }
  delay(5);
}

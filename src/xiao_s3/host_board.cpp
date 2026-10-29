// host_board.cpp — hostboard:: implementation for the headless XIAO ESP32-S3 Sense host.
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "pins.h"
#include "../host_common/host_board.h"
#include "../host_common/host_link.h"
#include "../host_common/host_core.h"
#include <Wire.h>
#include <sys/time.h>
#include <Preferences.h>
#include "../host_common/power.h"
#include "../host_common/rtc.h"

extern uint8_t g_brightness;  // main.cpp: reported in STATUS only (no backlight)
extern bool    g_fgScan;      // main.cpp: a phone-started ("G") foreground scan is running

static SPIClass sdSPI(FSPI);
static bool s_rtcPresent = false;

#ifdef MINI_RGB_LED
// Per-colour PWM duty (0..255) to balance the three dice. Red (470 ohm) is by far the weakest; at full
// duty green/blue swamp it (magenta read purple, yellow read green). Tuned by eye on mini #2 (2026-10-27):
// R+B 255/15 = true magenta, R+G 255/40 = true yellow.
static const uint8_t kLedDuty[3] = { 255, 40, 15 };
static const uint8_t kLedPin[3]  = { LED_R_PIN, LED_G_PIN, LED_B_PIN };

// Per-LedColor channel levels (0..255, scaled by kLedDuty). Amber = yellow with ~40% of its green.
static const uint8_t kLedLevel[][3] = {
  {   0,   0,   0 },  // Off
  { 255,   0,   0 },  // Red
  {   0, 255,   0 },  // Green
  {   0,   0, 255 },  // Blue
  { 255,   0, 255 },  // Magenta
  {   0, 255, 255 },  // Cyan
  { 255, 100,   0 },  // Amber
};

// Overall brightness % (web Settings "LED brightness", CMD "I:", NVS cydui/ledbri). 100 = the tuned duty
// above; the default is 5 (25 was still glaring in the dark, and 5 stays visible in daylight -- tested on
// mini #2, 2026-10-28). PWM is 12-bit so the dim green/blue channels keep enough steps to hold the colour
// balance when scaled down.
static constexpr int kLedPwmBits = 12, kLedPwmMax = (1 << kLedPwmBits) - 1;
static constexpr uint8_t kLedDefaultPct = 5;
static uint8_t  s_ledPct = kLedDefaultPct;
static LedColor s_ledColor = LedColor::Off;

static void writeRgb(LedColor c) {  // active high (common cathode)
  s_ledColor = c;
  const uint8_t* lv = kLedLevel[(int)c];
  for (int i = 0; i < 3; i++) {
    uint32_t d = (uint32_t)kLedDuty[i] * lv[i] * s_ledPct;           // <= 255*255*100
    uint32_t v = (uint32_t)((uint64_t)d * kLedPwmMax / (255u * 255u * 100u));
    if (d && !v) v = 1;  // a lit channel never rounds to fully off
    ledcWrite(kLedPin[i], v);
  }
}
#endif

int hostboard::ledBrightness() {
#ifdef MINI_RGB_LED
  return s_ledPct;
#else
  return -1;
#endif
}
void hostboard::setLedBrightness(int pct) {
#ifdef MINI_RGB_LED
  pct = constrain(pct, 0, 100);
  if (pct == s_ledPct) return;  // skip a no-op NVS write
  s_ledPct = (uint8_t)pct;
  writeRgb(s_ledColor);         // re-apply the current colour at the new level
  Preferences p; p.begin("cydui", false); p.putUChar("ledbri", s_ledPct); p.end();
#else
  (void)pct;
#endif
}

void hostboard::begin() {
#ifdef MINI_RGB_LED
  { Preferences p; p.begin("cydui", true); s_ledPct = p.getUChar("ledbri", kLedDefaultPct); p.end();
    if (s_ledPct > 100) s_ledPct = kLedDefaultPct; }
  for (int i = 0; i < 3; i++) ledcAttach(kLedPin[i], 5000, kLedPwmBits);
  // Boot self-test R -> G -> B so a miswired or dead leg is obvious at power-up.
  writeRgb(LedColor::Red);   delay(250);
  writeRgb(LedColor::Green); delay(250);
  writeRgb(LedColor::Blue);  delay(250);
  writeRgb(LedColor::Off);
#elif SCAN_LED_PIN >= 0
  pinMode(SCAN_LED_PIN, OUTPUT);
  digitalWrite(SCAN_LED_PIN, HIGH);  // active low: off
#endif
  hostlink::begin(LINK_RX_PIN, LINK_TX_PIN);  // UART1 on D7(RX)/D6(TX)
  power::init(BAT_ADC_PIN);  // 100k/100k divider on D1 (GPIO2)
  power::sample();           // seed so STATUS bat= is valid immediately
  Serial.printf("[S3] bat %d mV (%d%%)\n", power::voltageMv(), power::percent());
  // DS3231 on I2C D4/D5 (GPIO5/6): if it holds a valid time, latch it as the lowest-priority time source
  // and seed the system clock so a wall clock exists before any phone/GPS fix (docs/mini-build.md §6).
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  s_rtcPresent = rtc::begin(Wire);
  if (s_rtcPresent) g_rtcWrite = rtc::writeUtc;  // enables the phone/GPS -> RTC write-back (core::disciplineRtc)
  uint32_t utc = s_rtcPresent ? rtc::readUtc() : 0;
  if (utc) {
    g_rtcEpochUtc = utc; g_rtcAtMs = millis();
    struct timeval tv = { (time_t)utc, 0 };
    settimeofday(&tv, nullptr);
  }
  Serial.printf("[S3] RTC %s\n", !s_rtcPresent ? "not found" : utc ? "valid (latched)" : "present, time not set");
}
void hostboard::sample() {
  power::sample();
  static uint32_t lastLog = 0;  // console readout for checking against a multimeter (~30 s)
  if (millis() - lastLog >= 30000) {
    lastLog = millis();
    Serial.printf("[S3] bat %d mV (%d%%)\n", power::voltageMv(), power::percent());
  }
}
bool hostboard::sdBegin() {
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  return SD.begin(SD_CS_PIN, sdSPI) && SD.cardType() != CARD_NONE;
}
void hostboard::setScanLed(LedColor c) {
#ifdef MINI_RGB_LED
  writeRgb(c);
#elif SCAN_LED_PIN >= 0
  digitalWrite(SCAN_LED_PIN, c != LedColor::Off ? LOW : HIGH);
#else
  (void)c;  // GPIO21 is the SD chip-select; no LED
#endif
}
int     hostboard::batteryPercent() { return power::percent(); }
uint8_t hostboard::brightness()     { return g_brightness; }
bool    hostboard::rtcPresent()     { return s_rtcPresent; }
bool    hostboard::foregroundScan() { return g_fgScan; }
const char* hostboard::boardTag() { return "MINI"; }
bool    hostboard::pipelineAllowed() { return true; }

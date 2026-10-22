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
#include "../host_common/power.h"
#include "../host_common/rtc.h"

extern uint8_t g_brightness;  // main.cpp: reported in STATUS only (no backlight)
extern bool    g_fgScan;      // main.cpp: a phone-started ("G") foreground scan is running

static SPIClass sdSPI(FSPI);
static bool s_rtcPresent = false;

void hostboard::begin() {
#if SCAN_LED_PIN >= 0
  pinMode(SCAN_LED_PIN, OUTPUT);
  digitalWrite(SCAN_LED_PIN, HIGH);  // active low: off
#endif
  hostlink::begin(LINK_RX_PIN, LINK_TX_PIN);  // UART1 on D7(RX)/D6(TX)
  power::init(BAT_ADC_PIN);  // 100k/100k divider on D3 (GPIO4)
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
void hostboard::setScanLed(bool r, bool g, bool b) {
#if SCAN_LED_PIN >= 0
  digitalWrite(SCAN_LED_PIN, (r || g || b) ? LOW : HIGH);
#else
  (void)r; (void)g; (void)b;  // GPIO21 is the SD chip-select; no LED
#endif
}
int     hostboard::batteryPercent() { return power::percent(); }
uint8_t hostboard::brightness()     { return g_brightness; }
bool    hostboard::rtcPresent()     { return s_rtcPresent; }
bool    hostboard::foregroundScan() { return g_fgScan; }
bool    hostboard::pipelineAllowed() { return true; }

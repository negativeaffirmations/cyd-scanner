// host_board.h — board-provided interface the shared host core calls (docs/mini-build.md §4.2/§4.6).
// Declarations only; each board (src/cyd/, src/xiao_s3/) implements them.
#pragma once
#include <stdint.h>

// Status-LED colours the core asks for (applyScanLed); each board maps them onto its own LED.
enum class LedColor : uint8_t { Off, Red, Green, Blue, Magenta, Cyan, Amber };

namespace hostboard {
void    begin();                           // board bring-up (pins, link UART, LED, power, display/RTC)
void    sample();                          // refresh the battery reading (called every scan cadence)
bool    sdBegin();                         // bring up the SD bus on the board's pins + SD.begin(); true if a card is present
void    setScanLed(LedColor c);            // CYD: on/off RGB on 4/16/17 (Amber shows yellow); S3: PWM RGB or no-op
int     batteryPercent();                  // power::percent() (-1 = no battery sensed)
uint8_t brightness();                      // CYD: backlight %; S3: 100 (reported in STATUS only)
int     ledBrightness();                   // status-LED brightness % (STATUS led=); -1 = no dimmable LED (CYD, mini #1)
void    setLedBrightness(int pct);         // "I:<pct>": clamp, apply, persist (NVS cydui/ledbri); no-op without a dimmable LED
bool    rtcPresent();                      // CYD: false (no DS3231); S3: DS3231 probe result
bool    foregroundScan();                  // true -> log to g_fg + run the live-scan path. CYD: g_screen == SCR_SCAN
bool    pipelineAllowed();                 // headless cycle may run now. CYD: false on Explore/drill-down screens
const char* boardTag();                    // short board kind in the device name: "CYD" / "MINI"
}

// host_board.h — board-provided interface the shared host core calls (docs/mini-build.md §4.2/§4.6).
// Declarations only; each board (src/cyd/, src/xiao_s3/) implements them.
#pragma once
#include <stdint.h>

namespace hostboard {
void    begin();                           // board bring-up (pins, link UART, LED, power, display/RTC)
void    sample();                          // refresh the battery reading (called every scan cadence)
bool    sdBegin();                         // bring up the SD bus on the board's pins + SD.begin(); true if a card is present
void    setScanLed(bool r, bool g, bool b);// CYD: RGB LED (active low) on 4/16/17; S3: one LED or no-op
int     batteryPercent();                  // CYD: power::percent(); S3: -1
uint8_t brightness();                      // CYD: backlight %; S3: 100 (reported in STATUS only)
bool    rtcPresent();                      // CYD: false (no DS3231); S3: DS3231 probe result
bool    foregroundScan();                  // true -> log to g_fg + run the live-scan path. CYD: g_screen == SCR_SCAN
bool    pipelineAllowed();                 // headless cycle may run now. CYD: false on Explore/drill-down screens
}

// rtc.h — DS3231 real-time clock (I2C 0x68), direct register access (no library). Holds UTC.
// The AT24C32 EEPROM many modules carry at 0x57 is deliberately never touched.
// Compiled into every host env but only used where hostboard::rtcPresent() (XIAO-S3).
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <stdint.h>

namespace rtc {
bool     begin(TwoWire& w);  // probe 0x68 (caller has already done w.begin(sda, scl)); true if present
bool     valid();            // present AND oscillator-stop flag clear AND a plausible date (set at some point)
uint32_t readUtc();          // UTC epoch seconds, or 0 if absent/invalid
void     writeUtc(uint32_t epoch);  // set the time and clear the oscillator-stop flag
}

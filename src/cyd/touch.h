// touch.h — calibrated, orientation-independent touch for the CYD (XPT2046).
//
// The XPT2046 is read over a dedicated **software SPI (bit-bang)** bus on its own
// pins (CLK/MOSI/MISO/CS/IRQ per hardware/PINOUT.md). Bit-banging keeps touch fully
// independent of the two hardware SPI peripherals, which are already taken by the
// display (VSPI) and the SD card (HSPI) — so touch can run alongside SD without bus
// contention (the reason it was previously unused).
//
// Points are read in the panel's NATIVE frame (portrait). Calibration stores the raw
// ADC values at the native edges, so it describes the physical glass and never depends
// on display rotation. getScreen() transforms a native point into screen pixels for
// whatever rotation the display currently uses. Calibration persists in NVS (flash).
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Panel native (rotation 0) pixel dimensions for the CYD's ILI9341.
static constexpr int16_t TOUCH_NATIVE_W = 240;
static constexpr int16_t TOUCH_NATIVE_H = 320;

// Raw ADC values at the native panel edges (may be inverted, i.e. left > right;
// the math handles either direction).
struct TouchCal {
  int16_t rawLeft   = 0;    // raw X at native x = 0
  int16_t rawRight  = 4095; // raw X at native x = NATIVE_W - 1
  int16_t rawTop    = 0;    // raw Y at native y = 0
  int16_t rawBottom = 4095; // raw Y at native y = NATIVE_H - 1
  bool    valid     = false;
};

class Touch {
 public:
  // Bring up the bit-bang touch bus. Pins per hardware/PINOUT.md.
  void begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, int8_t irq);

  bool loadCal();  // load calibration from NVS; returns true if a valid set was found
  void saveCal();  // persist current calibration to NVS

  // Interactive two-point calibration. Draws targets, waits for taps, computes
  // and stores the native-frame calibration. Restores the display rotation after.
  void calibrate(TFT_eSPI& tft);

  bool touched();  // true while the panel is pressed (PENIRQ low)

  // If touched, fill screen coords (sx, sy) for the display's CURRENT rotation
  // plus a pressure proxy z, and return true. Uses the stored calibration.
  bool getScreen(TFT_eSPI& tft, int16_t& sx, int16_t& sy, int16_t& z);

  const TouchCal& cal() const { return cal_; }

 private:
  int8_t   sck_ = -1, miso_ = -1, mosi_ = -1, cs_ = -1, irq_ = -1;
  TouchCal cal_;

  uint16_t readChan(uint8_t cmd);                 // one 12-bit bit-bang conversion
  bool     readRaw(int16_t& rx, int16_t& ry);     // averaged raw X/Y while pressed
  bool     readStableRaw(int16_t& rx, int16_t& ry, uint32_t timeoutMs = 20000);
};

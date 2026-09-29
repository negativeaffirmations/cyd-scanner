// touch.h — calibrated, orientation-independent touch for the CYD (XPT2046).
//
// The XPT2046 is always read in the panel's NATIVE frame (touch-controller
// rotation 0). Calibration stores the raw ADC values that correspond to the
// native panel edges, so it describes the physical glass and never depends on
// how the display is rotated. getScreen() then transforms the native point into
// screen pixels for whatever rotation the display is currently using
// (tft.getRotation()), so changing orientation does not require recalibration.
//
// Calibration is persisted in NVS (flash), so it is a one-time on-device step.
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

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
  Touch(uint8_t csPin, uint8_t irqPin);

  // Bring up the dedicated touch SPI bus. Pins per hardware/PINOUT.md.
  void begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs);

  bool loadCal();  // load calibration from NVS; returns true if a valid set was found
  void saveCal();  // persist current calibration to NVS

  // Interactive two-point calibration. Draws targets, waits for taps, computes
  // and stores the native-frame calibration. Restores the display rotation after.
  void calibrate(TFT_eSPI& tft);

  bool touched() { return ts_.touched(); }

  // If touched, fill screen coords (sx, sy) for the display's CURRENT rotation
  // plus pressure z, and return true. Uses the stored calibration.
  bool getScreen(TFT_eSPI& tft, int16_t& sx, int16_t& sy, int16_t& z);

  const TouchCal& cal() const { return cal_; }

 private:
  SPIClass             spi_{HSPI};
  XPT2046_Touchscreen  ts_;
  TouchCal             cal_;

  // Average raw x/y over one press (native frame). Returns false on timeout.
  bool readStableRaw(int16_t& rx, int16_t& ry, uint32_t timeoutMs = 20000);
};

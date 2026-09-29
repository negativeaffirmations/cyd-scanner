// touch.cpp — see touch.h for the design. Bit-bang XPT2046 driver.
#include "touch.h"
#include <Preferences.h>

static Preferences s_prefs;
static const char* kNamespace = "touchcal";

// XPT2046 control bytes: start=1, channel select, 12-bit, PD=00 (keep PENIRQ on).
static constexpr uint8_t CMD_X = 0xD0;  // X position
static constexpr uint8_t CMD_Y = 0x90;  // Y position

void Touch::begin(int8_t sck, int8_t miso, int8_t mosi, int8_t cs, int8_t irq) {
  sck_ = sck; miso_ = miso; mosi_ = mosi; cs_ = cs; irq_ = irq;
  pinMode(sck_, OUTPUT);
  pinMode(mosi_, OUTPUT);
  pinMode(cs_, OUTPUT);
  pinMode(miso_, INPUT);
  if (irq_ >= 0) pinMode(irq_, INPUT);
  digitalWrite(cs_, HIGH);   // idle: deselected
  digitalWrite(sck_, LOW);   // SPI mode 0: clock idle low
}

// One conversion: shift the 8-bit command out (MSB first), then clock 12 result
// bits in. SPI mode 0 — MOSI set while clock low, MISO sampled on the rising edge.
uint16_t Touch::readChan(uint8_t cmd) {
  for (int8_t i = 7; i >= 0; i--) {
    digitalWrite(mosi_, (cmd >> i) & 1);
    digitalWrite(sck_, HIGH);
    digitalWrite(sck_, LOW);
  }
  uint16_t v = 0;
  for (int8_t i = 11; i >= 0; i--) {
    digitalWrite(sck_, HIGH);
    if (digitalRead(miso_)) v |= (1u << i);
    digitalWrite(sck_, LOW);
  }
  return v;  // 0..4095
}

bool Touch::touched() {
  if (irq_ >= 0) return digitalRead(irq_) == LOW;  // PENIRQ asserts low on press
  return false;
}

// Average several X/Y conversions while pressed, discarding obvious outliers.
bool Touch::readRaw(int16_t& rx, int16_t& ry) {
  if (!touched()) return false;
  long sx = 0, sy = 0;
  int  n = 0;
  digitalWrite(cs_, LOW);
  for (int i = 0; i < 12 && touched(); i++) {
    uint16_t x = readChan(CMD_X);
    uint16_t y = readChan(CMD_Y);
    if (x == 0 || x == 4095 || y == 0 || y == 4095) continue;  // rail = noise
    sx += x; sy += y; n++;
  }
  digitalWrite(cs_, HIGH);
  if (n < 3) return false;
  rx = (int16_t)(sx / n);
  ry = (int16_t)(sy / n);
  return true;
}

bool Touch::loadCal() {
  s_prefs.begin(kNamespace, /*readOnly=*/true);
  bool ok = s_prefs.getBool("ok", false);
  if (ok) {
    cal_.rawLeft   = s_prefs.getShort("L", cal_.rawLeft);
    cal_.rawRight  = s_prefs.getShort("R", cal_.rawRight);
    cal_.rawTop    = s_prefs.getShort("T", cal_.rawTop);
    cal_.rawBottom = s_prefs.getShort("B", cal_.rawBottom);
    cal_.valid     = true;
  }
  s_prefs.end();
  return ok;
}

void Touch::saveCal() {
  s_prefs.begin(kNamespace, /*readOnly=*/false);
  s_prefs.putShort("L", cal_.rawLeft);
  s_prefs.putShort("R", cal_.rawRight);
  s_prefs.putShort("T", cal_.rawTop);
  s_prefs.putShort("B", cal_.rawBottom);
  s_prefs.putBool("ok", true);
  s_prefs.end();
}

bool Touch::readStableRaw(int16_t& rx, int16_t& ry, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!touched()) {
    if (millis() - t0 > timeoutMs) return false;
    delay(10);
  }
  long sx = 0, sy = 0;
  int  n = 0;
  while (touched() && n < 64) {
    int16_t x, y;
    if (readRaw(x, y)) { sx += x; sy += y; n++; }
    delay(5);
  }
  if (n == 0) return false;
  rx = (int16_t)(sx / n);
  ry = (int16_t)(sy / n);
  while (touched()) delay(10);  // wait for release
  return true;
}

void Touch::calibrate(TFT_eSPI& tft) {
  uint8_t savedRotation = tft.getRotation();
  tft.setRotation(0);  // calibrate in the native portrait frame

  const int16_t inset = 30;
  const int16_t tx[2] = {inset, (int16_t)(TOUCH_NATIVE_W - 1 - inset)};
  const int16_t ty[2] = {inset, (int16_t)(TOUCH_NATIVE_H - 1 - inset)};
  int16_t rx[2] = {0, 0}, ry[2] = {0, 0};

  for (int i = 0; i < 2; i++) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Touch calibration", TOUCH_NATIVE_W / 2, 40, 2);
    tft.drawString(i == 0 ? "Tap the marker (1/2)" : "Tap the marker (2/2)",
                   TOUCH_NATIVE_W / 2, 60, 2);
    // crosshair target
    tft.drawLine(tx[i] - 10, ty[i], tx[i] + 10, ty[i], TFT_RED);
    tft.drawLine(tx[i], ty[i] - 10, tx[i], ty[i] + 10, TFT_RED);
    tft.drawCircle(tx[i], ty[i], 6, TFT_RED);

    if (!readStableRaw(rx[i], ry[i])) {
      tft.drawString("timeout - retry", TOUCH_NATIVE_W / 2, 90, 2);
      i--;  // retry this point
      delay(500);
      continue;
    }
    delay(250);
  }

  // Linear fit per axis: raw = slope * nativePixel + intercept, extrapolated to
  // the native edges (pixel 0 and pixel MAX).
  float slopeX = (float)(rx[1] - rx[0]) / (float)(tx[1] - tx[0]);
  float slopeY = (float)(ry[1] - ry[0]) / (float)(ty[1] - ty[0]);
  cal_.rawLeft   = (int16_t)lroundf(rx[0] - slopeX * tx[0]);
  cal_.rawRight  = (int16_t)lroundf(cal_.rawLeft + slopeX * (TOUCH_NATIVE_W - 1));
  cal_.rawTop    = (int16_t)lroundf(ry[0] - slopeY * ty[0]);
  cal_.rawBottom = (int16_t)lroundf(cal_.rawTop + slopeY * (TOUCH_NATIVE_H - 1));
  cal_.valid = true;
  saveCal();

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Calibration saved", TOUCH_NATIVE_W / 2, TOUCH_NATIVE_H / 2, 2);
  Serial.printf("[CYD] cal: L=%d R=%d T=%d B=%d\n",
                cal_.rawLeft, cal_.rawRight, cal_.rawTop, cal_.rawBottom);
  delay(900);

  tft.setRotation(savedRotation);
}

bool Touch::getScreen(TFT_eSPI& tft, int16_t& sx, int16_t& sy, int16_t& z) {
  int16_t px, py;
  if (!readRaw(px, py)) return false;
  z = 1;  // pressed (bit-bang path uses PENIRQ for presence, not analog pressure)

  // Normalize raw -> native pixels (handles inverted axes via signed spans).
  float spanX = (float)(cal_.rawRight - cal_.rawLeft);
  float spanY = (float)(cal_.rawBottom - cal_.rawTop);
  if (spanX == 0) spanX = 1;
  if (spanY == 0) spanY = 1;
  int nx = lroundf((px - cal_.rawLeft) / spanX * (TOUCH_NATIVE_W - 1));
  int ny = lroundf((py - cal_.rawTop)  / spanY * (TOUCH_NATIVE_H - 1));
  nx = constrain(nx, 0, TOUCH_NATIVE_W - 1);
  ny = constrain(ny, 0, TOUCH_NATIVE_H - 1);

  // Transform native (portrait) pixels -> current display rotation.
  switch (tft.getRotation() & 3) {
    case 0: sx = nx;                       sy = ny;                       break;
    case 1: sx = ny;                       sy = TOUCH_NATIVE_W - 1 - nx;  break;
    case 2: sx = TOUCH_NATIVE_W - 1 - nx;  sy = TOUCH_NATIVE_H - 1 - ny;  break;
    case 3: sx = TOUCH_NATIVE_H - 1 - ny;  sy = nx;                       break;
  }
  return true;
}

// rtc.cpp — see rtc.h. Clean-room DS3231 register access (datasheet layout: 0x00-0x06 BCD
// sec/min/hour/dow/date/month(+century)/year, 0x0F status bit7 = OSF).
#include "rtc.h"

// Built only for boards with a DS3231 (xiao_s3). The CYD env excludes this file in platformio.ini: merely
// #including <Wire.h> makes PlatformIO's LDF link the Wire library (static `Wire` object + I2C driver,
// ~32 KB) even if never called, which would break the CYD's byte-identical flash/RAM.
namespace {
constexpr uint8_t ADDR = 0x68;
constexpr uint8_t REG_STATUS = 0x0F;
TwoWire* s_w = nullptr;
bool     s_present = false;

uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
void civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

bool readRegs(uint8_t reg, uint8_t* buf, uint8_t n) {
  if (!s_w) return false;
  s_w->beginTransmission(ADDR);
  s_w->write(reg);
  if (s_w->endTransmission(false) != 0) return false;  // repeated start
  if (s_w->requestFrom((int)ADDR, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = s_w->read();
  return true;
}
}  // namespace

bool rtc::begin(TwoWire& w) {
  s_w = &w;
  w.beginTransmission(ADDR);
  s_present = (w.endTransmission() == 0);
  return s_present;
}

uint32_t rtc::readUtc() {
  if (!s_present) return 0;
  uint8_t st;
  if (!readRegs(REG_STATUS, &st, 1) || (st & 0x80)) return 0;  // oscillator stopped: time not trustworthy
  uint8_t r[7];
  if (!readRegs(0x00, r, 7)) return 0;
  unsigned sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F), hr = bcd2bin(r[2] & 0x3F);  // 24 h mode
  unsigned date = bcd2bin(r[4] & 0x3F), mon = bcd2bin(r[5] & 0x1F);
  int year = 2000 + bcd2bin(r[6]);
  if (mon < 1 || mon > 12 || date < 1 || date > 31 || hr > 23 || min > 59 || sec > 59) return 0;
  int32_t days = daysFromCivil(year, mon, date);
  if (days < 0) return 0;
  uint32_t epoch = (uint32_t)days * 86400UL + hr * 3600UL + min * 60UL + sec;
  return epoch >= 1700000000UL ? epoch : 0;  // before ~2023 = never set / reset default
}

bool rtc::valid() { return readUtc() != 0; }

void rtc::writeUtc(uint32_t epoch) {
  if (!s_present) return;
  uint32_t days = epoch / 86400UL, rem = epoch % 86400UL;
  int y; unsigned m, d;
  civilFromDays((int32_t)days, y, m, d);
  if (y < 2000 || y > 2099) return;
  uint8_t dow = (uint8_t)((days + 4) % 7) + 1;  // 1970-01-01 was a Thursday; DS3231 dow 1..7 (arbitrary phase)
  s_w->beginTransmission(ADDR);
  s_w->write((uint8_t)0x00);
  s_w->write(bin2bcd(rem % 60));
  s_w->write(bin2bcd((rem / 60) % 60));
  s_w->write(bin2bcd(rem / 3600));            // 24 h mode (bit6 = 0)
  s_w->write(dow);
  s_w->write(bin2bcd(d));
  s_w->write(bin2bcd(m));                     // century bit left clear (2000-2099)
  s_w->write(bin2bcd(y - 2000));
  s_w->endTransmission();
  uint8_t st;                                  // clear the oscillator-stop flag
  if (readRegs(REG_STATUS, &st, 1)) {
    s_w->beginTransmission(ADDR);
    s_w->write(REG_STATUS);
    s_w->write((uint8_t)(st & ~0x80));
    s_w->endTransmission();
  }
}

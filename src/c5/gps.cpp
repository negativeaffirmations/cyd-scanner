// gps.cpp — minimal NMEA reader (see gps.h). Receive-only.
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pins.h"
#include "gps.h"

namespace gps {

static HardwareSerial GpsSerial(0);          // UART0 (console is USB-CDC, so it's free)
static SemaphoreHandle_t s_mux = nullptr;
static Fix      s_fix = {0, 0, -1000, 0, 0};  // latest good fix (mutex-guarded)
static uint32_t s_fixMs = 0;                  // millis() of the last good sentence
static constexpr uint32_t FIX_STALE_MS = 5000;  // drop the fix if no valid sentence for this long

static constexpr size_t NMEA_LINE_MAX = 100;       // NMEA max is 82; overlong lines are discarded
static char   s_line[NMEA_LINE_MAX];
static size_t s_len = 0;
static bool   s_overflow = false;

void begin() {
  s_mux = xSemaphoreCreateMutex();
  GpsSerial.setClockSource(UART_CLK_SRC_XTAL);
  GpsSerial.setRxBufferSize(512);
  GpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, /*TX=*/-1);  // read-only: never transmit to the module
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// ddmm.mmmm (lat) / dddmm.mmmm (lon) + hemisphere -> deg*1e7. Returns false if malformed.
static bool parseCoord(const char* f, char hemi, int32_t& out) {
  if (!*f || (hemi != 'N' && hemi != 'S' && hemi != 'E' && hemi != 'W')) return false;
  char* end = nullptr;
  double v = strtod(f, &end);
  if (end == f || v < 0) return false;
  int    deg = (int)(v / 100.0);
  double min = v - deg * 100.0;
  if (min >= 60.0) return false;
  double d = deg + min / 60.0;
  if (d > 180.0) return false;
  int32_t r = (int32_t)(d * 1e7 + 0.5);
  out = (hemi == 'S' || hemi == 'W') ? -r : r;
  return true;
}

// Parse one complete sentence (no CR/LF, NUL-terminated, starts with '$').
static void parseLine(char* s) {
  if (s[0] != '$') return;
  char* star = strchr(s, '*');
  if (!star || !star[1] || !star[2]) return;
  uint8_t sum = 0;
  for (const char* p = s + 1; p < star; p++) sum ^= (uint8_t)*p;
  int h1 = hexVal(star[1]), h2 = hexVal(star[2]);
  if (h1 < 0 || h2 < 0 || sum != (uint8_t)((h1 << 4) | h2)) return;  // bad checksum
  *star = 0;

  // Split on commas in place (empty fields preserved).
  char* f[16];
  int nf = 0;
  f[nf++] = s + 1;
  for (char* p = s + 1; *p && nf < 16; p++)
    if (*p == ',') { *p = 0; f[nf++] = p + 1; }

  const char* id = f[0];            // e.g. "GPRMC", "GNGGA"
  if (strlen(id) != 5) return;
  const char* type = id + 2;        // talker-agnostic (GP / GN / ...)

  if (!strcmp(type, "RMC") && nf >= 7) {
    // time,status,lat,N/S,lon,E/W,...
    if (f[2][0] != 'A') return;     // not valid
    int32_t lat, lon;
    if (!parseCoord(f[3], f[4][0], lat) || !parseCoord(f[5], f[6][0], lon)) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_fix.lat = lat; s_fix.lon = lon;
    if (s_fix.fix == 0) { s_fix.fix = 1; s_fix.altM = -1000; s_fix.sats = 0; }  // RMC-only: 2D, no alt
    s_fixMs = millis();
    xSemaphoreGive(s_mux);
  } else if (!strcmp(type, "GGA") && nf >= 10) {
    // time,lat,N/S,lon,E/W,fixQual,numSats,HDOP,alt,M,...
    int q = atoi(f[6]);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (q <= 0) {
      s_fix = Fix{0, 0, -1000, 0, 0};
    } else {
      int32_t lat, lon;
      if (parseCoord(f[2], f[3][0], lat) && parseCoord(f[4], f[5][0], lon)) {
        int sats = atoi(f[7]);
        s_fix.lat = lat; s_fix.lon = lon;
        s_fix.sats = (uint8_t)(sats < 0 ? 0 : sats > 255 ? 255 : sats);
        if (f[9][0]) {
          int a = (int)lroundf(strtof(f[9], nullptr));
          s_fix.altM = (int16_t)(a < -999 ? -999 : a > 32000 ? 32000 : a);
          s_fix.fix  = (s_fix.sats >= 4) ? 2 : 1;  // GGA has no 2D/3D flag: altitude + >=4 sats ~ 3D
        } else {
          s_fix.altM = -1000;
          s_fix.fix  = 1;
        }
        s_fixMs = millis();
      }
    }
    xSemaphoreGive(s_mux);
  }
}

void poll() {
  int budget = 256;  // bound work per call; the 512 B RX buffer holds the rest for next loop
  while (budget-- > 0 && GpsSerial.available()) {
    char c = (char)GpsSerial.read();
    if (c == '\n') {
      if (!s_overflow && s_len > 0) {
        if (s_line[s_len - 1] == '\r') s_len--;
        s_line[s_len] = 0;
        parseLine(s_line);
      }
      s_len = 0; s_overflow = false;
    } else if (s_len < NMEA_LINE_MAX - 1) {
      s_line[s_len++] = c;
    } else {
      s_overflow = true;  // overlong: discard through the next newline
    }
  }
}

void snapshot(Fix& out) {
  xSemaphoreTake(s_mux, portMAX_DELAY);
  if (s_fix.fix && millis() - s_fixMs > FIX_STALE_MS) s_fix = Fix{0, 0, -1000, 0, 0};  // lost signal
  out = s_fix;
  xSemaphoreGive(s_mux);
}

}  // namespace gps

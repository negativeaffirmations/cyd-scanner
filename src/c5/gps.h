// gps.h — onboard GPS (GY-NEO6MV2 NEO-6M) NMEA reader for the ESP32-C5.
//
// Clean-room minimal NMEA parser (RMC + GGA, GP/GN talkers). Receive-only: the module's RXD is
// never driven (TX = -1). The latest fix rides to the CYD in link_protocol::Status (v9) where it
// is a FALLBACK to the phone's GPS.
#pragma once
#include <stdint.h>

namespace gps {

struct Fix {
  int32_t  lat;     // deg * 1e7, 0 = no fix
  int32_t  lon;     // deg * 1e7, 0 = no fix
  int16_t  altM;    // metres MSL, -1000 = unknown
  uint8_t  sats;    // satellites in use (0 if no fix)
  uint8_t  fix;     // 0 = no fix, 1 = 2D, 2 = 3D
  uint32_t epochUtc;// UTC epoch secs from the last valid RMC date+time (0 = unknown)
};

void begin();               // open UART0 @ 9600 and init
void poll();                // non-blocking: drain bytes, parse complete NMEA lines
void snapshot(Fix& out);    // mutex-guarded copy of the latest fix (0/0/-1000/0/0 if none)

}  // namespace gps

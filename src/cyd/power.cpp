// power.cpp — CYD battery monitor. See power.h.
#include "power.h"
#include "pins.h"
#include <Arduino.h>

namespace {
  // Single-cell LiPo resting-voltage -> state-of-charge. Monotonic points, linearly
  // interpolated between them. Deliberately conservative near the knees (3.3-3.6 V), where
  // the pack drops fast. Under load the reading sags, so this reads a little low mid-discharge
  // — fine for a "roughly how much is left" status glyph.
  struct Pt { int mv; int pct; };
  const Pt CURVE[] = {
    {4200, 100}, {4100, 95}, {4000, 85}, {3900, 75}, {3800, 60}, {3750, 50},
    {3700, 42},  {3650, 30}, {3600, 22}, {3500, 13}, {3400, 8},  {3300, 4},
    {3200, 1},   {3000, 0},
  };
  const int NCURVE = (int)(sizeof(CURVE) / sizeof(CURVE[0]));

  // Below this the divider is reading essentially nothing: battery switch off / no cell /
  // running on USB only. Treat as "no battery present" rather than 0 %.
  const int BAT_MIN_VALID_MV = 2500;

  int g_mv = 0;  // smoothed battery mV (0 = never sampled)

  int mvToPct(int mv) {
    if (mv >= CURVE[0].mv)          return 100;
    if (mv <= CURVE[NCURVE - 1].mv) return 0;
    for (int i = 1; i < NCURVE; i++) {
      if (mv >= CURVE[i].mv) {                      // CURVE[i-1].mv > mv >= CURVE[i].mv
        const Pt& a = CURVE[i - 1];
        const Pt& b = CURVE[i];
        return b.pct + (int)((long)(a.pct - b.pct) * (mv - b.mv) / (a.mv - b.mv));
      }
    }
    return 0;
  }
}

namespace power {
  void init() {
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);  // full ~0-3.3 V span (divider tops out ~2.1 V)
    g_mv = 0;
  }

  void sample() {
    uint32_t acc = 0;
    const int N = 8;                                 // average a few reads to settle ADC noise
    for (int i = 0; i < N; i++) acc += analogReadMilliVolts(BAT_ADC_PIN);
    int raw = (int)(acc / N) * 2;                    // undo the 100k/100k divider
    if (g_mv == 0) g_mv = raw;                       // seed on first sample
    else           g_mv += (raw - g_mv) / 4;         // EMA (~0.25 weight) to smooth load dips
  }

  int  voltageMv() { return g_mv; }
  bool present()   { return g_mv >= BAT_MIN_VALID_MV; }
  int  percent()   { return present() ? mvToPct(g_mv) : -1; }
}

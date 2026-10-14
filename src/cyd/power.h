// power.h — CYD battery monitor (single-cell LiPo, voltage-only).
//
// The cell is sensed through a 100 kΩ / 100 kΩ divider off the *switched* battery rail
// (boost IN+ side), feeding CYD GPIO35 (input-only, ADC1_CH7). So:
//     Vbat(mV) = analogReadMilliVolts(BAT_ADC_PIN) * 2
// ADC1 is unaffected by Wi-Fi/BLE, so this keeps working during scanning and the SoftAP.
//
// Voltage-only: the TP4056 CHRG pad is NOT wired, so there is no USB-charge-detect input
// (GPIO26 stays free). We report state-of-charge from the resting voltage curve, nothing more.
// See hardware/PINOUT.md §4-§5. CYD-only; no link-protocol involvement.
#pragma once
#include <stdint.h>

namespace power {
  void init();        // configure the ADC pin; call once in setup()
  void sample();      // one averaged reading -> smoothed state; call on the scan cadence (~2 s)
  int  voltageMv();   // smoothed battery voltage in mV (0 until first sample)
  int  percent();     // smoothed state-of-charge 0..100, or -1 when no battery is present
  bool present();     // true when a plausible battery voltage is on GPIO35
}

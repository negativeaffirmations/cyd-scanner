// power.h — host battery monitor (single-cell LiPo, voltage-only), shared by the CYD and XIAO-S3.
//
// The cell is sensed through a 100 kΩ / 100 kΩ divider off the *switched* battery rail, feeding an
// ADC1 pin the board passes to init() (its BAT_ADC_PIN): CYD GPIO35 (input-only, ADC1_CH7; boost IN+
// side), XIAO-S3 D1 = GPIO2 (ADC1_CH1; switch output that feeds both XIAOs' B+). So:
//     Vbat(mV) = analogReadMilliVolts(BAT_ADC_PIN) * 2
// ADC1 is unaffected by Wi-Fi/BLE, so this keeps working during scanning and the SoftAP.
//
// Voltage-only: no charge-detect input on either board (the CYD's TP4056 CHRG pad is NOT wired, GPIO26
// stays free). We report state-of-charge from the resting voltage curve, nothing more.
// See hardware/PINOUT.md §4-§5 (CYD) and §6 (mini build). No link-protocol involvement.
#pragma once
#include <stdint.h>

namespace power {
  void init(int adcPin); // configure the ADC pin (board's BAT_ADC_PIN); call once in setup()
  void sample();      // one averaged reading -> smoothed state; call on the scan cadence (~2 s)
  int  voltageMv();   // smoothed battery voltage in mV (0 until first sample)
  int  percent();     // smoothed state-of-charge 0..100, or -1 when no battery is present
  bool present();     // true when a plausible battery voltage is on the ADC pin
}

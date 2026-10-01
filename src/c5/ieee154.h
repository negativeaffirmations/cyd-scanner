// ieee154.h — passive 802.15.4 (Zigbee/Thread) presence sniffer (Phase 4).
//
// Receive-only: the radio is put in promiscuous RX (no PAN/address filter) and
// each frame's MAC header is parsed in the RX ISR into a compact record that is
// queued for task context, where tick() turns it into a Detection (source =
// Ieee802154) for the shared table. -D IEEE154_TEST adds a [154] console line.
//
// Strictly PASSIVE: nothing is transmitted (no TX / ED / CCA-TX APIs are used,
// and the driver is never asked to ACK — promiscuous mode suppresses auto-ACK).
// The radio PHY is owned by main.cpp, which time-slices it; this module only
// enables/disables the 802.15.4 subsystem and hops channels on request.
#pragma once

#include <stdint.h>
#include <esp_err.h>
#include "link_protocol.h"

namespace ieee154 {

static constexpr uint8_t  CH_MIN       = 11;   // 2.4 GHz O-QPSK channels 11..26
static constexpr uint8_t  CH_MAX       = 26;
static constexpr int      QUEUE_DEPTH  = 16;   // ISR -> task parsed-record queue
static constexpr uint16_t PAN_NONE     = 0xFFFF;  // record had no PAN ID field

// Source address mode of a parsed frame.
enum AddrMode : uint8_t { ADDR_NONE = 0, ADDR_SHORT = 2, ADDR_EXT = 3 };

// Compact record produced in the ISR (kept small: copied through a queue).
struct Record {
  uint8_t  channel;
  int8_t   rssi;
  uint8_t  lqi;
  uint8_t  ftype;     // FCF frame type: 0 beacon, 1 data, 3 MAC cmd, 5 multipurpose...
  uint16_t pan;       // source PAN (else dest PAN), PAN_NONE if absent
  uint8_t  addrMode;  // AddrMode of src
  uint8_t  addr[8];   // src address as on air (little-endian: short uses [0..1])
};

// Sink called from tick() (task context) once per captured frame; main.cpp folds it
// into the detection table.
using DetCb = void (*)(const link_protocol::Detection& d);

void     begin(DetCb cb);        // create the ISR->task queue + register sink (once, at boot)
esp_err_t enable();              // enable 802.15.4 + promiscuous RX; returns driver result
void     disable();              // hand the PHY back (before a Wi-Fi scan)
void     setChannel(uint8_t ch); // retune while capturing (11..26)
void     tick();                 // drain the queue into the sink (task context)
bool     active();               // true while the 802.15.4 subsystem is enabled

}  // namespace ieee154

// promisc.h — passive promiscuous-mode Wi-Fi capture (Phase 2).
//
// Sniffs 802.11 frames the AP-only WiFi.scanNetworks() never sees:
//   - probe REQUESTS  -> Wi-Fi clients (Flock cameras act as clients that
//                        channel-hop 1/6/11 with wildcard SSIDs — a strong tell)
//   - beacons / probe RESPONSES -> APs, enriched with an IE fingerprint
//   - uplink DATA frames -> the addr2 source MAC of a client ASSOCIATED to a home
//                        AP (a Ring/Nest/Wyze camera that, once joined, neither
//                        beacons nor probes — invisible to every other path)
// For management frames it computes an IE-order fingerprint hash (survives MAC
// randomization); for data frames it just harvests the client MAC. Each becomes
// a Detection handed to the registered sink.
//
// Strictly PASSIVE: receive-only. Nothing is transmitted or injected. The radio
// is owned by main.cpp, which time-slices AP scanning and promiscuous capture;
// this module only toggles promiscuous mode and hops channels on request.
#pragma once

#include <stdint.h>
#include "link_protocol.h"

namespace promisc {

// Sink called (from the Wi-Fi task context) once per captured frame. Keep it
// fast; main.cpp routes it into the shared, mutex-guarded detection table.
using DetCb = void (*)(const link_protocol::Detection& d);

// Optional AP sink for evil-twin tracking: called for each beacon/probe-response with a
// non-empty SSID. enc class: 0 open, 1 WEP, 2 secured (RSN/WPA present).
using ApCb = void (*)(const char* ssid, const uint8_t* bssid, uint8_t enc, uint8_t ch);

void begin(DetCb cb, ApCb apCb = nullptr);  // register the sinks (call once, at boot)
uint16_t deauthRecent();     // peak deauth+disassoc 1 s-rate OBSERVED, held ~10 s so polls between
                             // promiscuous windows still see a recent flood (receive-only)
void enable();               // turn promiscuous mode on (management + data-frame filter)
void disable();              // turn promiscuous mode off (before an AP scan)
void setChannel(uint8_t ch); // park the radio on a channel while capturing
bool active();               // true while promiscuous mode is on
void frameStats(uint32_t& mgmt, uint32_t& data, uint32_t& dataClient);  // read+reset per-window RX counters

}  // namespace promisc

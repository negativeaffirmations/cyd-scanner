// gattid.h — brief read-only GATT identification of nearby BLE devices (C5). See docs/gatt-identify.md.
//
// Connects to a candidate for a couple of seconds, reads the standard identification characteristics with ATT Read By
// Type (no service discovery, no writes, no subscriptions) and disconnects. The NimBLE Security Manager is compiled
// out of this build, so a read that needs encryption is recorded (DI_AUTH) and never followed by pairing.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "link_protocol.h"

class NimBLEAdvertisedDevice;

namespace gattid {

bool stableAddr(uint8_t addrType, const uint8_t mac[6]);   // public or random-static (the only addresses a read may target)

void begin();       // load the saved console settings
void disable(const char* why);   // off for this boot (no usable random address, a link that never closed)

// NimBLE scan task, once per received advert (including scan responses): track the device, apply the denylist.
// `mac` is big-endian text order, as Detection.mac.
void observe(const NimBLEAdvertisedDevice* dev, const uint8_t mac[6], int8_t rssi);

// Loop task, on each StartScan: the host's enable flag, RSSI floor, skip + priority MACs (null = payload had no hint).
void applyHint(const link_protocol::GattHint* hint);

// Loop task, every pass. `canStart` = the caller's radio is idle in the BLE part of the time-slice with enough time
// left; `cycleId` advances once per scan cycle, also when the AP scan is off (one attempt per cycle). Also keeps closing
// a link that did not go away, even when disabled, and restarts the C5 if one stays open 30 s after the first terminate.
void tick(uint32_t now, bool canStart, uint32_t cycleId);

bool holdsRadio();   // a slot is using the radio: the BLE scan must stay stopped (blePaused())
bool busy();         // a slot is running, or a link is still open / closing: the next AP scan must wait
uint32_t worstSlotMs();   // longest a slot can take from start to a closed link, with margin: a slot opens only if this much time remains
uint32_t lastSlotMs();    // duration of the most recent slot

// Results for BLE rows in the live table (`live` = the stream copy): up to `max` DevInfo frames that are due.
int  collect(const link_protocol::Detection* live, int nLive, link_protocol::DevInfo* out, int max);

// Console: `gatt ...`. Returns false when the line is not a gatt command.
bool console(const char* line);

uint32_t attempts();   // since boot
uint32_t successes();

}  // namespace gattid

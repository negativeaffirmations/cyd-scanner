// devinfo.h — GATT identification results from the C5 (Reply::DevInfo), kept per BLE MAC. See docs/gatt-identify.md.
//
// Own C5's results only. The store is heap-allocated when the first frame arrives (a host that never gets one pays
// nothing). The reads themselves are switched by the phone (CMD "C:<0|1>") / serial `gatt on|off`, NVS cydui/gattrd,
// default on; the switch rides every StartScan as GattHint.flags.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "link_protocol.h"

// Entries kept (oldest heard replaced). The S3 envs build with -DHOST_MAX_DEVINFO=64.
#ifndef HOST_MAX_DEVINFO
#define HOST_MAX_DEVINFO 24
#endif

namespace devinfo {

struct Entry {
  link_protocol::DevInfo di;
  uint32_t heardMs;       // millis() of the last frame
  uint32_t readMs;        // millis() when these field values first arrived (a new read; re-sent frames keep it)
  uint32_t loggedFile;    // file_id (as a number) of the log file the devinfo line was written to, 0 = none
  uint32_t loggedHash;    // hash of the fields that line carried
  bool     used, pending; // pending: a frame arrived that the log has not looked at yet
};

void load();                      // NVS: the on/off switch
bool enabled();
void setEnabled(bool on);         // saved; takes effect on the next StartScan

void ingest(const link_protocol::DevInfo& in);   // terminate strings, drop our own radios + whitelisted devices, store
const link_protocol::DevInfo* find(const uint8_t mac[6]);
int      capacity();              // HOST_MAX_DEVINFO once the store exists, else 0
Entry*   entryAt(int i);          // null for an unused slot
uint32_t fieldHash(const link_protocol::DevInfo& di);

// A display name from the GATT fields: the name when read, else "<maker> <model>" (when a model was read), else "".
// Returns di.name itself or `buf`.
const char* nameOf(const link_protocol::DevInfo& di, char* buf, size_t cap);

// Fill the hint appended to every StartScan (skip: our own address, the phone, whitelisted BLE devices; prio: BLE
// devices at Suspect or above). Built from the groups of the previous cycle.
void fillHint(link_protocol::GattHint& h);

}  // namespace devinfo

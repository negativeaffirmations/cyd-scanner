// whitelist.h — user-managed "known, not a threat" device list.
//
// Loads /whitelist.csv from the SD card into bounded static RAM (no heap). A whitelisted
// device is still listed, but shown muted and excluded from the follow alert. The file is
// user data (own device identifiers) and is gitignored; it self-seeds a commented header.
//
// Rule format (4 fields; label may contain commas):  kind,pattern,srcmask,label
//   mac,AA:BB:CC:DD:EE:FF,A,label    exact MAC (6 bytes)
//   macpfx,AA:BB:CC,A,label          MAC/OUI prefix (3..5 bytes)
//   nexact,<name>,B,label            exact device name
//   ncontains,<text>,B,label         name substring
//   blecid,004C,B,label              BLE manufacturer company ID (hex)
//   bleuuid,<uuid16|uuid128>,B,label BLE service UUID
// srcmask: W/B/4/A (as in signatures.csv).
#pragma once

#include <Arduino.h>

namespace whitelist {

// Load /whitelist.csv (seeding a commented header first if absent). Returns true if the
// file was read (even when it holds no rules).
bool begin();

// Re-parse /whitelist.csv (after an edit, add() or removeAt()).
bool reload();

int  count();  // loaded rules

// Is this device whitelisted? `srcBits` is the union of (1 << Detection.source) over the
// device's detections (a probe request should also carry the WIFI24 bit, as in sigdb).
// `name` may be null/empty; `companyId` 0 and an all-zero `svc` mean "none".
bool match(const uint8_t mac[6], const char* name, uint16_t companyId,
           const uint8_t svc[16], uint8_t srcBits);

// Append one rule line to the file and reload. False if the line is invalid, the table
// is full, or the SD write failed.
bool add(const char* csvLine);

// Remove the idx-th loaded rule (0-based, file order) from the file and reload.
bool removeAt(int idx);

// Copy the idx-th rule's CSV line (as stored in the file) into buf. False if out of range.
bool lineAt(int idx, char* buf, size_t cap);

// Append every loaded rule's CSV line (newline-terminated, file order) to `out` in one
// pass over the file. Same order as lineAt()/removeAt() indices.
void listAll(String& out);

// --- Fleet whitelist (fw 0.39.0, docs/fleet-mode.md §26) ---
// A fleet LEADER shares its whole whitelist with its joined clients; a client mutes a device when its own
// list OR the fleet list matches. The fleet list lives in RAM only (heap, allocated on first use, so a unit
// outside a fleet pays nothing) and never touches this unit's /whitelist.csv.
constexpr int MAX_LIST = 48;  // rules per list (own or fleet)
constexpr int REC_LEN = 26;   // wire form of one rule: kind, srcmask, 24-byte pattern (no label)
uint16_t gen();               // bumped on every reload(): the leader re-sends when it changes
bool     exportRec(int idx, uint8_t out[REC_LEN]);   // idx-th own rule in wire form; false if out of range
void     setFleet(const uint8_t* recs, int n);       // client: replace the fleet list (invalid records dropped)
void     clearFleet();
int      fleetCount();

}  // namespace whitelist

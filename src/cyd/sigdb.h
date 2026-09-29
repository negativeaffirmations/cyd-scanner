// sigdb.h — signature database + weighted confidence scoring (Phase 1).
//
// Loads a rule set from /signatures.csv on the SD card into bounded, PSRAM-free RAM,
// and scores each streamed Detection by summing the best match from each layer
// (OUI + device-name), mapping the total to a confidence Tier. The DB is editable on
// the card (or pushed via the phone link) with NO firmware reflash. If the card has
// no DB, a small built-in fallback keeps detection working.
//
// Passive/defensive scope: this only classifies observed broadcast RF.
#pragma once

#include <Arduino.h>
#include "link_protocol.h"  // Detection, Source, SourceMask

namespace sigdb {

enum class Tier : uint8_t { None = 0, Suspect, Likely, Confirmed };

// Result of scoring one Detection. Rule indices reference the loaded tables so no
// per-detection label copies are made.
struct ScoreResult {
  uint8_t score = 0;
  Tier    tier  = Tier::None;
  int8_t  bestOui  = -1;  // index into the OUI table, -1 = no match
  int8_t  bestStr  = -1;  // index into the string table, -1 = no match
  int8_t  bestIe   = -1;  // index into the IE-fingerprint table, -1 = no match
  int8_t  bestUuid = -1;  // index into the BLE service-UUID table, -1 = no match
  int8_t  bestCid  = -1;  // index into the BLE company-ID table, -1 = no match
};

// Load /signatures.csv (writing the built-in seed first if the file is absent).
// Returns true if a DB file was parsed; false if running on the compiled fallback.
bool begin();

// Re-parse /signatures.csv (e.g. after an edit or a phone push). Same return meaning.
bool reload();

// Score one detection against the loaded rules.
void score(const link_protocol::Detection& d, ScoreResult& out);

// Label of the strongest matched rule for a result ("" if none).
const char* labelFor(const ScoreResult& r);

const char* tierName(Tier t);   // "none"/"suspect"/"likely"/"confirmed"
bool        loaded();           // true if a DB file was parsed (vs fallback)
uint16_t    ruleCount();        // total loaded rules (OUI + string)

}  // namespace sigdb

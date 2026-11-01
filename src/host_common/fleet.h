// fleet.h — fleet-mode facade (docs/fleet-mode.md). Cooperating scanners talk over ESP-NOW on the HOST
// radios (CYD ESP32 / mini S3) on one fixed 2.4 GHz channel. P1 = transport + boot modes: roles in NVS, Wi-Fi
// STA + ESP-NOW bring-up with a random per-boot MAC, the boot-loop fallback, and (under -DFLEET_DEBUG only)
// cleartext ping/pong bench frames. No pairing/crypto yet and nothing is transmitted without FLEET_DEBUG.
//
// Normal-mode footprint: a few statics only. Every buffer is heap-allocated by beginClient()/beginLeader().
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace fleet {

constexpr uint8_t ROLE_OFF = 0, ROLE_CLIENT = 1, ROLE_LEADER = 2;   // NVS cydscan/fltrole

// --- roles / NVS (call loadRole() first thing in setup(), before anything touches BT) ---
// Reads fltrole/fltch, refuses leader on a board that can't lead, and runs the boot-loop guard: 3 consecutive
// crash boots (panic / WDT / brownout) clear fltrole and boot normal (fellBack()).
void    loadRole();
uint8_t role();                    // the role this boot runs (after the guard)
bool    isClient();                // role() == ROLE_CLIENT
bool    setRole(uint8_t r);        // persist fltrole (caller restarts); false when NVS fails / r invalid for this board
uint8_t channel();                 // fleet channel 1..13 (NVS fltch, default 1)
bool    setChannel(uint8_t ch);    // persist; retunes immediately when the fleet is active
bool    fellBack();                // true once if this boot cleared fltrole via the guard (consumed on read)
uint8_t fellBackRole();            // the role the guard cleared (valid after fellBack() returned true)
void    retune();                  // re-assert the fleet channel (webshare stop() / AP.clear() leaves the radio on ch 1)

// --- lifecycle ---
bool    beginClient();             // Wi-Fi STA + ESP-NOW + heap buffers; false on failure (fleet stays inactive)
bool    beginLeader();
bool    active();                  // ESP-NOW is up (webshare keeps the STA, joins the fleet channel)
void    service();                 // drain <= 8 RX frames, debug ping/stats, boot-guard timer. Cheap + safe when inactive.
// Randomize the STA MAC again. Only while NOT joined (a joined unit never changes MAC): deinit ESP-NOW ->
// esp_wifi_stop -> set_mac -> start -> re-init. Prints the elapsed ms. P2 calls it before answering a Probe.
bool    rerandomizeMac();
const uint8_t* staMac();           // current fleet STA MAC (6 B)
uint16_t maxPayload();             // 250 (ESP-NOW v1) or 1470 (v2); 0 before begin
void    onRekey();                 // the unit's phone key changed: drop any session (P1: log only)

// --- UI / diagnostics ---
struct Ui {
  bool     active;
  uint8_t  role, channel;
  bool     joined;                 // P1: always false (no handshake yet)
  bool     leaderSeen;             // FLEET_DEBUG: a ping was heard from a leader recently
  uint8_t  leaderMac[6];
  int8_t   rssi;                   // last leader frame (leaderSeen)
  uint32_t ageMs;                  // since the last leader frame (leaderSeen)
  uint32_t txOk, txFail;           // ESP-NOW send results (send callback)
  uint32_t rxFrames, rxDrop;       // frames queued / dropped (queue full or oversize)
};
void uiSnapshot(Ui& u);
void printStatus();                // serial `fleet`
void dbgSetRate(int mbps);         // FLEET_DEBUG bench knobs (no-ops otherwise): peer PHY rate 1 or 12
void dbgSetPad(int len);           //   ping frame size: 250 / 1300 / 0 = alternate

}  // namespace fleet

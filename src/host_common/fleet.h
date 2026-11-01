// fleet.h — fleet-mode facade (docs/fleet-mode.md). Cooperating scanners talk over ESP-NOW on the HOST
// radios (CYD ESP32 / mini S3) on one fixed 2.4 GHz channel. P1 = transport + boot modes: roles in NVS, Wi-Fi
// STA + ESP-NOW bring-up with a random per-boot MAC, the boot-loop fallback, and (under -DFLEET_DEBUG only)
// cleartext ping/pong bench frames. P2 = enrollment + auth: the leader stores members (sid + fleet key, never the
// member's phone key), probes while one is un-joined, and runs the Hello/Join/JoinAck handshake into an encrypted,
// tagged session; Sync/MStat heartbeats detect loss. Frames are sent only while a fleet role is set.
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
// esp_wifi_stop -> set_mac -> start -> re-init. Prints the elapsed ms. A client calls it before answering the first
// Probe after it lost or left a session (docs §16.4); at boot the MAC is already fresh.
bool    rerandomizeMac();
const uint8_t* staMac();           // current fleet STA MAC (6 B)
uint16_t maxPayload();             // 250 (ESP-NOW v1) or 1470 (v2); 0 before begin
void    onRekey();                 // the unit's phone key changed: its fleet key changed too, drop any session
// Host log hook: fleet events ("join", "lost", "unjoin", "release", "member_add", ...) -> logFleetEvent(op).
void    setEventSink(void (*fn)(const char* op));

// --- phone commands (CMD "M:<op>[:args]", docs §9.4) ---
// Runs on loop(). Writes ONE reply body ("OK<TAB>..." / "ERR<TAB><why>") that the host frames as LOGDATA "FLT=<n>".
// Ops: l list | a add | r remove | x release | c channel | L1/L0 leader on/off | j become a client.
enum CmdAction : uint8_t { CMD_NONE = 0, CMD_RESTART = 1 };   // RESTART: the host reboots after sending the reply
CmdAction handlePhoneCmd(const char* cmd, char* reply, size_t cap);
// STATUS part 2 field value: "<role>,<joined>/<enrolled>,<ch>" (client: joined 0/1 of 1).
void    statusField(char* out, size_t cap);

// --- P3: time / position shared across the fleet (host_core source 4 = "fleet", docs §7 + §16.6) ---
// Client: the leader's clock + fleet position from its Sync. Leader: posFix = the freshest live GPS fix a MEMBER
// reported (used only when the leader has no live fix of its own); timeFix/timeFresh are false (it IS the source).
bool timeFix(uint32_t* utc, uint32_t* atMs);   // UTC `utc` was current at millis() `atMs`; false = never had one
bool timeFresh();                              // client: a Sync with a valid time arrived within 15 s
bool posFix(float* lat, float* lon, uint32_t* ageMs);   // position + its age NOW; false = none
// MACs of the fleet's own radios (leader BLE advert, its phone, its SoftAP, members' BLE): never logged as detections.
bool isFleetSelf(const uint8_t mac[6]);

// --- UI / diagnostics ---
struct Ui {
  bool     active;
  uint8_t  role, channel;
  bool     joined;                 // client: in an authenticated session with a leader
  bool     leaderSeen;             // joined, or (FLEET_DEBUG) a ping heard recently
  uint8_t  leaderMac[6];
  char     leaderName[21];         // from the encrypted Sync ("" before the first one)
  uint8_t  slot, nMembers;         // from Join
  int8_t   rssi;                   // last leader frame (leaderSeen)
  uint32_t ageMs;                  // since the last leader frame (leaderSeen)
  uint8_t  members, joinedMembers; // leader: enrolled / joined
  uint32_t txOk, txFail;           // ESP-NOW send results (send callback)
  uint32_t rxFrames, rxDrop;       // frames queued / dropped (queue full or oversize)
};
void uiSnapshot(Ui& u);
void printStatus();                // serial `fleet`
void dbgSetRate(int mbps);         // FLEET_DEBUG bench knobs (no-ops otherwise): peer PHY rate 1 or 12
void dbgSetPad(int len);           //   ping frame size: 250 / 1300 / 0 = alternate
void dbgSetTrace(bool on);         //   one console line per ping sent / received, with the actual RF channel
// FLEET_DEBUG bench enrollment without a phone: the client prints its fleet key, the leader stores it directly.
void dbgPrintFk();                 //   client: "fleet fk"
bool dbgAddMember(const char* sidHex, const char* fkHex, const char* name);   // leader: "fleet add <sid> <fk> <name>"

}  // namespace fleet

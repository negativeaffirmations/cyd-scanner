// fleet_proto.h — host-to-host fleet wire protocol (docs/fleet-mode.md §4). HOST-ONLY: the C5s never include it.
// All structs are packed, little-endian. FLEET_PROTO_VERSION is independent of the CYD<->C5 link version
// and of DETS. P1: Hdr + the FLEET_DEBUG ping/pong. P2: Probe/Hello/Join/JoinAck, Sync + MStat as heartbeats, Ctrl.
// Session frames (JoinAck and later) carry an 8-byte HMAC tag AFTER the struct (fleet_internal.h TAG_LEN), so their
// on-air length is sizeof(struct) + 8. Sizes are pinned by static_asserts.
#pragma once
#include <stdint.h>
#include "link_protocol.h"

namespace fleet_proto {

constexpr uint8_t  FLEET_PROTO_VERSION = 5;   // 5 (fw 0.34.0): IRKs shared (MStat.irk, Irks); 4 (fw 0.30.0): radio MACs shared (Sync.nRadio, MStat.bleMac/c5Mac); 3 (fw 0.25.0): LogRows/LogAck store-and-forward (P6 item 6); 2 (0.24.0): Sync.fleetId
constexpr uint8_t  FLEET_DEFAULT_CH    = 1;
constexpr uint16_t FLEET_V1_MAX = 250, FLEET_V2_MAX = 1470;
// Clients per leader (docs/fleet-mode.md §16.5): the prebuilt libs allow 7 ENCRYPTED ESP-NOW peers (the
// broadcast Probe peer is unencrypted and doesn't count); 6 clients leaves one encrypted slot spare (P2's startJoin
// deletes a rejoining member's stale entry first, so it is headroom). A CYD client needs only 1.
constexpr int      FLEET_MAX_MEMBERS = 6;
constexpr int      DETS_PER_V1 = 2, DETS_PER_V2 = 16, RIDS_PER_V1 = 3, RIDS_PER_V2 = 8;
// TX power in 0.25 dBm units (esp_wifi_set_max_tx_power): 32 = 8 dBm, plenty for units a few feet apart
// and the least desense on each unit's own C5 (§16.2).
constexpr int8_t   FLEET_TXPWR_QDBM = 32;

enum class Msg : uint8_t { Probe = 0x01, Hello = 0x02, Join = 0x03, JoinAck = 0x04,
                           Sync = 0x10, Plan = 0x11, Ctrl = 0x12, Irks = 0x13, Threats = 0x14,
                           MStat = 0x20, DetBatch = 0x21, RidBatch = 0x22, LogRows = 0x23, LogAck = 0x24,
                           DbgPing = 0xF0, DbgPong = 0xF1 };   // Dbg*: P1 bench frames, sent only under -DFLEET_DEBUG
enum CtrlOp : uint8_t { CTRL_RELEASE = 1, CTRL_UNJOIN = 2, CTRL_NEWSESSION = 3, CTRL_SETCH = 4 };
enum PlanRole : uint8_t { PLANROLE_FULL = 0, PLANROLE_WIFI = 1, PLANROLE_WIFI24 = 2, PLANROLE_WIFI5 = 3, PLANROLE_BLE = 4 };   // scan-plan role (not fleet::ROLE_*)

#pragma pack(push, 1)

struct Hdr     { uint8_t ver, type; uint16_t sess; uint32_t seq; };                       //  8
struct Probe   { Hdr h; uint8_t nl[16]; uint8_t ch, rsv; };                                // 26 bcast, clear
struct Hello   { Hdr h; uint8_t nl[16], nc[16], tag[16]; uint16_t maxPayload; };           // 58 unicast, clear
struct Join    { Hdr h; uint8_t nl[16], nc[16]; uint8_t slot, nMembers; uint16_t maxPayload;
                 uint8_t proof[16]; };                                                     // 60 unicast, clear
struct JoinAck { Hdr h; uint8_t proof[16]; uint8_t board, caps; uint8_t bleMac[6];
                 char fw[12]; char name[20]; };                                            // 64 encrypted
// Fixed radios: 3 slots per unit, the leader first, then members in slot order: [BLE advert, C5 Wi-Fi, download AP].
// All-zero = unknown. The rest of Sync.selfMacs (up to one per unit) are connected phones.
constexpr uint8_t FLEET_RADIOS   = 3 * (1 + FLEET_MAX_MEMBERS);
constexpr uint8_t FLEET_SELF_MAX = FLEET_RADIOS + 1 + FLEET_MAX_MEMBERS;
struct Sync    { Hdr h; uint32_t utc; int16_t tzOffMin; uint8_t tsrc, psrc;
                 int32_t lat, lon; uint32_t fixAgeMs; uint8_t ch, flags; uint8_t leaderSid[6];
                 uint8_t nSelf, nRadio; uint8_t selfMacs[FLEET_SELF_MAX][6]; char leaderName[20];
                 uint8_t fleetId[8]; uint8_t fixSlot; };   // 235 encrypted, every 2 s; the first nRadio (= FLEET_RADIOS) of selfMacs are fixed radios (kept in NVS), the rest phones;
                                          // v2: fleetId = the leader's random per-boot fleet session id (log `fleet_id`); fixSlot = the member slot whose GPS
                                          // IS the fleet position (0 = none: the leader's phone / own GPS) -> devpos `used`
struct Plan    { Hdr h; uint8_t planGen, role, slot, nMembers; link_protocol::ScanPlan plan; };   // 30 encrypted (P4)
struct Ctrl    { Hdr h; uint8_t op; uint8_t arg[3]; };                                     // 12 encrypted
struct MStat   { Hdr h; uint8_t planGen, c5Link; int8_t bat; uint8_t gpsFix;
                 uint16_t dets, deauth; uint8_t evil, rid, tsrc, psrc; uint32_t uniq;
                 uint16_t heapKB, txFailPm; int32_t lat, lon; uint8_t phoneMac[6];
                 uint8_t srcMask; uint8_t bleMac[6], c5Mac[6], apMac[6];
                 uint8_t irk[16]; };                                                       // 77 encrypted, every 2 s (phoneMac: its own phone; srcMask: its enabled sources;
                                          // v5: irk = its BLE identity resolving key, all-zero = none, e.g. a CYD client)
// v5: every unit's BLE IRK, slot 0 = the leader, k+1 = member k (all-zero = unknown). Leader -> client every 10 s and on a change.
// A unit's advert address changes every 15 min; the IRK resolves any of them, so no old address has to be remembered.
struct Irks    { Hdr h; uint8_t n; uint8_t irk[1 + FLEET_MAX_MEMBERS][16]; };            // 121 encrypted
// fw 0.36.0: the fleet-wide threat readout, leader -> each client right after every Sync, so a client's tiles, alert
// banner and LED show what the whole fleet sees (the leader merges every unit's rows). Additive: no version bump, a
// unit that doesn't know the type ignores it. Per category id (host_core Category, < FLEET_CATS): device count
// (capped 255), worst tier, best signature score. flags bit0 = deauth alert up, bit1 = evil-twin alert up.
constexpr uint8_t FLEET_CATS = 16;
struct Threats { Hdr h; uint8_t n; uint8_t cnt[FLEET_CATS], worst[FLEET_CATS], score[FLEET_CATS];
                 uint8_t follow, flags; uint16_t deauth; uint8_t evil, rid; };              // 63 encrypted
struct DetBatchHdr { Hdr h; uint16_t cycle; uint8_t idx, cnt; };                          // 12 + cnt*81
struct RidBatchHdr { Hdr h; uint8_t cnt, rsv; };                                          // 10 + cnt*75
// P6 item 6 (docs §22): LogRows client->leader = complete newline-terminated lines of the client's own log file, verbatim,
// right after the header (nLines of them; bytes = frame - sizeof(LogRowsHdr) - TAG_LEN). fileId = the origin file's
// file_id (8 hex = 4 bytes); firstRow = the 1-based line number of the first line in that file; byteOff = its offset.
// flags bit0 = a re-sent anchor (meta / timesync: stored every time). LogAck leader->client echoes the span it holds.
struct LogRowsHdr { Hdr h; uint8_t fileId[4]; uint32_t firstRow, byteOff; uint8_t flags, nLines; };   // 22 + lines
struct LogAck     { Hdr h; uint8_t fileId[4]; uint32_t throughRow, nextByteOff; };                       // 20
constexpr uint8_t LOGROWS_ANCHOR = 0x01;
// P1 bench frame (DbgPing leader->bcast, DbgPong client->leader): the whole frame is padded to the size the
// leader picked; padLen = frame length - sizeof(DbgPing).
struct DbgPing { Hdr h; uint16_t padLen; };                                                // 10 + padLen
#pragma pack(pop)

static_assert(sizeof(Hdr) == 8, "fleet wire size");
static_assert(sizeof(Probe) == 26 && sizeof(Hello) == 58 && sizeof(Join) == 60 && sizeof(JoinAck) == 64, "fleet wire size");
static_assert(sizeof(Sync) == 235 && sizeof(Plan) == 30 && sizeof(Ctrl) == 12 && sizeof(MStat) == 77, "fleet wire size");
static_assert(sizeof(Irks) == 121 && sizeof(Threats) == 63, "fleet wire size");
// Sync.flags: bit0 time valid (utc >= 2023), bit1 position valid, bit2 position LIVE at the leader, bit3 tz known.
static_assert(sizeof(LogRowsHdr) == 22 && sizeof(LogAck) == 20, "fleet wire size");
static_assert(sizeof(DetBatchHdr) == 12 && sizeof(RidBatchHdr) == 10 && sizeof(DbgPing) == 10, "fleet wire size");
// A full v2 DetBatch / RidBatch must fit one v2 ESP-NOW frame.
static_assert(sizeof(DetBatchHdr) + DETS_PER_V2 * sizeof(link_protocol::Detection) <= FLEET_V2_MAX, "DetBatch v2 fits");
static_assert(sizeof(RidBatchHdr) + RIDS_PER_V2 * sizeof(link_protocol::RemoteId) <= FLEET_V2_MAX, "RidBatch v2 fits");

}  // namespace fleet_proto

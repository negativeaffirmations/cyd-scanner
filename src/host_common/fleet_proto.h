// fleet_proto.h — host-to-host fleet wire protocol (docs/fleet-mode.md §4). HOST-ONLY: the C5s never include it.
// All structs are packed, little-endian. FLEET_PROTO_VERSION is independent of the CYD<->C5 link version
// and of DETS. P1: Hdr + the FLEET_DEBUG ping/pong. P2: Probe/Hello/Join/JoinAck, Sync + MStat as heartbeats, Ctrl.
// Session frames (JoinAck and later) carry an 8-byte HMAC tag AFTER the struct (fleet_internal.h TAG_LEN), so their
// on-air length is sizeof(struct) + 8. Sizes are pinned by static_asserts.
#pragma once
#include <stdint.h>
#include "link_protocol.h"

namespace fleet_proto {

constexpr uint8_t  FLEET_PROTO_VERSION = 1;
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
                           Sync = 0x10, Plan = 0x11, Ctrl = 0x12,
                           MStat = 0x20, DetBatch = 0x21, RidBatch = 0x22,
                           DbgPing = 0xF0, DbgPong = 0xF1 };   // Dbg*: P1 bench frames, sent only under -DFLEET_DEBUG
enum CtrlOp : uint8_t { CTRL_RELEASE = 1, CTRL_UNJOIN = 2, CTRL_NEWSESSION = 3, CTRL_SETCH = 4 };
enum PlanRole : uint8_t { PLANROLE_FULL = 0, PLANROLE_WIFI = 1, PLANROLE_WIFI24 = 2, PLANROLE_WIFI5 = 3, PLANROLE_BLE = 4 };   // scan-plan role (not fleet::ROLE_*)

#pragma pack(push, 1)
// Same shape as link_protocol::ScanPlan will have in link v12 (P4). Mirrored here so the P1 wire structs
// compile without touching the link protocol; replaced by the real type in P4.
struct ScanPlanWire { uint8_t b[18]; };

struct Hdr     { uint8_t ver, type; uint16_t sess; uint32_t seq; };                       //  8
struct Probe   { Hdr h; uint8_t nl[16]; uint8_t ch, rsv; };                                // 26 bcast, clear
struct Hello   { Hdr h; uint8_t nl[16], nc[16], tag[16]; uint16_t maxPayload; };           // 58 unicast, clear
struct Join    { Hdr h; uint8_t nl[16], nc[16]; uint8_t slot, nMembers; uint16_t maxPayload;
                 uint8_t proof[16]; };                                                     // 60 unicast, clear
struct JoinAck { Hdr h; uint8_t proof[16]; uint8_t board, caps; uint8_t bleMac[6];
                 char fw[12]; char name[20]; };                                            // 64 encrypted
struct Sync    { Hdr h; uint32_t utc; int16_t tzOffMin; uint8_t tsrc, psrc;
                 int32_t lat, lon; uint32_t fixAgeMs; uint8_t ch, flags; uint8_t leaderSid[6];
                 uint8_t nSelf; uint8_t selfMacs[9][6]; char leaderName[20]; };            // 111 encrypted, every 2 s; 9 = leader BLE + phone peer + SoftAP BSSID + 6 members
struct Plan    { Hdr h; uint8_t planGen, role, slot, nMembers; ScanPlanWire plan; };       // 30 encrypted
struct Ctrl    { Hdr h; uint8_t op; uint8_t arg[3]; };                                     // 12 encrypted
struct MStat   { Hdr h; uint8_t planGen, c5Link; int8_t bat; uint8_t gpsFix;
                 uint16_t dets, deauth; uint8_t evil, rid, tsrc, psrc; uint32_t uniq;
                 uint16_t heapKB, txFailPm; int32_t lat, lon; uint8_t phoneMac[6]; };      // 42 encrypted, every 2 s (phoneMac: its own phone, a fleet self MAC)
struct DetBatchHdr { Hdr h; uint16_t cycle; uint8_t idx, cnt; };                          // 12 + cnt*81
struct RidBatchHdr { Hdr h; uint8_t cnt, rsv; };                                          // 10 + cnt*75
// P1 bench frame (DbgPing leader->bcast, DbgPong client->leader): the whole frame is padded to the size the
// leader picked; padLen = frame length - sizeof(DbgPing).
struct DbgPing { Hdr h; uint16_t padLen; };                                                // 10 + padLen
#pragma pack(pop)

static_assert(sizeof(Hdr) == 8, "fleet wire size");
static_assert(sizeof(Probe) == 26 && sizeof(Hello) == 58 && sizeof(Join) == 60 && sizeof(JoinAck) == 64, "fleet wire size");
static_assert(sizeof(Sync) == 111 && sizeof(Plan) == 30 && sizeof(Ctrl) == 12 && sizeof(MStat) == 42, "fleet wire size");
// Sync.flags: bit0 time valid (utc >= 2023), bit1 position valid, bit2 position LIVE at the leader, bit3 tz known.
static_assert(sizeof(DetBatchHdr) == 12 && sizeof(RidBatchHdr) == 10 && sizeof(DbgPing) == 10, "fleet wire size");
// A full v2 DetBatch / RidBatch must fit one v2 ESP-NOW frame.
static_assert(sizeof(DetBatchHdr) + DETS_PER_V2 * sizeof(link_protocol::Detection) <= FLEET_V2_MAX, "DetBatch v2 fits");
static_assert(sizeof(RidBatchHdr) + RIDS_PER_V2 * sizeof(link_protocol::RemoteId) <= FLEET_V2_MAX, "RidBatch v2 fits");

}  // namespace fleet_proto

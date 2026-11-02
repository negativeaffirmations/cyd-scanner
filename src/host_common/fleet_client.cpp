// fleet_client.cpp — see fleet.h and docs/fleet-mode.md §1.5, §5.3-§5.4. Client state machine:
//   SEARCH  listen for a Probe (dwell 9 s on the last channel, then 1 / 6 / 11; after a fruitless pass, all of 1-13),
//           answer at most one per 2 s. While this unit's Wi-Fi download AP is up the radio stays on the fleet channel.
//   HELLO   Hello sent (tag over the probe nonce, our nonce and both MACs, keyed by this unit's fleet key FK)
//   JOINING Join verified (its proof authenticates the leader), session key derived, JoinAck resent every 300 ms
//   JOINED  first tagged Sync arrived; MStat every 2 s; leader lost after 15 s without a Sync
// After a session ends the next Hello goes out from a fresh random MAC (docs §16.4).
#include <Arduino.h>
#include <Preferences.h>
#include <new>
#include "fleet.h"
#include "fleet_internal.h"
#include "devkey.h"
#include "host_board.h"
#include "webshare.h"
#include "phone.h"
#include "host_core.h"
#include "host_log.h"

using namespace fleet::detail;

namespace {

constexpr uint32_t DWELL_MS = 9000, HELLO_GAP_MS = 2000, JOIN_WAIT_MS = 5000;
constexpr uint32_t ACK_EVERY_MS = 300, ACK_GIVEUP_MS = 1000, SYNC_LOST_MS = 15000, MSTAT_MS = 2000;
constexpr uint8_t  ACK_TRIES = 5;
constexpr uint32_t TIME_FRESH_MS = 15000;   // a joined client trusts the leader's clock this long after a Sync
constexpr uint32_t MIN_UTC = 1700000000UL;  // anything earlier (~2023) is not a real clock
// P5 forwarding (docs §9.1): resend a row when its RSSI moved >= 6 dB, its identity fields changed, or every 10 s
// (keeps it alive in the leader's 30 s remote table); at most 6 frames per cycle, 5 ms apart.
constexpr uint32_t FWD_REFRESH_MS = 10000;
constexpr int      FWD_RSSI_DB = 6, FWD_MAX_FRAMES = 6;

// What was last sent for one device (heap, MAX_DET entries; ~32 B each).
struct SentRow {
  bool     used;
  uint8_t  src;
  uint8_t  mac[6];
  uint16_t pan;
  int8_t   rssi;
  uint16_t flags;
  uint16_t cid;
  uint32_t ie, nameHash, lastSentMs;
};

enum CState : uint8_t { C_SEARCH, C_HELLO, C_JOINING, C_JOINED };

struct ClientSt {
  CState   st;
  uint8_t  fk[KEY_LEN];
  bool     fkOk;
  uint8_t  leaderMac[6];
  uint8_t  nl[NONCE_LEN], nc[NONCE_LEN];
  uint32_t helloMs, lastHelloMs;
  uint16_t sess;
  uint8_t  sk[KEY_LEN];
  uint32_t txSeq;
  SeqWin   rx;
  uint8_t  slot, nMembers;
  uint32_t lastAckMs;
  uint8_t  acks;
  uint32_t lastSyncMs, lastMStatMs;
  int8_t   rssi;
  char     leaderName[21];
  uint8_t  leaderSid[6], fleetId[8];   // P6: from the Sync (log fleet_leader / fleet_id); haveIdent once one arrived
  uint8_t  fixSlot;                    // P6: Sync.fixSlot (whose GPS is the fleet position; 0 = none)
  bool     haveIdent;
  bool     needRemac;
  bool     fullSweep;                  // this pass covers 1-13 (the preferred list found nothing)
  uint8_t  hop[13], nHop, hopIdx;
  uint32_t hopStartMs;
  uint8_t  pendingCh;                  // CTRL_SETCH that arrived while the SoftAP was up (applied when it stops)
  // P3: latched from the last Sync that carried them (kept after the leader goes: free-running / held fallbacks)
  uint32_t fltUtc, fltUtcAtMs;         // leader UTC and the millis() it arrived
  float    fltLat, fltLon;
  uint32_t fltFixAgeRx, fltFixAtMs;    // the fix's age when the Sync arrived + that millis()
  bool     haveTime, havePos;
  bool     fltPosLive;                 // the leader marked its fix live (Sync flags bit2)
  uint8_t  selfMacs[9][6];
  uint8_t  nSelf;
  // P4: the scan role the leader assigned (reset to FULL whenever the session ends)
  link_protocol::ScanPlan plan;
  uint8_t  planRole, planGen;
  // P5 forwarding
  uint16_t fwdCycle;
  uint16_t fwdStart;                   // where the last capped cycle stopped: the next one resumes there (no starving)
  uint16_t lastDets;                   // rows of the last successful cycle (MStat.dets; steady across a busy miss)
  uint32_t rowsSent, framesSent, ridFrames;
};
// P6 item 6 store-and-forward (docs §22): this boot's log files go to the leader from line 1, read straight from the
// unit's own SD files at acked byte offsets (no RAM queue). Each file (live fg / bg stream, or one rotated away earlier
// this boot) keeps its own acked position, so switching streams or a New Session resumes instead of re-sending.
// Stop-and-wait: one LogRows frame in flight, rebuilt and resent if no LogAck within retryMs (600 ms, doubling to 5 s,
// reset on an ack); at most ~LOG_BYTES_PER_S so DetBatch / MStat keep their airtime.
constexpr uint32_t LOG_RESEND_MS = 600, LOG_RESEND_MAX_MS = 5000, LOG_IDLE_POLL_MS = 500, LOG_BYTES_PER_S = 8192;
constexpr size_t   LOG_MIN_ROOM = 520;    // a full 512 B log line must fit one frame (false on a v1 / 250 B link: no forwarding)
constexpr int      LOG_POS = 10;          // files tracked: live fg + bg + the 8 most recent rotated ones
enum AnchorSt : uint8_t { A_NONE = 0, A_META, A_TS, A_FL };   // after (re)joining: this file's meta line, timesync line, then its last fleet evt line, re-sent
struct LogPos {
  char     id[9];                      // file_id ("" = free slot)
  uint32_t ackOff, ackRow;             // acked position: byte offset of the next unsent line + its 1-based row
  uint32_t metaLen;                    // line 1 when it is the meta line (recorded as it is acked), else 0
  uint32_t tsOff, tsRow, tsLen;        // the first timesync line (tsLen 0 = none acked yet)
  uint32_t flOff, flRow, flLen;        // the LAST "evt":"fleet" line carrying a fleet_id (flLen 0 = none acked yet)
  uint32_t lastSz, stamp;              // size at the last torn-tail poll / LRU stamp
  uint8_t  anchor, torn;               // AnchorSt for the current session / unchanged torn-tail polls
  bool     finished;                   // a rotated file fully forwarded (or abandoned): never polled again
};
// Carry-over across a reboot (fw 0.27.0): the acked positions of the most recent not-fully-forwarded files (this boot's
// plus still-pending carried ones), kept in NVS "cydscan"/"fltcarry" so the NEXT boot forwards what this one did not. At
// most CARRY_MAX are stored and CARRY_LOAD loaded; written when the file set / a path changes and otherwise at most
// every CARRY_PUT_MS (flash wear: a few writes/min while rows flow, none when idle).
constexpr int      CARRY_MAX = 3, CARRY_LOAD = 2;
constexpr uint8_t  CARRY_VER = 1;
constexpr uint32_t CARRY_PUT_MS = 30000, CARRY_CHK_MS = 1000;
struct CarryEnt {
  char     id[9], path[48];
  uint32_t ackOff, ackRow, metaLen, tsOff, tsRow, tsLen, flOff, flRow, flLen;
};
struct CarryRec { uint8_t ver, n; CarryEnt e[CARRY_MAX]; };
struct LogFwd {
  LogPos   pos[LOG_POS];
  CarryRec saved;                      // what NVS holds (zero-padded, so memcmp compares it)
  CarryEnt carry[CARRY_LOAD];          // previous-boot files still to drain / verify (id "" = empty)
  uint32_t savedMs, carryChkMs;
  uint32_t carryLoaded, carryGone, carryDone;
  uint32_t inflFlOff, inflFlRow, inflFlLen;   // last fleet evt line found in the in-flight data frame
  uint32_t clock;
  bool     infl, inflAnchor, warnedV1;   // a frame awaits its LogAck
  char     inflId[9];
  uint32_t inflOff, inflLen, inflLines, inflRow, inflMs;
  uint32_t inflMetaLen, inflTsOff, inflTsRow, inflTsLen;   // meta / timesync lines found in the in-flight data frame
  uint32_t retryMs, nextMs;
  uint32_t rowsFwd, frames, resends, skipped, anchorsSent;
};
LogFwd* g_lf = nullptr;

ClientSt* C = nullptr;
SentRow*  g_sent = nullptr;            // MAX_DET rows, allocated with the client state (normal mode: never)
uint8_t*  g_fwdBuf = nullptr;          // one sealed DetBatch / RidBatch frame

uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h;
}

// Same device for forwarding purposes: source + MAC, plus the PAN for a short 802.15.4 address.
bool sameKey(const SentRow& e, const Detection& d) {
  if (e.src != d.source || memcmp(e.mac, d.mac, 6)) return false;
  if (d.source == (uint8_t)Source::Ieee802154 && !(d.flags & FLAG_154_EXTENDED)) return e.pan == d.panId;
  return true;
}

// Should this row go out now? Updates the sent table when it should.
bool needsSend(const Detection& d, uint32_t now) {
  const uint32_t nh = fnv1a(d.name);
  int hit = -1, oldest = 0;
  for (int i = 0; i < MAX_DET; i++) {
    if (g_sent[i].used && sameKey(g_sent[i], d)) { hit = i; break; }
    if (!g_sent[i].used) { if (g_sent[oldest].used) oldest = i; }
    else if (g_sent[oldest].used && g_sent[i].lastSentMs < g_sent[oldest].lastSentMs) oldest = i;
  }
  if (hit >= 0) {
    const SentRow& e = g_sent[hit];
    const int dr = (int)d.rssi - (int)e.rssi;
    if (dr < FWD_RSSI_DB && dr > -FWD_RSSI_DB && e.flags == d.flags && e.ie == d.ie_hash && e.cid == d.companyId &&
        e.nameHash == nh && now - e.lastSentMs < FWD_REFRESH_MS) return false;
  }
  SentRow& e = g_sent[hit >= 0 ? hit : oldest];                    // new device: a free slot, else the stalest
  e.used = true; e.src = d.source; memcpy(e.mac, d.mac, 6); e.pan = d.panId;
  e.rssi = d.rssi; e.flags = d.flags; e.cid = d.companyId; e.ie = d.ie_hash; e.nameHash = nh; e.lastSentMs = now;
  return true;
}

// Preferred pass: the saved channel, then 1 / 6 / 11. Full pass: the saved channel, then every other one, so a member
// that missed a move to e.g. ch 3 still finds its leader.
void buildHops(bool full) {
  C->fullSweep = full;
  C->nHop = 0;
  auto addCh = [](uint8_t c) {
    for (int k = 0; k < C->nHop; k++) if (C->hop[k] == c) return;
    C->hop[C->nHop++] = c;
  };
  addCh(g_ch);
  if (full) { for (uint8_t c = 1; c <= 13; c++) addCh(c); }
  else { addCh(1); addCh(6); addCh(11); }
  C->hopIdx = 0;
  C->hopStartMs = millis();
  setRadioChannel(C->hop[0]);
}

bool ensureFk() {
  if (!C->fkOk) C->fkOk = ownFleetKey(C->fk);
  return C->fkOk;
}

void toSearch(const char* why, bool remac) {
  if (C->st != C_SEARCH) peerDel(C->leaderMac);
  if (C->planRole != PLANROLE_FULL) Serial.println("[FLT] client: no leader -> scanning everything (full)");
  C->plan = link_protocol::ScanPlan{}; C->planRole = PLANROLE_FULL;   // a client on its own covers everything (§8.4)
  memset(C->sk, 0, sizeof(C->sk));
  C->st = C_SEARCH;
  C->needRemac = C->needRemac || remac;
  buildHops(false);
  if (why) Serial.printf("[FLT] client: %s -> searching\n", why);
}

void sendAck() {
  uint8_t buf[sizeof(JoinAck) + TAG_LEN];
  JoinAck* a = (JoinAck*)buf;
  memset(buf, 0, sizeof(buf));
  uint8_t t[32];
  C->acks++;
  C->lastAckMs = millis();
  if (!Hmac(C->fk, KEY_LEN).add("CFLO-FLT-ACK-1").add(C->nl, NONCE_LEN).add(C->nc, NONCE_LEN).add16(C->sess).finish(t)) return;
  memcpy(a->proof, t, sizeof(a->proof));
  a->board = hostboard::fleetLeaderCapable() ? 2 : 1;            // 1 CYD / 2 MINI
  a->caps  = (g_onbAtMs ? 0x01 : 0) | (hostboard::fleetLeaderCapable() ? 0x08 : 0);   // bit0 GPS seen, bit3 phone link
  phone::ownMac(a->bleMac);                                        // a mini client's BLE advert: the fleet must not log it
  strncpy(a->fw, devkey::fwVersion(), sizeof(a->fw) - 1);
  strncpy(a->name, devkey::unitName(), sizeof(a->name) - 1);
  sendSealed(C->leaderMac, buf, sizeof(JoinAck), Msg::JoinAck, C->sess, C->txSeq, C->sk);
}

void sendMStat() {
  uint8_t buf[sizeof(MStat) + TAG_LEN];
  MStat* m = (MStat*)buf;
  memset(buf, 0, sizeof(buf));
  fleet::Ui u; fleet::uiSnapshot(u);
  const uint32_t tot = u.txOk + u.txFail;
  m->heapKB = (uint16_t)(ESP.getFreeHeap() / 1024);
  m->txFailPm = tot ? (uint16_t)((uint64_t)u.txFail * 1000 / tot) : 0;
  m->bat = (int8_t)hostboard::batteryPercent();
  m->planGen = C->planGen;
  m->dets = C->lastDets;                                           // P5: scan counters (the leader shows them)
  m->deauth = g_deauthRecent; m->evil = g_evilCount; m->rid = g_ridCount;
  m->srcMask = g_srcMask;                                          // the leader only assigns roles this unit can run
  m->c5Link = g_linkOk ? 1 : 0;
  // This unit's OWN live GPS fix (not the fleet position it stamps): the leader may use it as the fleet position
  // when it has no live fix of its own (docs §16.6). P5 fills the scan counters.
  const bool ownFix = g_onbValid && g_onbAtMs && millis() - g_onbAtMs < 10000;
  m->gpsFix = ownFix ? 1 : 0;
  if (ownFix) { m->lat = (int32_t)lroundf(g_onbLat * 1e7f); m->lon = (int32_t)lroundf(g_onbLon * 1e7f); }
  phone::peerMac(m->phoneMac);                                     // a mini client's own phone: the fleet must not log it
  sendSealed(C->leaderMac, buf, sizeof(MStat), Msg::MStat, C->sess, C->txSeq, C->sk);
  C->lastMStatMs = millis();
}

void onProbe(const Rx& f) {
  if (C->st != C_SEARCH || f.len != sizeof(Probe)) return;
  const uint32_t now = millis();
  if (C->lastHelloMs && now - C->lastHelloMs < HELLO_GAP_MS) return;
  if (!ensureFk()) return;
  Probe p; memcpy(&p, f.data, sizeof(p));
  if (C->needRemac && !webshare::active()) {                       // a fresh MAC for each new session (docs §16.4)
    if (!fleet::rerandomizeMac()) {
      // The radio didn't come back (or kept the old MAC): rather than sit offline holding ~50 KB of Wi-Fi, or reuse
      // the old MAC, reboot -- the fleet role is in NVS, so the unit comes straight back as a client.
      Serial.println("[FLT] client: radio restart failed -- rebooting");
      event("radio_fail");
      Serial.flush(); delay(200);
      ESP.restart();
    }
    C->needRemac = false;
    setRadioChannel(C->hop[C->hopIdx]);                            // the restart retuned to g_ch; stay where the probe was heard
  }
  memcpy(C->leaderMac, f.mac, 6);
  memcpy(C->nl, p.nl, NONCE_LEN);
  randomBytes(C->nc, NONCE_LEN);
  Hello h = {};
  h.h.ver = FLEET_PROTO_VERSION; h.h.type = (uint8_t)Msg::Hello;
  memcpy(h.nl, C->nl, NONCE_LEN); memcpy(h.nc, C->nc, NONCE_LEN);
  uint8_t t[32];
  if (!Hmac(C->fk, KEY_LEN).add("CFLO-FLT-HELLO-1").add(C->nl, NONCE_LEN).add(C->nc, NONCE_LEN)
           .add(C->leaderMac, 6).add(g_mac, 6).finish(t)) return;
  memcpy(h.tag, t, sizeof(h.tag));
  h.maxPayload = g_maxPayload;
  if (!peerSet(C->leaderMac, nullptr)) return;
  if (!sendClear(C->leaderMac, &h, sizeof(h))) { peerDel(C->leaderMac); return; }   // no stray peers left behind
  C->st = C_HELLO;
  C->helloMs = C->lastHelloMs = now;
}

void onJoin(const Rx& f) {
  if (C->st != C_HELLO || f.len != sizeof(Join) || memcmp(f.mac, C->leaderMac, 6)) return;
  if (millis() - C->helloMs > JOIN_WAIT_MS) return;
  Join j; memcpy(&j, f.data, sizeof(j));
  if (j.h.sess == 0 || memcmp(j.nl, C->nl, NONCE_LEN) || memcmp(j.nc, C->nc, NONCE_LEN)) return;
  uint8_t t[32], sk[32];
  if (!Hmac(C->fk, KEY_LEN).add("CFLO-FLT-JOIN-1").add(C->nl, NONCE_LEN).add(C->nc, NONCE_LEN).add16(j.h.sess)
           .add8(j.slot).add8(j.nMembers).finish(t)) return;
  if (!ctEqual(t, j.proof, sizeof(j.proof))) { Serial.println("[FLT] client: Join proof failed (not our leader)"); return; }
  if (!Hmac(C->fk, KEY_LEN).add("CFLO-FLT-SK-1").add(C->nl, NONCE_LEN).add(C->nc, NONCE_LEN).add16(j.h.sess)
           .add(C->leaderMac, 6).add(g_mac, 6).finish(sk)) return;
  C->sess = j.h.sess; C->slot = j.slot; C->nMembers = j.nMembers;
  memcpy(C->sk, sk, KEY_LEN);
  memset(sk, 0, sizeof(sk));
  memset(t, 0, sizeof(t));
  if (!peerSet(C->leaderMac, C->sk)) { toSearch("encrypt failed", true); return; }
  C->txSeq = 0; C->rx.reset(); C->acks = 0;
  C->st = C_JOINING;
  if (g_sent) memset(g_sent, 0, MAX_DET * sizeof(SentRow));        // a new session: forward every row afresh
  if (g_lf) g_lf->infl = false;                                    // (onSync, once JOINED, restarts the anchors)
  sendAck();
}

bool sessionOk(const Rx& f, size_t structLen) {
  if ((C->st != C_JOINING && C->st != C_JOINED) || memcmp(f.mac, C->leaderMac, 6) || f.len != structLen + TAG_LEN) return false;
  const Hdr* h = (const Hdr*)f.data;
  if (h->sess != C->sess || !tagOk(f.data, f.len, C->sk)) return false;
  return C->rx.accept(h->seq);
}

// A fleet event with the P6 extras: the leader as the peer, this unit's slot. Call it while still JOINED so the log
// line also carries the fleet id (fleetIdent()).
void clientEvent(const char* op) {
  fleet::EventInfo x = {};
  x.slot = -1; x.n = -1;
  if (C->haveIdent) {
    for (int k = 0; k < 6; k++) snprintf(x.peer + 2 * k, 3, "%02x", C->leaderSid[k]);
    strncpy(x.peerName, C->leaderName, sizeof(x.peerName) - 1);
  }
  if (C->slot) x.slot = (int8_t)C->slot;
  event(op, x);
}

void onSync(const Rx& f) {
  if (!sessionOk(f, sizeof(Sync))) return;
  Sync s; memcpy(&s, f.data, sizeof(s));
  memcpy(C->leaderName, s.leaderName, sizeof(s.leaderName)); C->leaderName[20] = 0;
  memcpy(C->leaderSid, s.leaderSid, 6); memcpy(C->fleetId, s.fleetId, 8); C->haveIdent = true; C->fixSlot = s.fixSlot;
  const uint32_t now = millis();
  C->lastSyncMs = now;
  C->rssi = f.rssi;
  // P3: the fleet clock / position / tz / self MACs (flags bit0 time valid, bit1 position valid)
  if ((s.flags & 0x01) && s.utc >= MIN_UTC) { C->fltUtc = s.utc; C->fltUtcAtMs = now; C->haveTime = true; }
  if (s.flags & 0x08) phone::setTzOffsetMin(s.tzOffMin);           // only a tz the leader really knows (never its default 0)
  if ((s.flags & 0x02) && s.lat >= -900000000 && s.lat <= 900000000 && s.lon >= -1800000000 && s.lon <= 1800000000) {
    C->fltLat = s.lat / 1e7f; C->fltLon = s.lon / 1e7f;
    C->fltFixAgeRx = s.fixAgeMs; C->fltFixAtMs = now; C->havePos = true;
  }
  C->fltPosLive = (s.flags & 0x06) == 0x06;
  C->nSelf = s.nSelf <= 9 ? s.nSelf : 9;
  memcpy(C->selfMacs, s.selfMacs, sizeof(C->selfMacs));
  if (C->st == C_JOINING) {
    C->st = C_JOINED;
    const uint8_t here = C->hop[C->hopIdx];
    if (here != g_ch) fleet::setChannel(here);                      // found the leader elsewhere: remember that channel
    Serial.printf("[FLT] client: JOINED %s (slot %u of %u, ch %u)\n", C->leaderName, (unsigned)C->slot,
                  (unsigned)C->nMembers, (unsigned)g_ch);
    clientEvent("join");
    C->lastMStatMs = 0;
    if (g_lf) {                                                     // P6 item 6: this leader gets each file's meta + timesync again
      g_lf->infl = false; g_lf->nextMs = 0; g_lf->retryMs = LOG_RESEND_MS;
      for (LogPos& q : g_lf->pos) q.anchor = q.ackOff ? A_META : A_NONE;   // nothing acked yet: line 1 goes out as an ordinary row
    }
  }
}

void onPlan(const Rx& f) {
  if (C->st != C_JOINED || !sessionOk(f, sizeof(Plan))) return;
  Plan p; memcpy(&p, f.data, sizeof(p));
  if (p.role > PLANROLE_BLE) return;
  if (p.slot >= 1 && p.slot <= FLEET_MAX_MEMBERS) C->slot = p.slot;   // a removed member shifts the slots after it
  const bool changed = p.planGen != C->planGen || p.role != C->planRole;
  C->plan = p.plan; C->planRole = p.role; C->planGen = p.planGen;
  if (changed) {
    Serial.printf("[FLT] client: scan role %s (plan %u)\n", fleet::planRoleName(p.role), (unsigned)p.planGen);
    clientEvent("plan");
  }
}

void onCtrl(const Rx& f) {
  if (C->st != C_JOINED || !sessionOk(f, sizeof(Ctrl))) return;
  Ctrl c; memcpy(&c, f.data, sizeof(c));
  C->lastSyncMs = millis();
  switch (c.op) {
    case CTRL_RELEASE:                                             // leave fleet mode (a CYD returns to its phone link)
      Serial.println("[FLT] client: released by the leader -- restarting in normal mode");
      clientEvent("release");
      fleet::setRole(fleet::ROLE_OFF);
      Serial.flush();
      delay(300);
      ESP.restart();
      break;
    case CTRL_UNJOIN:
      clientEvent("unjoin");
      toSearch("removed by the leader", true);
      break;
    case CTRL_SETCH:                                               // sent 3x: repeats of the same channel are no-ops
      if (c.arg[0] >= 1 && c.arg[0] <= 13 && c.arg[0] != g_ch) {
        if (webshare::active()) { C->pendingCh = c.arg[0]; break; }   // keep this unit's live download AP where it is
        Serial.printf("[FLT] client: leader moves the fleet to ch %u\n", (unsigned)c.arg[0]);
        fleet::setChannel(c.arg[0]);
        C->hop[C->hopIdx] = c.arg[0];
      }
      break;
    default: break;
  }
}

// ---- P6 item 6: log store-and-forward (client side) ----
bool idToBytes(const char* id, uint8_t out[4]) {
  for (int i = 0; i < 4; i++) {
    unsigned v;
    if (sscanf(id + 2 * i, "%2x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

LogPos* findPos(LogFwd* W, const char* id, bool create) {
  for (LogPos& q : W->pos) if (q.id[0] && !strcmp(q.id, id)) { q.stamp = ++W->clock; return &q; }
  if (!create) return nullptr;
  LogPos* v = nullptr;                                               // a free slot, else a finished file, else the stalest
  for (LogPos& q : W->pos) {
    if (!q.id[0]) { v = &q; break; }
    if (W->infl && !strcmp(q.id, W->inflId)) continue;
    if (!v || (q.finished && !v->finished) || (q.finished == v->finished && q.stamp < v->stamp)) v = &q;
  }
  if (!v) return nullptr;
  memset(v, 0, sizeof(*v));
  memcpy(v->id, id, 9);
  v->ackRow = 1; v->stamp = ++W->clock;
  return v;
}

// Build + send one LogRows frame from the `len` line bytes already placed at g_fwdBuf + sizeof(LogRowsHdr).
void sendLogFrame(const char* id, uint32_t row, uint32_t off, uint32_t len, uint32_t lines, bool anchor) {
  LogFwd* W = g_lf;
  LogRowsHdr* h = (LogRowsHdr*)g_fwdBuf;
  memset(h, 0, sizeof(*h));
  if (!idToBytes(id, h->fileId)) return;
  h->firstRow = row; h->byteOff = off; h->flags = anchor ? LOGROWS_ANCHOR : 0; h->nLines = (uint8_t)lines;
  const uint32_t now = millis();
  if (!sendSealed(C->leaderMac, g_fwdBuf, sizeof(LogRowsHdr) + len, Msg::LogRows, C->sess, C->txSeq, C->sk)) {
    W->nextMs = now + 200;                                           // radio queue busy / crypto: try again shortly
    return;
  }
  W->infl = true; W->inflAnchor = anchor; W->inflMs = now;
  memcpy(W->inflId, id, 9);
  W->inflOff = off; W->inflLen = len; W->inflLines = lines; W->inflRow = row;
}

bool lineHas(const uint8_t* ln, size_t ll, const char* k, size_t kl) {
  for (size_t i = 0; i + kl <= ll; i++) if (ln[i] == '"' && !memcmp(ln + i, k, kl)) return true;
  return false;
}
bool lineHasTimesync(const uint8_t* ln, size_t ll) { static const char k[] = "\"evt\":\"timesync\""; return lineHas(ln, ll, k, sizeof(k) - 1); }
// A fleet event line that names its session (join / plan / start ...): the anchor that keeps a file's tail attributed to its fleet.
bool lineHasFleetEvt(const uint8_t* ln, size_t ll) {
  static const char k[] = "\"evt\":\"fleet\"", k2[] = "\"fleet_id\":";
  return lineHas(ln, ll, k, sizeof(k) - 1) && lineHas(ln, ll, k2, sizeof(k2) - 1);
}

// Make progress on one file. True = it sent a frame or changed state (the round is over); false = idle.
bool pumpFile(LogFwd* W, LogPos* P, const char* path, bool live, size_t room) {
  uint8_t* body = g_fwdBuf + sizeof(LogRowsHdr);
  uint32_t sz = 0;
  if (P->anchor == A_META && !P->metaLen) P->anchor = A_TS;
  if (P->anchor == A_TS && !P->tsLen) P->anchor = A_FL;
  if (P->anchor == A_FL && !P->flLen) P->anchor = A_NONE;
  if (P->anchor != A_NONE) {                                         // re-send what the leader needs to date / attribute this file's rows
    const bool meta = P->anchor == A_META, ts = P->anchor == A_TS;
    const uint32_t off = meta ? 0 : ts ? P->tsOff : P->flOff, len = meta ? P->metaLen : ts ? P->tsLen : P->flLen,
                   row = meta ? 1 : ts ? P->tsRow : P->flRow;
    if (len > room || logFwdRead(path, off, body, len, &sz) != (int)len || body[len - 1] != '\n') {
      P->anchor = meta ? A_TS : ts ? A_FL : A_NONE;                  // unreadable / too big: skip this anchor
      return true;
    }
    sendLogFrame(P->id, row, off, len, 1, true);
    return true;
  }
  const int n = logFwdRead(path, P->ackOff, body, room, &sz);
  if (n <= 0) {                                                      // caught up (or the file is gone)
    if (!live) P->finished = true;
    return false;
  }
  size_t used = 0;
  uint32_t lines = 0;
  W->inflMetaLen = W->inflTsLen = W->inflFlLen = 0;
  while (used < (size_t)n && lines < 255) {                          // whole, plausible lines only
    const uint8_t* ln = body + used;
    const uint8_t* nl = (const uint8_t*)memchr(ln, '\n', n - used);
    if (!nl) break;
    const size_t ll = (size_t)(nl - ln) + 1;
    if (ln[0] != '{' || ll < 3) break;
    if (P->ackRow == 1 && lines == 0 && !P->metaLen && ll > 13 && !memcmp(ln, "{\"evt\":\"meta\"", 13)) W->inflMetaLen = (uint32_t)ll;
    if (!P->tsLen && !W->inflTsLen && lineHasTimesync(ln, ll)) {
      W->inflTsOff = P->ackOff + (uint32_t)used; W->inflTsRow = P->ackRow + lines; W->inflTsLen = (uint32_t)ll;
    }
    if (lineHasFleetEvt(ln, ll)) {                                   // keep the LAST one in the frame
      W->inflFlOff = P->ackOff + (uint32_t)used; W->inflFlRow = P->ackRow + lines; W->inflFlLen = (uint32_t)ll;
    }
    used += ll; lines++;
  }
  if (lines == 0) {                                                  // the leading line can't go: skip it, or wait for its end
    const uint8_t* nl = (const uint8_t*)memchr(body, '\n', n);
    uint32_t ll = nl ? (uint32_t)(nl - body) + 1 : 0;
    if (!nl && (size_t)n >= room) {                                  // longer than a frame can carry: find its end
      uint8_t tmp[128];
      uint32_t pos = P->ackOff + (uint32_t)n;
      for (int k = 0; k < 16 && !ll; k++) {
        const int m = logFwdRead(path, pos, tmp, sizeof(tmp), &sz);
        if (m <= 0) break;
        const uint8_t* e = (const uint8_t*)memchr(tmp, '\n', m);
        if (e) ll = pos + (uint32_t)(e - tmp) + 1 - P->ackOff;
        pos += m;
      }
    }
    if (!ll) {                                                       // a partial tail: the writer hasn't finished it
      if (sz != P->lastSz) { P->lastSz = sz; P->torn = 0; }
      else if (++P->torn >= 4 && !live) P->finished = true;         // a rotated file that never completes it: done
      return false;
    }
    P->ackOff += ll; P->ackRow++; W->skipped++;
    Serial.printf("[FLT] client: skipped an unforwardable log line (%lu B)\n", (unsigned long)ll);
    return true;
  }
  sendLogFrame(P->id, P->ackRow, P->ackOff, (uint32_t)used, lines, false);
  return true;
}

// ---- carry-over of the previous boot's unforwarded tail (fw 0.27.0) ----
bool carryIdOk(const char* id) {
  for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)id[i]) || isupper((unsigned char)id[i])) return false;
  return id[8] == 0;
}

void carryLoad() {
  LogFwd* W = g_lf;
  W->saved.ver = CARRY_VER;
  Preferences p;
  if (!p.begin("cydscan", false)) return;
  CarryRec r;
  const bool ok = p.getBytesLength("fltcarry") == sizeof(r) && p.getBytes("fltcarry", &r, sizeof(r)) == sizeof(r);
  p.end();
  if (!ok || r.ver != CARRY_VER || r.n > CARRY_MAX) return;
  W->saved = r;
  int k = 0;
  for (int i = 0; i < r.n && k < CARRY_LOAD; i++) {
    CarryEnt c = r.e[i];
    c.path[sizeof(c.path) - 1] = 0;
    if (!carryIdOk(c.id) || strncmp(c.path, "/logs/", 6) || c.ackRow < 1) continue;
    W->carry[k++] = c;
    W->carryLoaded++;
  }
}

// Verify one carried file the first time we are joined (the SD is up) and register its position. False = SD not ready.
bool carryActivate(LogFwd* W, CarryEnt& c) {
  char path[48];
  snprintf(path, sizeof(path), "%s", c.path);
  uint32_t sz = 0;
  const int r = logFwdFind(path, sizeof(path), c.id, c.ackOff, &sz);
  if (r < 0) return false;
  if (r == 0 || sz <= c.ackOff) {                                    // deleted / cap-evicted, or everything was acked already
    Serial.printf("[FLT] client: carried-over log %s %s\n", c.id, r == 0 ? "is gone (dropped)" : "was fully forwarded");
    if (r == 0) W->carryGone++; else W->carryDone++;
    c.id[0] = 0;
    return true;
  }
  LogPos* P = findPos(W, c.id, true);
  if (!P) { c.id[0] = 0; return true; }
  snprintf(c.path, sizeof(c.path), "%s", path);                      // the live path (renamed on time sync since)
  P->ackOff = c.ackOff; P->ackRow = c.ackRow; P->metaLen = c.metaLen;
  P->tsOff = c.tsOff; P->tsRow = c.tsRow; P->tsLen = c.tsLen; P->flOff = c.flOff; P->flRow = c.flRow; P->flLen = c.flLen;
  P->anchor = P->ackOff ? A_META : A_NONE;                           // meta + timesync + fleet line first, then the tail
  Serial.printf("[FLT] client: carrying over %s (%s), %lu B unforwarded from row %lu\n", c.id, c.path,
                (unsigned long)(sz - c.ackOff), (unsigned long)c.ackRow);
  return true;
}

LogPos* findPosQuiet(LogFwd* W, const char* id) {
  for (LogPos& q : W->pos) if (q.id[0] && !strcmp(q.id, id)) return &q;
  return nullptr;
}

// ~1/s: keep NVS in step with the acked positions (this boot's files first, then still-pending carried ones).
void carryPersist() {
  LogFwd* W = g_lf;
  const uint32_t now = millis();
  if (!W || now - W->carryChkMs < CARRY_CHK_MS) return;
  W->carryChkMs = now;
  CarryRec r;
  memset(&r, 0, sizeof(r));
  r.ver = CARRY_VER;
  auto add = [&](const char* id, const char* path, const CarryEnt* own) {
    if (r.n >= CARRY_MAX || !id[0]) return;
    for (int i = 0; i < r.n; i++) if (!strcmp(r.e[i].id, id)) return;
    const LogPos* P = findPosQuiet(W, id);
    if (P && P->finished) return;
    CarryEnt& c = r.e[r.n++];
    memcpy(c.id, id, 9);
    snprintf(c.path, sizeof(c.path), "%s", path);
    c.ackRow = 1;
    if (P) {
      c.ackOff = P->ackOff; c.ackRow = P->ackRow; c.metaLen = P->metaLen; c.tsOff = P->tsOff; c.tsRow = P->tsRow;
      c.tsLen = P->tsLen; c.flOff = P->flOff; c.flRow = P->flRow; c.flLen = P->flLen;
    } else if (own) {
      const CarryEnt o = *own; memcpy(c.id, o.id, 9);
      c.ackOff = o.ackOff; c.ackRow = o.ackRow; c.metaLen = o.metaLen; c.tsOff = o.tsOff; c.tsRow = o.tsRow;
      c.tsLen = o.tsLen; c.flOff = o.flOff; c.flRow = o.flRow; c.flLen = o.flLen;
    }
  };
  FwdFile list[10];
  const int nf = logFwdList(list, 10);
  for (int k = 0; k < nf; k++) add(list[k].id, list[k].path, nullptr);
  for (const CarryEnt& c : W->carry) add(c.id, c.path, &c);
  if (!memcmp(&r, &W->saved, sizeof(r))) return;
  bool sameSet = r.n == W->saved.n;                                  // same files + paths: only progress moved
  for (int i = 0; sameSet && i < r.n; i++) sameSet = !strcmp(r.e[i].id, W->saved.e[i].id) && !strcmp(r.e[i].path, W->saved.e[i].path);
  if (sameSet && now - W->savedMs < CARRY_PUT_MS) return;           // a new / rotated / renamed file is written at once
  Preferences p;
  if (!p.begin("cydscan", false)) return;
  if (p.putBytes("fltcarry", &r, sizeof(r)) == sizeof(r)) { W->saved = r; W->savedMs = now; }
  p.end();
}

// Called from tick() while JOINED. Cheap when idle (a few SD opens per LOG_IDLE_POLL_MS).
void pumpLog() {
  LogFwd* W = g_lf;
  if (!W || !g_fwdBuf || C->st != C_JOINED || logXferActive()) return;   // (nothing below touches the SD during a transfer)
  const uint32_t now = millis();
  if (W->infl) {
    if (now - W->inflMs < W->retryMs) return;
    W->infl = false; W->resends++;                                   // no LogAck: rebuild the frame from the file and resend
    W->retryMs = W->retryMs * 2 > LOG_RESEND_MAX_MS ? LOG_RESEND_MAX_MS : W->retryMs * 2;   // the leader may be busy: back off
  } else if ((int32_t)(now - W->nextMs) < 0) {
    return;
  }
  const size_t maxFrame = g_maxPayload < FLEET_V2_MAX ? g_maxPayload : FLEET_V2_MAX;
  if (maxFrame < sizeof(LogRowsHdr) + TAG_LEN + LOG_MIN_ROOM) {      // v1 link: lines would be skipped wholesale, so don't
    if (!W->warnedV1) { W->warnedV1 = true; Serial.println("[FLT] client: ESP-NOW v1 link (250 B frames): log forwarding disabled"); }
    W->nextMs = now + 10000;
    return;
  }
  const size_t room = maxFrame - TAG_LEN - sizeof(LogRowsHdr);
  FwdFile list[10];
  const int nf = logFwdList(list, 10);
  bool did = false;
  for (CarryEnt& c : W->carry) {                                     // the previous boot's tail goes first
    if (!c.id[0] || did) continue;
    LogPos* P = findPosQuiet(W, c.id);
    if (!P) {
      if (!carryActivate(W, c)) { W->nextMs = now + 2000; return; }  // SD not ready: try again shortly
      P = c.id[0] ? findPosQuiet(W, c.id) : nullptr;
      if (!P) continue;
    }
    P->stamp = ++W->clock;
    did = pumpFile(W, P, c.path, false, room);
    if (P->finished) { Serial.printf("[FLT] client: carried-over log %s forwarded\n", c.id); W->carryDone++; c.id[0] = 0; }
  }
  for (int k = 0; k < nf && !did; k++) {                             // live streams first, then this boot's rotated files
    LogPos* P = findPos(W, list[k].id, true);
    if (!P || P->finished) continue;
    did = pumpFile(W, P, list[k].path, list[k].live, room);
  }
  if (!did) W->nextMs = now + LOG_IDLE_POLL_MS;
}

void onLogAck(const Rx& f) {
  LogFwd* W = g_lf;
  if (!W || C->st != C_JOINED || !sessionOk(f, sizeof(LogAck))) return;
  LogAck a; memcpy(&a, f.data, sizeof(a));
  uint8_t id[4];
  if (!W->infl || !idToBytes(W->inflId, id) || memcmp(id, a.fileId, 4) || a.nextByteOff != W->inflOff + W->inflLen) return;
  if (a.throughRow != W->inflRow + W->inflLines - 1) return;
  LogPos* P = findPos(W, W->inflId, false);
  const uint32_t now = millis();
  W->infl = false;
  W->retryMs = LOG_RESEND_MS;
  if (!P) return;
  if (W->inflAnchor) {
    W->anchorsSent++;
    P->anchor = P->anchor == A_META ? A_TS : P->anchor == A_TS ? A_FL : A_NONE;
    W->nextMs = now;
    return;
  }
  if (W->inflMetaLen) P->metaLen = W->inflMetaLen;                   // remember the anchors for the next join (same boot)
  if (W->inflTsLen && !P->tsLen) { P->tsOff = W->inflTsOff; P->tsRow = W->inflTsRow; P->tsLen = W->inflTsLen; }
  if (W->inflFlLen) { P->flOff = W->inflFlOff; P->flRow = W->inflFlRow; P->flLen = W->inflFlLen; }
  P->ackOff += W->inflLen; P->ackRow += W->inflLines; P->torn = 0;
  W->rowsFwd += W->inflLines; W->frames++;
  W->nextMs = now + W->inflLen * 1000UL / LOG_BYTES_PER_S;           // ~8 KB/s ceiling
}

const char* stName(CState s) { return s == C_JOINED ? "joined" : s == C_JOINING ? "joining" : s == C_HELLO ? "hello sent" : "searching"; }

}  // namespace

// ------------------------------------------------------------------ role hooks (fleet_internal.h)
namespace fleet { namespace detail { namespace client {

void begin() {
  if (!C) C = new (std::nothrow) ClientSt();
  if (!C) { Serial.println("[FLT] client: out of memory"); return; }
  memset(C, 0, sizeof(*C));
  C->st = C_SEARCH;
  if (!g_sent) g_sent = (SentRow*)calloc(MAX_DET, sizeof(SentRow));
  if (!g_fwdBuf) g_fwdBuf = (uint8_t*)malloc(FLEET_V2_MAX);
  if (!g_sent || !g_fwdBuf) Serial.println("[FLT] client: no memory for forwarding (rows stay local)");
  if (!g_lf) g_lf = (LogFwd*)calloc(1, sizeof(LogFwd));
  if (g_lf) { g_lf->retryMs = LOG_RESEND_MS; carryLoad(); }
  else Serial.println("[FLT] client: no memory for log forwarding");
  buildHops(false);                                                // the boot MAC is already fresh: no remac needed
}

void onFrame(const Rx& f) {
  if (!C || f.data[0] != FLEET_PROTO_VERSION) return;
  switch ((Msg)f.data[1]) {
    case Msg::Probe: onProbe(f); break;
    case Msg::Join:  onJoin(f); break;
    case Msg::Sync:  onSync(f); break;
    case Msg::Ctrl:  onCtrl(f); break;
    case Msg::Plan:  onPlan(f); break;
    case Msg::LogAck: onLogAck(f); break;
    default: break;
  }
}

void tick() {
  if (!C) return;
  const uint32_t now = millis();
  carryPersist();
  if (C->pendingCh && !webshare::active()) {                       // a channel move deferred while the AP was up
    uint8_t ch = C->pendingCh;
    C->pendingCh = 0;
    fleet::setChannel(ch);
    if (C->st == C_SEARCH) buildHops(false);
  }
  switch (C->st) {
    case C_SEARCH:
      if (webshare::active()) { C->hopStartMs = now; break; }      // the AP pins the radio: listen on the fleet channel only
      if (C->nHop > 1 && now - C->hopStartMs >= DWELL_MS) {
        if (C->hopIdx + 1 >= C->nHop) { buildHops(!C->fullSweep); break; }   // pass done: alternate preferred / full
        C->hopIdx++;
        C->hopStartMs = now;
        setRadioChannel(C->hop[C->hopIdx]);
      }
      break;
    case C_HELLO:
      if (now - C->helloMs > JOIN_WAIT_MS) toSearch(nullptr, false);   // no Join: answer the next probe (same MAC)
      break;
    case C_JOINING:
      if (C->acks < ACK_TRIES) { if (now - C->lastAckMs >= ACK_EVERY_MS) sendAck(); }
      else if (now - C->lastAckMs > ACK_GIVEUP_MS) toSearch("no Sync after JoinAck", true);
      break;
    case C_JOINED:
      if (now - C->lastSyncMs > SYNC_LOST_MS) { clientEvent("lost"); toSearch("leader lost", true); }
      else {
        if (!C->lastMStatMs || now - C->lastMStatMs >= MSTAT_MS) sendMStat();
        pumpLog();                                                 // P6 item 6: after MStat, so heartbeats never wait on the SD
      }
      break;
  }
}

void fillUi(Ui& u) {
  if (!C) return;
  u.joined = C->st == C_JOINED;
  u.leaderSeen = u.joined;
  if (u.joined) {
    memcpy(u.leaderMac, C->leaderMac, 6);
    memcpy(u.leaderName, C->leaderName, sizeof(u.leaderName));
    u.slot = C->slot; u.nMembers = C->nMembers;
    u.rssi = C->rssi;
    u.ageMs = millis() - C->lastSyncMs;
  } else {
    u.channel = C->hop[C->hopIdx];                                 // the channel being searched right now
  }
}

void printStatus() {
  if (!C) return;
  char ms[18]; macStr(C->leaderMac, ms);
  Serial.printf("[FLT] client: %s", stName(C->st));
  if (C->st == C_JOINED)
    Serial.printf("  leader %s (%s) slot %u/%u rssi %d sync %lus ago", C->leaderName, ms, (unsigned)C->slot,
                  (unsigned)C->nMembers, (int)C->rssi, (unsigned long)((millis() - C->lastSyncMs) / 1000));
  else
    Serial.printf("  on ch %u", (unsigned)C->hop[C->hopIdx]);
  Serial.println();
  PosFix f = posSnapshot();
  Serial.printf("[FLT] client: time src %d epoch %lu tz %d | pos src %d%s age %lus | fleet time %s pos %s | self MACs %u\n",
                timeSource(), (unsigned long)epochLocal(), phone::tzOffsetMin(), (int)f.src, f.live ? " live" : "",
                (unsigned long)(f.ageMs / 1000), C->haveTime ? (timeFresh() ? "fresh" : "stale") : "none",
                C->havePos ? "yes" : "none", (unsigned)C->nSelf);
  Serial.printf("[FLT] client: forwarded %lu rows in %lu frames, %lu RID frames\n", (unsigned long)C->rowsSent,
                (unsigned long)C->framesSent, (unsigned long)C->ridFrames);
  if (g_lf) {                                                      // P6 item 6: log store-and-forward
    Serial.printf("[FLT] client: log fwd %lu lines in %lu frames, %lu resends, %lu skipped, %lu anchors%s\n",
                  (unsigned long)g_lf->rowsFwd, (unsigned long)g_lf->frames, (unsigned long)g_lf->resends,
                  (unsigned long)g_lf->skipped, (unsigned long)g_lf->anchorsSent, g_lf->infl ? " (frame in flight)" : "");
    Serial.printf("[FLT] client: carry-over (NVS fltcarry, %u file(s) saved): loaded %lu, forwarded %lu, gone %lu\n",
                  (unsigned)g_lf->saved.n, (unsigned long)g_lf->carryLoaded, (unsigned long)g_lf->carryDone,
                  (unsigned long)g_lf->carryGone);
    for (const CarryEnt& c : g_lf->carry) {
      if (!c.id[0]) continue;
      const LogPos* P = findPosQuiet(g_lf, c.id);
      Serial.printf("[FLT]   carried %s %s %s from row %lu off %lu\n", c.id, c.path, P ? "draining" : "pending verify",
                    (unsigned long)(P ? P->ackRow : c.ackRow), (unsigned long)(P ? P->ackOff : c.ackOff));
    }
    FwdFile list[10];
    const int nf = logFwdList(list, 10);
    for (const LogPos& q : g_lf->pos) {
      if (!q.id[0]) continue;
      uint32_t sz = 0; uint8_t b;
      bool live = false;
      for (int k = 0; k < nf; k++) if (!strcmp(list[k].id, q.id)) { logFwdRead(list[k].path, 0, &b, 0, &sz); live = list[k].live; }
      Serial.printf("[FLT]   file %s%s acked row %lu off %lu, backlog %lu B%s\n", q.id, live ? " (live)" : "",
                    (unsigned long)(q.ackRow - 1), (unsigned long)q.ackOff,
                    (unsigned long)(sz > q.ackOff && !q.finished ? sz - q.ackOff : 0), q.finished ? " (done)" : "");
    }
  }
}

bool joined() { return C && C->st == C_JOINED; }

// P5 (docs §9.1): after each successful scan cycle, forward the rows that changed (or are due a refresh) as sealed
// DetBatch frames, and the Remote-IDs as a RidBatch. Raw Detections: the leader re-scores with its own DB.
void forwardCycle() {
  if (!C || C->st != C_JOINED || !g_sent || !g_fwdBuf) return;
  const uint32_t now = millis();
  const int per = g_maxPayload >= sizeof(DetBatchHdr) + DETS_PER_V2 * sizeof(Detection) + TAG_LEN ? DETS_PER_V2 : DETS_PER_V1;
  C->fwdCycle++;
  int frames = 0, cnt = 0;
  auto flush = [&]() {
    if (!cnt) return;
    DetBatchHdr* h = (DetBatchHdr*)g_fwdBuf;
    h->cycle = C->fwdCycle; h->idx = (uint8_t)frames; h->cnt = (uint8_t)cnt;
    if (frames) delay(5);
    if (sendSealed(C->leaderMac, g_fwdBuf, sizeof(DetBatchHdr) + cnt * sizeof(Detection), Msg::DetBatch, C->sess,
                   C->txSeq, C->sk)) { C->rowsSent += cnt; C->framesSent++; }
    frames++; cnt = 0;
  };
  memset(g_fwdBuf, 0, sizeof(DetBatchHdr));
  C->lastDets = (uint16_t)g_detCount;
  // Start where the last capped cycle stopped, so a few jittery rows near the top can't starve the rest of their
  // 10 s refresh (REV-003). The cap only bites on a v1 link or a large table.
  const int n = g_detCount;
  const int start = n ? C->fwdStart % n : 0;
  int i = 0;
  for (; i < n && frames < FWD_MAX_FRAMES; i++) {
    const int r = (start + i) % n;
    if (!(g_detVia[r] & 1)) continue;                                // only what this unit's own C5 heard
    if (!needsSend(g_dets[r], now)) continue;
    memcpy(g_fwdBuf + sizeof(DetBatchHdr) + cnt * sizeof(Detection), &g_dets[r], sizeof(Detection));
    if (++cnt == per) flush();
  }
  C->fwdStart = (uint16_t)(n ? (start + i) % n : 0);
  if (frames < FWD_MAX_FRAMES) flush();
  int nr = 0;                                                        // Remote-IDs: every cycle while present
  const int rper = per == DETS_PER_V2 ? RIDS_PER_V2 : RIDS_PER_V1;
  for (int i = 0; i < g_ridStoreCount && nr < rper; i++) {
    if (!(g_ridVia[i] & 1)) continue;
    memcpy(g_fwdBuf + sizeof(RidBatchHdr) + nr * sizeof(RemoteId), &g_rids[i], sizeof(RemoteId));
    nr++;
  }
  if (nr) {
    RidBatchHdr* rh = (RidBatchHdr*)g_fwdBuf;
    rh->cnt = (uint8_t)nr; rh->rsv = 0;
    delay(5);
    if (sendSealed(C->leaderMac, g_fwdBuf, sizeof(RidBatchHdr) + nr * sizeof(RemoteId), Msg::RidBatch, C->sess,
                   C->txSeq, C->sk)) C->ridFrames++;
  }
}

void fwdStats(FwdStats& s) {
  if (!C) return;
  s.rowsSent = C->rowsSent; s.framesSent = C->framesSent; s.ridFrames = C->ridFrames;
}

link_protocol::ScanPlan plan(uint8_t* role) {
  if (!C || C->st != C_JOINED) { *role = PLANROLE_FULL; return link_protocol::ScanPlan{}; }
  *role = C->planRole;
  return C->plan;
}

bool timeFix(uint32_t* utc, uint32_t* atMs) {
  if (!C || !C->haveTime) return false;
  *utc = C->fltUtc; *atMs = C->fltUtcAtMs;
  return true;
}
bool timeFresh() { return C && C->haveTime && C->st == C_JOINED && millis() - C->fltUtcAtMs < TIME_FRESH_MS; }
// The fleet position is live exactly while the LEADER calls it live and its Syncs are current, so every unit agrees
// on when the shared track is live vs held. Held (the leader's fix aged out, or the leader is gone) still returns
// the last fleet fix with its true age for the held fallback.
bool posFix(float* lat, float* lon, uint32_t* ageMs, bool* live) {
  if (!C || !C->havePos) return false;
  *lat = C->fltLat; *lon = C->fltLon;
  const uint32_t since = millis() - C->fltFixAtMs;
  *ageMs = C->fltFixAgeRx > UINT32_MAX - since ? UINT32_MAX : C->fltFixAgeRx + since;   // the leader's age + ours
  *live = C->fltPosLive && C->st == C_JOINED && millis() - C->lastSyncMs < SYNC_LOST_MS;
  return true;
}
bool isSelf(const uint8_t mac[6]) {
  if (!C) return false;
  for (int i = 0; i < C->nSelf; i++) if (!memcmp(C->selfMacs[i], mac, 6)) return true;
  return false;
}
void dropSession(const char* why) { if (C && C->st != C_SEARCH) toSearch(why, true); }
void forgetKey() { if (C) { C->fkOk = false; memset(C->fk, 0, sizeof(C->fk)); } }
bool gpsIsFleetPos() { return C && C->st == C_JOINED && C->slot && C->fixSlot == C->slot; }
bool ident(uint8_t id[8], uint8_t leaderSid[6]) {
  if (!C || C->st != C_JOINED || !C->haveIdent) return false;
  memcpy(id, C->fleetId, 8); memcpy(leaderSid, C->leaderSid, 6);
  return true;
}

}}}  // namespace fleet::detail::client

namespace fleet {
void dbgPrintFk() {
#ifdef FLEET_DEBUG
  uint8_t fk[KEY_LEN];
  if (!ownFleetKey(fk)) { Serial.println("[FLT] fleet key unavailable"); return; }
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", fk[i]);
  memset(fk, 0, sizeof(fk));
  Serial.printf("[FLT] bench enrollment: on the leader run  fleet add %s %s %s\n", devkey::sid(), hex, devkey::unitName());
#endif
}
}  // namespace fleet

// fleet_client.cpp — see fleet.h and docs/fleet-mode.md §1.5, §5.3-§5.4. Client state machine:
//   SEARCH  listen for a Probe (dwell 9 s on the last channel, then 1 / 6 / 11; after a fruitless pass, all of 1-13),
//           answer at most one per 2 s. While this unit's Wi-Fi download AP is up the radio stays on the fleet channel.
//   HELLO   Hello sent (tag over the probe nonce, our nonce and both MACs, keyed by this unit's fleet key FK)
//   JOINING Join verified (its proof authenticates the leader), session key derived, JoinAck resent every 300 ms
//   JOINED  first tagged Sync arrived; MStat every 2 s; leader lost after 15 s without a Sync
// After a session ends the next Hello goes out from a fresh random MAC (docs §16.4).
#include <Arduino.h>
#include <new>
#include "fleet.h"
#include "fleet_internal.h"
#include "devkey.h"
#include "host_board.h"
#include "webshare.h"
#include "phone.h"
#include "host_core.h"

using namespace fleet::detail;

namespace {

constexpr uint32_t DWELL_MS = 9000, HELLO_GAP_MS = 2000, JOIN_WAIT_MS = 5000;
constexpr uint32_t ACK_EVERY_MS = 300, ACK_GIVEUP_MS = 1000, SYNC_LOST_MS = 15000, MSTAT_MS = 2000;
constexpr uint8_t  ACK_TRIES = 5;
constexpr uint32_t TIME_FRESH_MS = 15000;   // a joined client trusts the leader's clock this long after a Sync
constexpr uint32_t MIN_UTC = 1700000000UL;  // anything earlier (~2023) is not a real clock

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
};
ClientSt* C = nullptr;

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
  sendAck();
}

bool sessionOk(const Rx& f, size_t structLen) {
  if ((C->st != C_JOINING && C->st != C_JOINED) || memcmp(f.mac, C->leaderMac, 6) || f.len != structLen + TAG_LEN) return false;
  const Hdr* h = (const Hdr*)f.data;
  if (h->sess != C->sess || !tagOk(f.data, f.len, C->sk)) return false;
  return C->rx.accept(h->seq);
}

void onSync(const Rx& f) {
  if (!sessionOk(f, sizeof(Sync))) return;
  Sync s; memcpy(&s, f.data, sizeof(s));
  memcpy(C->leaderName, s.leaderName, sizeof(s.leaderName)); C->leaderName[20] = 0;
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
    event("join");
    C->lastMStatMs = 0;
  }
}

void onCtrl(const Rx& f) {
  if (C->st != C_JOINED || !sessionOk(f, sizeof(Ctrl))) return;
  Ctrl c; memcpy(&c, f.data, sizeof(c));
  C->lastSyncMs = millis();
  switch (c.op) {
    case CTRL_RELEASE:                                             // leave fleet mode (a CYD returns to its phone link)
      Serial.println("[FLT] client: released by the leader -- restarting in normal mode");
      event("release");
      fleet::setRole(fleet::ROLE_OFF);
      Serial.flush();
      delay(300);
      ESP.restart();
      break;
    case CTRL_UNJOIN:
      event("unjoin");
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

const char* stName(CState s) { return s == C_JOINED ? "joined" : s == C_JOINING ? "joining" : s == C_HELLO ? "hello sent" : "searching"; }

}  // namespace

// ------------------------------------------------------------------ role hooks (fleet_internal.h)
namespace fleet { namespace detail { namespace client {

void begin() {
  if (!C) C = new (std::nothrow) ClientSt();
  if (!C) { Serial.println("[FLT] client: out of memory"); return; }
  memset(C, 0, sizeof(*C));
  C->st = C_SEARCH;
  buildHops(false);                                                // the boot MAC is already fresh: no remac needed
}

void onFrame(const Rx& f) {
  if (!C || f.data[0] != FLEET_PROTO_VERSION) return;
  switch ((Msg)f.data[1]) {
    case Msg::Probe: onProbe(f); break;
    case Msg::Join:  onJoin(f); break;
    case Msg::Sync:  onSync(f); break;
    case Msg::Ctrl:  onCtrl(f); break;
    default: break;
  }
}

void tick() {
  if (!C) return;
  const uint32_t now = millis();
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
      if (now - C->lastSyncMs > SYNC_LOST_MS) { event("lost"); toSearch("leader lost", true); }
      else if (!C->lastMStatMs || now - C->lastMStatMs >= MSTAT_MS) sendMStat();
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
}

bool joined() { return C && C->st == C_JOINED; }

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

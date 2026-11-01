// fleet_leader.cpp — see fleet.h and docs/fleet-mode.md §5, §17. The member store (NVS cydscan/fltmem: sid, key gen,
// name and the member's FLEET key; never its phone key) + the enrollment counter (fltactr), the Probe schedule, Hello
// matching, Join, one encrypted + tagged session per member, the Sync heartbeat, loss detection, and the phone `M:`
// commands (which also run on a unit that isn't leading: `M:j`, `M:L1`, `M:l`).
#include <Arduino.h>
#include <Preferences.h>
#include <new>
#include <stdarg.h>
#include <initializer_list>
#include "fleet.h"
#include "fleet_internal.h"
#include "devkey.h"
#include "host_board.h"
#include "webshare.h"
#include "phone.h"
#include "host_core.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_memory_utils.h>

using namespace fleet::detail;
static_assert(DET_MEMBER_SLOTS == fleet_proto::FLEET_MAX_MEMBERS, "g_detFleetRssi needs one slot per member");

namespace {

constexpr uint32_t PROBE_FAST_MS = 3000, PROBE_SLOW_MS = 15000, PROBE_BURST_MS = 120000;   // docs §6
constexpr uint32_t NONCE_LIFE_MS = 10000;   // a Hello may answer either of the last 2 probes for 10 s
constexpr uint32_t JOIN_TIMEOUT_MS = 5000, ENCRYPT_FALLBACK_MS = 500, SENT_WAIT_MS = 200;
constexpr uint32_t SYNC_MS = 2000, LOST_MS = 10000;
constexpr uint32_t REMAC_IDLE_MS = 15UL * 60 * 1000;   // nobody joined: rotate the leader MAC this often (tracking)
constexpr int      HELLOS_PER_S = 10;
constexpr uint32_t MEMBER_FIX_LIVE_MS = 10000;   // a member's own GPS fix counts as live this long
// P4 coverage split (docs §8.3-§8.4, §16.1): a member counts toward the role layout once it has been joined this long
// (hysteresis against flapping), and keeps its role this long after it is lost (a quick reboot rejoins without a
// reshuffle). Plans are re-sent every PLAN_RESEND_MS so a member that missed one converges.
constexpr uint32_t ROLE_STABLE_MS = 10000, ROLE_GRACE_MS = 30000, PLAN_RESEND_MS = 10000;
constexpr uint8_t  PLANMODE_AUTO = 0, PLANMODE_OFF = 3;
// P5 (docs §9.2): members' forwarded rows live this long (= the C5 table TTL); member alert counts this long.
// Rows are refreshed by members every 10 s, so 20 s survives one lost refresh and clears departed devices sooner.
constexpr uint32_t REMOTE_TTL_MS = 20000, MEMBER_STAT_FRESH_MS = 10000;
constexpr int      REMOTE_ROWS = 768, REMOTE_ROWS_NOPSRAM = 64, REMOTE_RIDS = 16;   // 768 x ~88 B = 66 KB PSRAM

struct RemoteRow { Detection d; uint8_t member; bool used; uint32_t lastMs; };   // ~88 B; PSRAM
struct RemoteRid { RemoteId r; uint8_t member; bool used; uint32_t lastMs; };

struct MemberRec { uint8_t sid[6]; uint32_t kg; char name[21]; uint8_t fk[KEY_LEN]; };   // persisted
struct Store { uint8_t n; MemberRec m[FLEET_MAX_MEMBERS]; };

enum MState : uint8_t { M_ENROLLED = 0, M_JOINING = 1, M_JOINED = 2, M_LOST = 3 };   // FLT= state column
struct MemberRt {
  uint8_t  state;
  bool     enc;                        // the ESP-NOW peer is encrypted with LMK = SK
  uint8_t  mac[6];                     // current radio MAC (learned from its Hello; never stored)
  uint16_t sess;
  uint8_t  sk[KEY_LEN];
  uint8_t  nl[NONCE_LEN], nc[NONCE_LEN];
  uint32_t txSeq;
  SeqWin   rx;
  uint32_t joinSentMs, lastRxMs, lastSyncMs;
  int8_t   rssi;
  uint8_t  board, caps;
  char     fw[13];
  uint16_t heapKB, txFailPm;
  uint8_t  bleMac[6];                  // its BLE advert MAC (JoinAck; zeros for a CYD client): a fleet "self" MAC
  float    fixLat, fixLon;             // its OWN live GPS fix from MStat (P3: fleet position fallback)
  uint32_t fixAtMs;                    // 0 = never
  uint8_t  phoneMac[6];                // its own connected phone (MStat), a fleet self MAC
  uint32_t joinedAtMs, lostAtMs;       // P4 role hysteresis / grace
  uint8_t  role;                       // assigned scan role (PlanRole)
  uint8_t  planSentGen;                // last plan generation sent to it
  uint32_t lastPlanMs;
  uint8_t  srcMask;                    // its enabled scan sources (MStat); 0 = not reported yet
  uint8_t  roleOverride;               // M:o: 0 = auto (the template), else PlanRole + 1 (runtime only, not saved)
  uint16_t deauth; uint8_t evil, rid;  // its latest alert counts (MStat), for the fleet-wide maxima
  uint32_t statAtMs;
};

struct Leader {
  Store    st;
  uint32_t addCtr;                     // next valid enrollment counter (NVS fltactr): every M:a binds it -> no replay
  MemberRt rt[FLEET_MAX_MEMBERS];
  uint8_t  sid[6];
  uint8_t  fleetId[8];                 // P6: random per leader boot (first begin()), sent in every Sync -> log fleet_id
  uint8_t  probeNl[2][NONCE_LEN];
  uint32_t probeMs[2];
  uint8_t  probeIdx;
  bool     probing;
  uint32_t lastProbeMs, burstStartMs;
  uint32_t helloWinMs;
  int      helloCount;
  uint8_t  chPending;
  uint32_t chSwitchAtMs;
  uint32_t lastRemacMs;
  int8_t   fixMember;                  // member whose GPS is the fleet position fallback (sticky; -1 = none)
  int8_t   fixUsed;                    // the member memberFix() last returned (live or held), -1 = none
  uint8_t  planGen;                    // bumped whenever the role layout changes
  uint8_t  leaderRole;                 // this leader's own scan role
  uint8_t  planMode;                   // PLANMODE_AUTO (template) / PLANMODE_OFF (everyone full); NVS fltpmode
  // P5 remote table (heap: PSRAM when present)
  RemoteRow* remote;
  int        nRemote;                  // capacity
  RemoteRid  rids[REMOTE_RIDS];
  uint32_t   rowsRx, framesRx, merged, appended;
};
Leader* L = nullptr;   // heap, created by the first use (normal-mode static footprint stays one pointer)

bool loadStore() {
  if (L) return true;
  L = new (std::nothrow) Leader();
  if (!L) return false;
  memset(L, 0, sizeof(*L));
  sidFromHex(devkey::sid(), L->sid);
  Preferences p;
  if (p.begin("cydscan", true)) {
    if (p.getBytesLength("fltmem") == sizeof(Store)) p.getBytes("fltmem", &L->st, sizeof(Store));
    L->addCtr = p.getULong("fltactr", 1);
    L->planMode = p.getUChar("fltpmode", PLANMODE_AUTO);
    p.end();
  }
  if (!L->addCtr) L->addCtr = 1;
  if (L->st.n > FLEET_MAX_MEMBERS) L->st.n = 0;
  for (int i = 0; i < L->st.n; i++) L->st.m[i].name[20] = 0;
  return true;
}
// Persist a candidate store (+ counter) BEFORE it becomes the live one, so RAM and NVS never disagree.
bool saveStore(const Store& st, uint32_t ctr) {
  Preferences p;
  if (!p.begin("cydscan", false)) return false;
  bool ok = p.putBytes("fltmem", &st, sizeof(Store)) == sizeof(Store) && p.putULong("fltactr", ctr) == sizeof(uint32_t);
  p.end();
  return ok;
}

int findSid(const uint8_t sid[6]) {
  for (int i = 0; i < L->st.n; i++) if (!memcmp(L->st.m[i].sid, sid, 6)) return i;
  return -1;
}
int findMac(const uint8_t* mac) {
  for (int i = 0; i < L->st.n; i++)
    if ((L->rt[i].state == M_JOINING || L->rt[i].state == M_JOINED) && !memcmp(L->rt[i].mac, mac, 6)) return i;
  return -1;
}

void restartProbing() { L->probing = false; }   // the next tick starts a fresh 2-minute fast burst

// A fleet event with the P6 extras: member `m` in slot i (or none) as the peer + the fleet size now.
void leaderEventRec(const char* op, const MemberRec* m, int i) {
  if (!L) { event(op); return; }
  fleet::EventInfo x = {};
  x.slot = -1;
  if (m) {
    for (int k = 0; k < 6; k++) snprintf(x.peer + 2 * k, 3, "%02x", m->sid[k]);
    strncpy(x.peerName, m->name, sizeof(x.peerName) - 1);
    x.slot = (int8_t)(i + 1);
  }
  int units = 1;
  for (int k = 0; k < L->st.n; k++) if (L->rt[k].state == M_JOINED) units++;
  x.n = (int8_t)units;
  event(op, x);
}
void leaderEvent(const char* op, int i) { leaderEventRec(op, L && i >= 0 && i < L->st.n ? &L->st.m[i] : nullptr, i); }

void endSession(int i, uint8_t newState) {
  MemberRt& r = L->rt[i];
  if (r.state == M_JOINED && newState == M_LOST) r.lostAtMs = millis();
  if (r.state == M_JOINING || r.state == M_JOINED) peerDel(r.mac);
  memset(r.sk, 0, sizeof(r.sk));
  r.state = newState;
  r.enc = false;
  restartProbing();
}

// Burst send then peer change: clearSent() BEFORE queuing the frames (Syncs to the same member leave completions behind
// all the time), then waitSent() after: it returns once a frame of the burst has left the radio, plus a short grace
// for the rest of the burst, so the following peer change / channel switch can't hit frames still in the queue.
void clearSent(const uint8_t* mac) { sendDone(mac); }
void waitSent(const uint8_t* mac) {
  const uint32_t t0 = millis();
  while (millis() - t0 < SENT_WAIT_MS && !sendDone(mac)) delay(2);
  delay(30);
}

void sendProbe() {
  Probe p = {};
  p.h.ver = FLEET_PROTO_VERSION; p.h.type = (uint8_t)Msg::Probe;
  L->probeIdx ^= 1;
  randomBytes(L->probeNl[L->probeIdx], NONCE_LEN);
  L->probeMs[L->probeIdx] = millis();
  memcpy(p.nl, L->probeNl[L->probeIdx], NONCE_LEN);
  p.ch = g_ch;
  uint8_t bc[6]; memset(bc, 0xFF, 6);
  sendClear(bc, &p, sizeof(p));
}

bool macSet(const uint8_t* m) { for (int k = 0; k < 6; k++) if (m[k]) return true; return false; }
void addSelf(Sync* s, const uint8_t* m) {
  if (s->nSelf >= 9 || !macSet(m)) return;
  for (int k = 0; k < s->nSelf; k++) if (!memcmp(s->selfMacs[k], m, 6)) return;
  memcpy(s->selfMacs[s->nSelf++], m, 6);
}

// Sync (every 2 s per member): the fleet clock (UTC + tz) and the ONE fleet position every unit stamps (docs §16.6:
// the leader's own choice -- phone by default, its GPS, else a member's live GPS), plus the fleet's own radio MACs.
void sendSync(int i) {
  MemberRt& r = L->rt[i];
  uint8_t buf[sizeof(Sync) + TAG_LEN];
  Sync* s = (Sync*)buf;
  memset(buf, 0, sizeof(buf));
  s->ch = g_ch;
  s->tzOffMin = (int16_t)phone::tzOffsetMin();
  s->tsrc = (uint8_t)timeSource();
  if (timeHasSync()) {
    s->utc = epochLocal() + (uint32_t)(phone::tzOffsetMin() * 60);   // epochLocal is local: back to UTC
    if (s->utc >= 1700000000UL) s->flags |= 0x01;                    // the same bar the client applies
  }
  if (phone::tzKnown()) s->flags |= 0x08;                            // never push a default 0 tz over a client's real one
  PosFix f = posSnapshot();
  s->psrc = f.src;
  if (f.src) {
    s->lat = (int32_t)lroundf(f.lat * 1e7f); s->lon = (int32_t)lroundf(f.lon * 1e7f);
    s->fixAgeMs = f.ageMs;
    s->flags |= 0x02 | (f.live ? 0x04 : 0);
    if (f.from == 4 && f.live && L->fixUsed >= 0) s->fixSlot = (uint8_t)(L->fixUsed + 1);   // a member's GPS is the fleet position
  }
  uint8_t m[6];
  if (phone::ownMac(m))  addSelf(s, m);                            // this leader's BLE advert
  if (phone::peerMac(m)) addSelf(s, m);                            // the phone connected to it
  if (webshare::active()) { WiFi.softAPmacAddress(m); addSelf(s, m); }   // its download AP
  for (int k = 0; k < L->st.n; k++)
    if (L->rt[k].state == M_JOINED) { addSelf(s, L->rt[k].bleMac); addSelf(s, L->rt[k].phoneMac); }
  memcpy(s->leaderSid, L->sid, 6);
  strncpy(s->leaderName, devkey::unitName(), sizeof(s->leaderName) - 1);
  memcpy(s->fleetId, L->fleetId, sizeof(s->fleetId));
  sendSealed(r.mac, buf, sizeof(Sync), Msg::Sync, r.sess, r.txSeq, r.sk);
  r.lastSyncMs = millis();
}

void sendCtrl(int i, uint8_t op, uint8_t arg) {
  MemberRt& r = L->rt[i];
  uint8_t buf[sizeof(Ctrl) + TAG_LEN];
  Ctrl* c = (Ctrl*)buf;
  memset(buf, 0, sizeof(buf));
  c->op = op; c->arg[0] = arg;
  sendSealed(r.mac, buf, sizeof(Ctrl), Msg::Ctrl, r.sess, r.txSeq, r.sk);
}

// A Hello's tag matched member i: answer with Join (cleartext, proof of the shared FK) and derive the session key.
// Nothing changes unless every HMAC succeeded.
void startJoin(int i, const uint8_t* mac, const Hello& h) {
  MemberRt& r = L->rt[i];
  const MemberRec& m = L->st.m[i];
  uint16_t sess;
  do { randomBytes(&sess, sizeof(sess)); } while (sess == 0);
  Join j = {};
  j.h.ver = FLEET_PROTO_VERSION; j.h.type = (uint8_t)Msg::Join; j.h.sess = sess;
  memcpy(j.nl, h.nl, NONCE_LEN); memcpy(j.nc, h.nc, NONCE_LEN);
  j.slot = (uint8_t)(i + 1);
  j.nMembers = L->st.n;
  j.maxPayload = g_maxPayload;
  uint8_t proof[32], sk[32];
  bool ok = Hmac(m.fk, KEY_LEN).add("CFLO-FLT-JOIN-1").add(h.nl, NONCE_LEN).add(h.nc, NONCE_LEN).add16(sess)
                .add8(j.slot).add8(j.nMembers).finish(proof);
  ok = ok && Hmac(m.fk, KEY_LEN).add("CFLO-FLT-SK-1").add(h.nl, NONCE_LEN).add(h.nc, NONCE_LEN).add16(sess)
                .add(g_mac, 6).add(mac, 6).finish(sk);
  if (!ok) { memset(sk, 0, sizeof(sk)); Serial.println("[FLT] Join: crypto error, ignored"); return; }
  if (r.state == M_JOINED) r.lostAtMs = millis();                 // re-joining without a loss mark (quick reboot):
                                                                   // the role grace applies -- and caps it at 30 s
  if (r.state == M_JOINING || r.state == M_JOINED) peerDel(r.mac);   // a rejoin replaces the old session
  memcpy(j.proof, proof, sizeof(j.proof));
  memcpy(r.sk, sk, KEY_LEN);
  memset(sk, 0, sizeof(sk));
  memcpy(r.mac, mac, 6);
  memcpy(r.nl, h.nl, NONCE_LEN); memcpy(r.nc, h.nc, NONCE_LEN);
  r.sess = sess; r.txSeq = 0; r.rx.reset(); r.enc = false;
  r.state = M_JOINING;
  r.joinSentMs = millis();
  sendDone(mac);                                                   // forget any stale completion for this MAC
  if (!peerSet(mac, nullptr) || !sendClear(mac, &j, sizeof(j))) { endSession(i, M_ENROLLED); return; }
  char ms[18]; macStr(mac, ms);
  Serial.printf("[FLT] Hello matched member %d (%s) from %s: Join sent, sess %04x\n", i + 1, m.name, ms, sess);
}

void onHello(const Rx& f) {
  if (f.len != sizeof(Hello)) return;
  const uint32_t now = millis();
  Hello h; memcpy(&h, f.data, sizeof(h));
  int k = -1;                                                      // which live probe it answers (cheap check first)
  for (int q = 0; q < 2; q++)
    if (L->probeMs[q] && now - L->probeMs[q] < NONCE_LIFE_MS && !memcmp(h.nl, L->probeNl[q], NONCE_LEN)) k = q;
  if (k < 0) return;
  if (now - L->helloWinMs >= 1000) { L->helloWinMs = now; L->helloCount = 0; }
  if (++L->helloCount > HELLOS_PER_S) return;                     // bound the HMAC work a flood of fresh-nonce Hellos causes
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    // A member with a session only rejoins via a probe sent AFTER that session began: a replayed older Hello
    // (or a second Hello racing a lost Join) can't tear the session down.
    if ((r.state == M_JOINED || r.state == M_JOINING) && (int32_t)(L->probeMs[k] - r.joinSentMs) <= 0) continue;
    uint8_t t[32];
    if (!Hmac(L->st.m[i].fk, KEY_LEN).add("CFLO-FLT-HELLO-1").add(h.nl, NONCE_LEN).add(h.nc, NONCE_LEN)
             .add(g_mac, 6).add(f.mac, 6).finish(t)) return;
    if (ctEqual(t, h.tag, sizeof(h.tag))) { startJoin(i, f.mac, h); return; }   // identifies the sid off-air
  }
}

// Shared checks for a session frame from member i: exact length, session id, tag, then the replay window.
bool sessionOk(int i, const Rx& f, size_t structLen) {
  MemberRt& r = L->rt[i];
  if (!r.enc || f.len != structLen + TAG_LEN) return false;
  const Hdr* h = (const Hdr*)f.data;
  if (h->sess != r.sess || !tagOk(f.data, f.len, r.sk)) return false;
  return r.rx.accept(h->seq);
}

void copyPrintable(char* dst, const char* src, size_t n) {   // member-supplied text goes into the TSV FLT= list
  size_t i = 0;
  for (; i + 1 < n && src[i]; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F && src[i] != '\t') ? src[i] : '?';
  dst[i] = 0;
}

bool nameOk(const char* s);   // defined with the M: helpers below

void onJoinAck(const Rx& f) {
  int i = findMac(f.mac);
  if (i < 0 || !sessionOk(i, f, sizeof(JoinAck))) return;
  MemberRt& r = L->rt[i];
  JoinAck a; memcpy(&a, f.data, sizeof(a));
  uint8_t t[32];
  if (!Hmac(L->st.m[i].fk, KEY_LEN).add("CFLO-FLT-ACK-1").add(r.nl, NONCE_LEN).add(r.nc, NONCE_LEN).add16(r.sess).finish(t))
    return;
  if (!ctEqual(t, a.proof, sizeof(a.proof))) return;
  r.lastRxMs = millis(); r.rssi = f.rssi;
  if (r.state != M_JOINED) {
    r.state = M_JOINED;
    // Back within the grace period (a quick reboot): it keeps its role and counts at once -- no reshuffle. A new or
    // long-gone member runs FULL until it has been joined for ROLE_STABLE_MS.
    const uint32_t nowJ = millis();
    const bool quick = r.lostAtMs && nowJ - r.lostAtMs < ROLE_GRACE_MS;
    r.joinedAtMs = quick ? nowJ - ROLE_STABLE_MS : nowJ;
    if (!quick) r.role = PLANROLE_FULL;
    r.lostAtMs = 0;
    r.planSentGen = (uint8_t)(L->planGen - 1);   // force a Plan to go out with the next tick
    r.board = a.board; r.caps = a.caps;
    memcpy(r.bleMac, a.bleMac, 6);
    copyPrintable(r.fw, a.fw, sizeof(r.fw));                     // reads at most sizeof(r.fw)-1 = sizeof(a.fw) bytes
    // Placeholder name (the phone held this member's key without a unit name -> "member" / "scanner-…" / empty): take
    // the name the member reports in its authenticated JoinAck, and save it, so the Fleet card + "seen by" chips show it.
    {
      MemberRec& mr = L->st.m[i];
      char nm[21]; copyPrintable(nm, a.name, sizeof(nm));
      if (nm[0] && nameOk(nm) && (!mr.name[0] || !strcmp(mr.name, "member") || !strncmp(mr.name, "scanner-", 8)) &&
          strcmp(mr.name, nm)) {
        Store next = L->st;
        strncpy(next.m[i].name, nm, sizeof(next.m[i].name) - 1);
        if (saveStore(next, L->addCtr)) L->st = next;
      }
    }
    Serial.printf("[FLT] member %d (%s) JOINED, fw %s\n", i + 1, L->st.m[i].name, r.fw);
    leaderEvent("member_join", i);
    sendSync(i);
  } else if (millis() - r.lastSyncMs > 250) {
    sendSync(i);                       // the client resends JoinAck until a Sync gets through
  }
}

void onMStat(const Rx& f) {
  int i = findMac(f.mac);
  if (i < 0 || L->rt[i].state != M_JOINED || !sessionOk(i, f, sizeof(MStat))) return;
  MStat m; memcpy(&m, f.data, sizeof(m));
  MemberRt& r = L->rt[i];
  r.lastRxMs = millis(); r.rssi = f.rssi;
  r.heapKB = m.heapKB; r.txFailPm = m.txFailPm;
  memcpy(r.phoneMac, m.phoneMac, 6);
  r.srcMask = m.srcMask;
  r.deauth = m.deauth; r.evil = m.evil; r.rid = m.rid; r.statAtMs = r.lastRxMs;
  if (m.planGen != L->planGen && r.planSentGen == L->planGen && millis() - r.lastPlanMs > 1000)
    r.planSentGen = (uint8_t)(L->planGen - 1);                     // it missed the last Plan: resend on the next tick
  if (m.gpsFix && m.lat >= -900000000 && m.lat <= 900000000 && m.lon >= -1800000000 && m.lon <= 1800000000) {
    r.fixLat = m.lat / 1e7f; r.fixLon = m.lon / 1e7f; r.fixAtMs = r.lastRxMs;
  }
}

// ---- P4: role layout (docs §8.3) ----
void sendPlan(int i) {
  MemberRt& r = L->rt[i];
  uint8_t buf[sizeof(Plan) + TAG_LEN];
  Plan* p = (Plan*)buf;
  memset(buf, 0, sizeof(buf));
  p->planGen = L->planGen; p->role = r.role; p->slot = (uint8_t)(i + 1); p->nMembers = L->st.n;
  p->plan = fleet::planForRole(r.role, L->planGen);
  sendSealed(r.mac, buf, sizeof(Plan), Msg::Plan, r.sess, r.txSeq, r.sk);
  r.planSentGen = L->planGen;
  r.lastPlanMs = millis();
}

// Template over the units that count (joined >= 10 s, or lost < 30 s ago), in member order:
//   N=1 leader FULL | N=2 member WIFI + leader BLE | N=3 WIFI24 + WIFI5 + leader BLE | N>=4 extras FULL.
// A member that doesn't count yet runs FULL. Mode OFF = everyone FULL. Bumps planGen when anything changes.
void assignRoles() {
  const uint32_t now = millis();
  uint8_t roles[FLEET_MAX_MEMBERS] = {0};
  uint8_t leaderRole = PLANROLE_FULL;
  int pres[FLEET_MAX_MEMBERS], np = 0;
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    // A member mid-rejoin (lost within the grace, or joining again) keeps counting while it still holds a role, so a
    // quick reboot never flips the layout -- not even for the instant between its Hello and its JoinAck.
    const bool inGrace = !r.lostAtMs || now - r.lostAtMs < ROLE_GRACE_MS;
    bool counts = (r.state == M_JOINED && now - r.joinedAtMs >= ROLE_STABLE_MS) ||
                  ((r.state == M_LOST || r.state == M_JOINING) && r.role != PLANROLE_FULL && inGrace);
    if (counts) pres[np++] = i;
  }
  // Only split when it can work: the leader must have BLE on (it takes the BLE role) and each Wi-Fi member must have
  // passive capture (PRB) on plus the band its role covers; anyone else runs FULL. (802.15.4 rides along only where
  // MASK_154 is on; the C5 also requires the source bit.)
  // ...and only while the leader's OWN C5 is up: the leader takes the BLE share, so with its C5 down (past the sticky
  // window, not a busy miss) a split would leave nobody on BLE -- everyone runs FULL until it is back (bench P5).
  const bool ownC5Up = g_lastLinkOkMs && millis() - g_lastLinkOkMs < LINK_STICKY_MS;
  if (L->planMode != PLANMODE_OFF && np >= 1 && (g_srcMask & MASK_BLE) && ownC5Up) {
    int wifi[FLEET_MAX_MEMBERS], nw = 0;
    for (int k = 0; k < np; k++) {
      const uint8_t m = L->rt[pres[k]].srcMask;
      if ((m & MASK_PROBE) && (m & (MASK_WIFI24 | MASK_WIFI5))) wifi[nw++] = pres[k];
    }
    if (nw >= 1) {
      leaderRole = PLANROLE_BLE;
      if (nw == 1) roles[wifi[0]] = PLANROLE_WIFI;
      else {
        const bool aHas5 = L->rt[wifi[0]].srcMask & MASK_WIFI5, bHas5 = L->rt[wifi[1]].srcMask & MASK_WIFI5;
        const int five = (bHas5 || !aHas5) ? 1 : 0;                // prefer a member with 5 GHz on for WIFI5
        roles[wifi[five]] = PLANROLE_WIFI5;
        roles[wifi[1 - five]] = PLANROLE_WIFI24;
        if (!(L->rt[wifi[five]].srcMask & MASK_WIFI5)) { roles[wifi[0]] = PLANROLE_WIFI; roles[wifi[1]] = PLANROLE_FULL; }
      }                                                            // 3rd+ Wi-Fi-capable member: FULL
    }
  }
  for (int i = 0; i < L->st.n; i++)                                // M:o overrides, for members that count
    if (L->rt[i].roleOverride && L->rt[i].roleOverride - 1 <= PLANROLE_BLE) {
      bool counts = false;
      for (int k = 0; k < np; k++) if (pres[k] == i) counts = true;
      if (counts) roles[i] = (uint8_t)(L->rt[i].roleOverride - 1);
    }
  bool changed = leaderRole != L->leaderRole;
  for (int i = 0; i < L->st.n; i++) if (roles[i] != L->rt[i].role) changed = true;
  if (!changed) return;
  L->leaderRole = leaderRole;
  for (int i = 0; i < L->st.n; i++) L->rt[i].role = roles[i];
  L->planGen++;
  Serial.printf("[FLT] plan %u: leader %s", (unsigned)L->planGen, fleet::planRoleName(leaderRole));
  for (int i = 0; i < L->st.n; i++) Serial.printf(", %s %s", L->st.m[i].name, fleet::planRoleName(roles[i]));
  Serial.println();
  leaderEvent("plan", -1);
}

// ---- P5: members' detections (docs §9.2) ----
// A variable-length sealed frame from member i: exact length for the row count it declares, then the usual checks.
bool batchOk(int i, const Rx& f, size_t hdrLen, size_t rowLen, uint8_t cnt, uint8_t maxCnt) {
  MemberRt& r = L->rt[i];
  if (!r.enc || r.state != M_JOINED || cnt == 0 || cnt > maxCnt || f.len != hdrLen + cnt * rowLen + TAG_LEN) return false;
  const Hdr* h = (const Hdr*)f.data;
  if (h->sess != r.sess || !tagOk(f.data, f.len, r.sk)) return false;
  return r.rx.accept(h->seq);
}

bool sameDet(const Detection& a, const Detection& b) {
  if (a.source != b.source || memcmp(a.mac, b.mac, 6)) return false;
  if (a.source == (uint8_t)Source::Ieee802154 && !(a.flags & FLAG_154_EXTENDED)) return a.panId == b.panId;
  return true;
}

void onDetBatch(const Rx& f) {
  int i = findMac(f.mac);
  if (i < 0 || !L->remote || f.len < sizeof(DetBatchHdr) + TAG_LEN) return;
  const uint8_t cnt = ((const DetBatchHdr*)f.data)->cnt;
  if (!batchOk(i, f, sizeof(DetBatchHdr), sizeof(Detection), cnt, DETS_PER_V2)) return;
  L->rt[i].lastRxMs = millis();                                    // traffic counts as alive too
  const uint32_t now = millis();
  for (int k = 0; k < cnt; k++) {
    Detection d; memcpy(&d, f.data + sizeof(DetBatchHdr) + k * sizeof(Detection), sizeof(d));
    d.name[sizeof(d.name) - 1] = 0;                                // member text: always terminated
    int hit = -1, freeIx = -1, oldest = -1;
    for (int j = 0; j < L->nRemote; j++) {
      RemoteRow& rr = L->remote[j];
      if (!rr.used || now - rr.lastMs >= REMOTE_TTL_MS) { rr.used = false; if (freeIx < 0) freeIx = j; continue; }
      if (rr.member == i && sameDet(rr.d, d)) { hit = j; break; }
      if (oldest < 0 || rr.lastMs < L->remote[oldest].lastMs) oldest = j;
    }
    const int j = hit >= 0 ? hit : freeIx >= 0 ? freeIx : oldest;
    if (j < 0) break;
    L->remote[j].d = d; L->remote[j].member = (uint8_t)i; L->remote[j].used = true; L->remote[j].lastMs = now;
  }
  L->rowsRx += cnt; L->framesRx++;
}

void onRidBatch(const Rx& f) {
  int i = findMac(f.mac);
  if (i < 0 || f.len < sizeof(RidBatchHdr) + TAG_LEN) return;
  const uint8_t cnt = ((const RidBatchHdr*)f.data)->cnt;
  if (!batchOk(i, f, sizeof(RidBatchHdr), sizeof(RemoteId), cnt, RIDS_PER_V2)) return;
  const uint32_t now = millis();
  for (int k = 0; k < cnt; k++) {
    RemoteId r; memcpy(&r, f.data + sizeof(RidBatchHdr) + k * sizeof(RemoteId), sizeof(r));
    r.uasId[sizeof(r.uasId) - 1] = 0; r.operatorId[sizeof(r.operatorId) - 1] = 0;   // member text: always terminated
    int hit = -1, freeIx = -1, oldest = -1;
    for (int j = 0; j < REMOTE_RIDS; j++) {
      RemoteRid& e = L->rids[j];
      if (!e.used || now - e.lastMs >= REMOTE_TTL_MS) { e.used = false; if (freeIx < 0) freeIx = j; continue; }
      if (e.member == i && !memcmp(e.r.mac, r.mac, 6)) { hit = j; break; }
      if (oldest < 0 || e.lastMs < L->rids[oldest].lastMs) oldest = j;
    }
    const int j = hit >= 0 ? hit : freeIx >= 0 ? freeIx : oldest;
    if (j < 0) break;
    L->rids[j].r = r; L->rids[j].member = (uint8_t)i; L->rids[j].used = true; L->rids[j].lastMs = now;
  }
}

// Drop a removed member's rows and renumber the rows of the members that slid down one slot (removeAt).
void remoteForget(int removed) {
  for (int j = 0; j < L->nRemote; j++) {
    RemoteRow& rr = L->remote[j];
    if (!rr.used) continue;
    if (rr.member == removed) rr.used = false; else if (rr.member > removed) rr.member--;
  }
  for (auto& e : L->rids) {
    if (!e.used) continue;
    if (e.member == removed) e.used = false; else if (e.member > removed) e.member--;
  }
}

const char* stateName(uint8_t s) {
  return s == M_JOINED ? "joined" : s == M_JOINING ? "joining" : s == M_LOST ? "lost" : "enrolled";
}

// --- M: command helpers ---
bool isHexN(const char* s, size_t n) {
  if (strlen(s) != n) return false;
  for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return false;
  return true;
}
bool isDigits(const char* s, size_t maxLen) { size_t n = strlen(s); return n && n <= maxLen && strspn(s, "0123456789") == n; }
void hexToBytes(const char* s, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) { char b[3] = { s[2 * i], s[2 * i + 1], 0 }; out[i] = (uint8_t)strtoul(b, nullptr, 16); }
}
bool nameOk(const char* s) {
  size_t n = strlen(s);
  if (!n || n > 20) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == ' ')) return false;
  }
  return true;
}
// devkey::hmac over "<a>|<b>|..." (ASCII), the encoding the web app uses for the enrollment wrap / tag.
bool keyHmac(uint8_t out[32], std::initializer_list<const char*> parts) {
  char msg[200];
  size_t n = 0;
  bool first = true;
  for (const char* p : parts) {
    size_t k = strlen(p);
    if (n + k + 1 >= sizeof(msg)) return false;
    if (!first) msg[n++] = '|';
    memcpy(msg + n, p, k); n += k;
    first = false;
  }
  bool ok = devkey::hmac((const uint8_t*)msg, n, out);
  memset(msg, 0, sizeof(msg));
  return ok;
}

void reply(char* out, size_t cap, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vsnprintf(out, cap, fmt, ap);
  va_end(ap);
}

// "a:<sid 12hex>:<kg>:<ctr>:<w 32hex>:<wrapped 32hex>:<tag 16hex>:<name>" (docs §5.2, §17). FK arrives masked under
// THIS unit's device key; the tag binds the leader sid, the enrollment counter (must equal the current one, which
// then advances: a captured command can never be replayed, even after a reboot), w, the member sid, kg, the wrapped
// key and the name.
void cmdAdd(char* args, char* out, size_t cap) {
  char* f[7]; int nf = 0;
  char* p = args;
  while (nf < 6) { char* c = strchr(p, ':'); if (!c) break; *c = 0; f[nf++] = p; p = c + 1; }
  f[nf++] = p;                                                   // the name is the rest (no ':' split)
  if (nf != 7) { reply(out, cap, "ERR\tbad args"); return; }
  const char *sidc = f[0], *kg = f[1], *ctr = f[2], *w = f[3], *wr = f[4], *tag = f[5], *name = f[6];
  if (!isHexN(sidc, 12) || !isDigits(kg, 9) || !isDigits(ctr, 9) || !isHexN(w, 32) || !isHexN(wr, 32) ||
      !isHexN(tag, 16) || !nameOk(name)) { reply(out, cap, "ERR\tbad args"); return; }
  if (!strcmp(sidc, devkey::sid())) { reply(out, cap, "ERR\tthat is this scanner"); return; }
  if (strtoul(ctr, nullptr, 10) != L->addCtr) { reply(out, cap, "ERR\tstale (refresh and retry)"); return; }
  uint8_t t[32], pad[32];
  if (!keyHmac(t, { "CFLO-FLT-ADD-2", devkey::sid(), ctr, w, sidc, kg, wr, name }) ||
      !keyHmac(pad, { "CFLO-FLT-WRAP-1", devkey::sid(), w, sidc })) { reply(out, cap, "ERR\tcrypto"); return; }
  uint8_t tagb[8]; hexToBytes(tag, tagb, 8);
  if (!ctEqual(t, tagb, 8)) { memset(pad, 0, sizeof(pad)); reply(out, cap, "ERR\tbad tag (key for this leader?)"); return; }
  MemberRec m = {};
  sidFromHex(sidc, m.sid);
  m.kg = strtoul(kg, nullptr, 10);
  strncpy(m.name, name, sizeof(m.name) - 1);
  uint8_t wrb[16]; hexToBytes(wr, wrb, 16);
  for (int k = 0; k < 16; k++) m.fk[k] = wrb[k] ^ pad[k];
  memset(pad, 0, sizeof(pad));
  int i = findSid(m.sid);
  Store next = L->st;
  const int slot = i < 0 ? next.n : i;
  if (i < 0) {
    if (next.n >= FLEET_MAX_MEMBERS) { memset(&m, 0, sizeof(m)); reply(out, cap, "ERR\tfull"); return; }
    next.n++;
  }
  next.m[slot] = m;
  memset(&m, 0, sizeof(m));
  bool saved = saveStore(next, L->addCtr + 1);
  if (!saved) { memset(&next, 0, sizeof(next)); reply(out, cap, "ERR\tNVS write failed"); return; }
  if (i >= 0) endSession(i, M_ENROLLED);                         // re-add after a rekey: the old FK is dead
  else memset(&L->rt[slot], 0, sizeof(MemberRt));
  L->st = next;
  memset(&next, 0, sizeof(next));
  L->addCtr++;
  restartProbing();
  Serial.printf("[FLT] member %d added: %s (%s kg %lu)\n", slot + 1, L->st.m[slot].name, sidc, (unsigned long)L->st.m[slot].kg);
  leaderEvent("member_add", slot);
  reply(out, cap, "OK\tadded\t%s", sidc);
}

bool removeAt(int i) {
  Store next = L->st;
  for (int k = i; k < next.n - 1; k++) next.m[k] = next.m[k + 1];
  next.n--;
  memset(&next.m[next.n], 0, sizeof(MemberRec));
  bool ok = saveStore(next, L->addCtr);
  if (ok) {
    endSession(i, M_ENROLLED);
    if (L->remote) remoteForget(i);
    // Slots shift down: each shifted member learns its new slot from its next Plan (sent at once: planSentGen stale).
    for (int k = i; k < L->st.n - 1; k++) { L->rt[k] = L->rt[k + 1]; L->rt[k].planSentGen = (uint8_t)(L->planGen - 1); }
    memset(&L->rt[L->st.n - 1], 0, sizeof(MemberRt));
    L->st = next;
  }
  memset(&next, 0, sizeof(next));
  return ok;
}

void cmdList(char* out, size_t cap) {
  const uint32_t now = millis();
  int n = snprintf(out, cap, "OK\t%u\t%u\t%u\t%u\t%u\t%lu\t%s", (unsigned)fleet::role(), (unsigned)fleet::active(),
                   (unsigned)g_ch, L ? (unsigned)L->planMode : 0u, (unsigned)g_maxPayload, L ? (unsigned long)L->addCtr : 0UL,
                   fleet::planRoleName(L ? L->leaderRole : 0));
  if (!L) return;
  for (int i = 0; i < L->st.n && n > 0 && n < (int)cap; i++) {
    const MemberRec& m = L->st.m[i];
    const MemberRt& r = L->rt[i];
    char sh[13]; sidToHex(m.sid, sh);
    const bool live = r.state == M_JOINED;
    n += snprintf(out + n, cap - n, "\n%d\t%s\t%s\t%lu\t%u\t%d\t%lu\t%s\t0\t-1\t-1\t0\t0\t%u\t%s", i + 1, sh, m.name,
                  (unsigned long)m.kg, (unsigned)r.state, live ? (int)r.rssi : 0,
                  live ? (unsigned long)((now - r.lastRxMs) / 1000) : 0UL, fleet::planRoleName(r.role), (unsigned)r.txFailPm,
                  live ? r.fw : "");
  }
}

}  // namespace

// ------------------------------------------------------------------ role hooks (fleet_internal.h)
namespace fleet { namespace detail { namespace leader {

void begin() {
  if (!loadStore()) { Serial.println("[FLT] leader: out of memory"); return; }
  for (int i = 0; i < FLEET_MAX_MEMBERS; i++) { memset(&L->rt[i], 0, sizeof(MemberRt)); }
  static const uint8_t kZero[8] = {0};
  while (!memcmp(L->fleetId, kZero, sizeof(L->fleetId))) esp_fill_random(L->fleetId, sizeof(L->fleetId));
  L->probing = false;
  L->lastRemacMs = millis();
  L->fixMember = -1; L->fixUsed = -1;
  if (!L->remote) {                                                // P5 remote table: PSRAM on the S3
    L->remote = (RemoteRow*)heap_caps_calloc(REMOTE_ROWS, sizeof(RemoteRow), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    L->nRemote = REMOTE_ROWS;
    if (!L->remote) {
      L->remote = (RemoteRow*)calloc(REMOTE_ROWS_NOPSRAM, sizeof(RemoteRow));
      L->nRemote = L->remote ? REMOTE_ROWS_NOPSRAM : 0;
    }
  }
  Serial.printf("[FLT] leader: %u member(s) enrolled\n", (unsigned)L->st.n);
}

void onFrame(const Rx& f) {
  if (!L || f.data[0] != FLEET_PROTO_VERSION) return;
  switch ((Msg)f.data[1]) {
    case Msg::Hello:   onHello(f); break;
    case Msg::JoinAck: onJoinAck(f); break;
    case Msg::MStat:   onMStat(f); break;
    case Msg::DetBatch: onDetBatch(f); break;
    case Msg::RidBatch: onRidBatch(f); break;
    default: break;                    // Probe/Join/Sync/Ctrl are leader -> client only
  }
}

void tick() {
  if (!L) return;
  const uint32_t now = millis();
  bool unjoined = false, anySession = false;
  for (int i = 0; i < L->st.n; i++) {
    MemberRt& r = L->rt[i];
    if (r.state == M_JOINING) {
      // Encrypt the peer only once the cleartext Join has LEFT the radio (esp_now_send just queues it); the
      // client's JoinAck (encrypted) follows. Fallback if the send callback was missed.
      if (!r.enc && (sendDone(r.mac) || now - r.joinSentMs >= ENCRYPT_FALLBACK_MS)) {
        r.enc = peerSet(r.mac, r.sk);
        if (!r.enc) { Serial.printf("[FLT] member %d: encrypt failed\n", i + 1); endSession(i, M_ENROLLED); continue; }
      }
      if (now - r.joinSentMs > JOIN_TIMEOUT_MS) {
        Serial.printf("[FLT] member %d: no JoinAck, back to enrolled\n", i + 1);
        endSession(i, M_ENROLLED);
      }
    } else if (r.state == M_JOINED) {
      if (now - r.lastRxMs > LOST_MS) {
        Serial.printf("[FLT] member %d (%s) LOST\n", i + 1, L->st.m[i].name);
        endSession(i, M_LOST);
        leaderEvent("member_lost", i);
      } else if (now - r.lastSyncMs >= SYNC_MS) {
        sendSync(i);
      }
    }
    if (L->rt[i].state != M_JOINED) unjoined = true;
    if (L->rt[i].state == M_JOINED || L->rt[i].state == M_JOINING) anySession = true;
  }
  if (anySession) L->lastRemacMs = now;
  else if (now - L->lastRemacMs >= REMAC_IDLE_MS && !webshare::active()) {
    // Nobody joined: a fresh MAC so a long-running leader's probes can't be tracked across hours (clients learn
    // the leader MAC from each Probe; nothing stores it). Joined sessions are never interrupted.
    L->lastRemacMs = now;
    if (!fleet::rerandomizeMac()) { Serial.println("[FLT] leader: radio restart failed -- rebooting"); event("radio_fail"); delay(200); ESP.restart(); }
  }
  if (unjoined) {                      // probe only while someone enrolled is missing (docs §6)
    if (!L->probing) { L->probing = true; L->burstStartMs = now; L->lastProbeMs = 0; }
    uint32_t iv = now - L->burstStartMs < PROBE_BURST_MS ? PROBE_FAST_MS : PROBE_SLOW_MS;
    if (!L->lastProbeMs || now - L->lastProbeMs >= iv) { L->lastProbeMs = now; sendProbe(); }
  } else {
    L->probing = false;
  }
  assignRoles();
  for (int i = 0; i < L->st.n; i++) {                              // deliver the layout (and refresh it every 10 s)
    MemberRt& r = L->rt[i];
    if (r.state == M_JOINED && r.enc && (r.planSentGen != L->planGen || now - r.lastPlanMs >= PLAN_RESEND_MS)) sendPlan(i);
  }
  if (L->chPending && (int32_t)(now - L->chSwitchAtMs) >= 0) {   // members were told CTRL_SETCH; follow them
    uint8_t ch = L->chPending; L->chPending = 0;
    fleet::setChannel(ch);
  }
}

void fillUi(Ui& u) {
  uint8_t e, j; counts(e, j);
  u.members = e; u.joinedMembers = j;
}

// The fleet-position fallback from members' own GPS. STICKY: keep the current member while its fix is live, so the
// fleet track can't bounce between two members a few metres apart every Sync; otherwise switch to the freshest
// live one. With none live, return the most recent one with its true age (the held fallback), live = false.
// Fold fresh member rows into this cycle's table (right after requestScan(); docs §9.2). A row this unit also has:
// keep the stronger RSSI, OR the flags, fill blanks, add the member's "seen by" bit. Otherwise append it. Member
// rows go through the same self / phone filters as local ones. Remote-IDs likewise into g_rids.
int mergeRemote() {
  if (!L || !L->remote) return 0;
  const uint32_t now = millis();
  uint8_t peer[6]; const bool havePeer = phone::peerMac(peer);
  int added = 0;
  for (int j = 0; j < L->nRemote; j++) {
    RemoteRow& rr = L->remote[j];
    if (!rr.used) continue;
    if (now - rr.lastMs >= REMOTE_TTL_MS || rr.member >= L->st.n) { rr.used = false; continue; }
    const Detection& d = rr.d;
    if (isSelfDet(d) || (havePeer && d.source == (uint8_t)Source::BleScan && !memcmp(d.mac, peer, 6))) continue;
    const uint8_t bit = (uint8_t)(1u << (rr.member + 1));
    int k = -1;
    for (int x = 0; x < g_detCount; x++) if (sameDet(g_dets[x], d)) { k = x; break; }
    if (k >= 0) {
      // This unit heard it too: its own row (and so its log line) stays untouched; the member's stronger RSSI and its
      // detector flags go to the display-only side arrays (REV-001: no member data in the leader's own log rows).
      int8_t& mr = g_detFleetRssi[k][rr.member];                     // per member: DETS v10's per-unit RSSI list
      if (d.rssi > mr) mr = d.rssi;
      g_detFleetFlags[k] |= d.flags;
      g_detVia[k] |= bit;
      L->merged++;
    } else if (g_detCount < MAX_DET) {
      g_dets[g_detCount] = d;                                        // a member-only row: never logged (no bit 0)
      clearFleetRssi(g_detCount); g_detFleetFlags[g_detCount] = 0;
      g_detFleetRssi[g_detCount][rr.member] = d.rssi;               // so the per-unit list reads every member alike
      g_detVia[g_detCount++] = bit;
      added++; L->appended++;
    }
  }
  for (auto& e : L->rids) {
    if (!e.used) continue;
    if (now - e.lastMs >= REMOTE_TTL_MS || e.member >= L->st.n) { e.used = false; continue; }
    const uint8_t bit = (uint8_t)(1u << (e.member + 1));
    int k = -1;
    for (int x = 0; x < g_ridStoreCount; x++) if (!memcmp(g_rids[x].mac, e.r.mac, 6)) { k = x; break; }
    if (k >= 0) g_ridVia[k] |= bit;
    else if (g_ridStoreCount < MAX_RID) { g_rids[g_ridStoreCount] = e.r; g_ridVia[g_ridStoreCount++] = bit; }
  }
  return added;
}

bool hasRemote() {
  if (!L || !L->remote) return false;
  const uint32_t now = millis();
  for (int j = 0; j < L->nRemote; j++) if (L->remote[j].used && now - L->remote[j].lastMs < REMOTE_TTL_MS) return true;
  return false;
}

void alertMax(uint16_t* deauth, uint8_t* evil, uint8_t* rid) {
  if (!L) return;
  const uint32_t now = millis();
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    if (r.state != M_JOINED || !r.statAtMs || now - r.statAtMs >= MEMBER_STAT_FRESH_MS) continue;
    if (r.deauth > *deauth) *deauth = r.deauth;
    if (r.evil > *evil) *evil = r.evil;
    if (r.rid > *rid) *rid = r.rid;
  }
}

void fwdStats(FwdStats& s) {
  if (!L) return;
  s.rowsRx = L->rowsRx; s.framesRx = L->framesRx; s.merged = L->merged; s.appended = L->appended;
}

link_protocol::ScanPlan ownPlan(uint8_t* role) {
  *role = L ? L->leaderRole : PLANROLE_FULL;
  return fleet::planForRole(*role, L ? L->planGen : 0);
}

bool memberFix(float* lat, float* lon, uint32_t* ageMs, bool* live) {
  if (!L) return false;
  const uint32_t now = millis();
  auto isLive = [&](int i) {
    const MemberRt& r = L->rt[i];
    return i >= 0 && i < L->st.n && r.state == M_JOINED && r.fixAtMs && now - r.fixAtMs < MEMBER_FIX_LIVE_MS;
  };
  int pick = isLive(L->fixMember) ? L->fixMember : -1;
  if (pick < 0) {
    for (int i = 0; i < L->st.n; i++)
      if (isLive(i) && (pick < 0 || L->rt[i].fixAtMs > L->rt[pick].fixAtMs)) pick = i;
    L->fixMember = (int8_t)pick;
  }
  if (pick < 0) {                                                  // none live: the most recent fix ever seen
    for (int i = 0; i < L->st.n; i++)
      if (L->rt[i].fixAtMs && (pick < 0 || L->rt[i].fixAtMs > L->rt[pick].fixAtMs)) pick = i;
    if (pick < 0) return false;
  }
  *lat = L->rt[pick].fixLat; *lon = L->rt[pick].fixLon; *ageMs = now - L->rt[pick].fixAtMs;
  *live = isLive(pick);
  L->fixUsed = (int8_t)pick;                                       // Sync.fixSlot when this fix becomes the fleet position
  return true;
}

bool isSelf(const uint8_t mac[6]) {
  if (!L) return false;
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    if (r.state != M_JOINED) continue;
    if (macSet(r.bleMac) && !memcmp(r.bleMac, mac, 6)) return true;
    if (macSet(r.phoneMac) && !memcmp(r.phoneMac, mac, 6)) return true;
  }
  if (webshare::active()) { uint8_t ap[6]; WiFi.softAPmacAddress(ap); if (!memcmp(ap, mac, 6)) return true; }   // own AP
  return false;
}

bool ident(uint8_t id[8], uint8_t leaderSid[6]) {
  static const uint8_t kZero[8] = {0};
  if (!L || !memcmp(L->fleetId, kZero, 8)) return false;
  memcpy(id, L->fleetId, 8); memcpy(leaderSid, L->sid, 6);
  return true;
}
void counts(uint8_t& enrolled, uint8_t& joined) {
  enrolled = joined = 0;
  if (!L) return;
  enrolled = L->st.n;
  for (int i = 0; i < L->st.n; i++) if (L->rt[i].state == M_JOINED) joined++;
}

void printStatus() {
  if (!L) return;
  const uint32_t now = millis();
  Serial.printf("[FLT] leader: %u member(s), probing %s, enroll ctr %lu, plan %u (%s), own role %s\n", (unsigned)L->st.n,
                L->probing ? "yes" : "no", (unsigned long)L->addCtr, (unsigned)L->planGen,
                L->planMode == PLANMODE_OFF ? "off" : "auto", fleet::planRoleName(L->leaderRole));
  Serial.printf("[FLT] leader: remote rows rx %lu in %lu frames, merged %lu, appended %lu, table %d (%s)\n",
                (unsigned long)L->rowsRx, (unsigned long)L->framesRx, (unsigned long)L->merged,
                (unsigned long)L->appended, L->nRemote, L->remote && esp_ptr_external_ram(L->remote) ? "psram" : "internal");
  int own = 0, mem = 0, both = 0;                                  // this cycle's table by "seen by"
  for (int i = 0; i < g_detCount; i++) {
    if (g_detVia[i] == 1) own++; else if (g_detVia[i] & 1) both++; else mem++;
  }
  Serial.printf("[FLT] leader: table %d rows = %d own only + %d members only + %d both\n", g_detCount, own, mem, both);
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    char sh[13]; sidToHex(L->st.m[i].sid, sh);
    Serial.printf("[FLT]   %d %-20s %s kg %lu  %s  role %s", i + 1, L->st.m[i].name, sh, (unsigned long)L->st.m[i].kg,
                  stateName(r.state), fleet::planRoleName(r.role));
    if (r.state == M_JOINED)
      Serial.printf("  rssi %d  %lus ago  heap %u KB  fw %s", (int)r.rssi, (unsigned long)((now - r.lastRxMs) / 1000),
                    (unsigned)r.heapKB, r.fw);
    Serial.println();
  }
}

}}}  // namespace fleet::detail::leader

// ------------------------------------------------------------------ public: phone commands + STATUS field
namespace fleet {

namespace {
CmdAction handleCmd(char* buf, char* out, size_t cap) {
  const char op = buf[0];
  char* args = buf[1] == ':' ? buf + 2 : buf + 1;
  const bool canLead = hostboard::fleetLeaderCapable();

  if (op == 'j') {                                               // become a fleet client (reboot; CYD frees BT memory)
    if (role() == ROLE_CLIENT) { reply(out, cap, "ERR\talready a client"); return CMD_NONE; }
    if (!setRole(ROLE_CLIENT)) { reply(out, cap, "ERR\tNVS write failed"); return CMD_NONE; }
    event("client_set");
    reply(out, cap, "OK\trestarting as a fleet client");
    return CMD_RESTART;
  }
  if (op == 'L') {                                               // L1 / L0: leader on / off (L0 also: a mini client leaves)
    if (!canLead) { reply(out, cap, "ERR\tthis scanner can only be a fleet client"); return CMD_NONE; }
    if (args[0] == '1') {
      const uint8_t was = role();
      if (!setRole(ROLE_LEADER)) { reply(out, cap, "ERR\tNVS write failed"); return CMD_NONE; }
      if (active() && was != ROLE_LEADER) { reply(out, cap, "OK\trestarting as leader"); return CMD_RESTART; }
      if (!active() && !beginLeader()) { reply(out, cap, "ERR\tleader start failed (stop the Wi-Fi download?)"); return CMD_NONE; }
      leaderEvent("start", -1);
      reply(out, cap, "OK\tleader on");
      return CMD_NONE;
    }
    if (role() == ROLE_OFF) { reply(out, cap, "OK\tnot in a fleet"); return CMD_NONE; }
    event("leave");
    setRole(ROLE_OFF);
    reply(out, cap, "OK\trestarting (left the fleet)");
    return CMD_RESTART;
  }
  if (op == 'l') {
    if (canLead) loadStore();
    cmdList(out, cap);
    return CMD_NONE;
  }
  if (op == 'o') {                                               // "o:<sid>:<role|auto>" per-member role override
    if (!canLead || !loadStore()) { reply(out, cap, "ERR\tthis scanner can only be a fleet client"); return CMD_NONE; }
    char* c = strchr(args, ':');
    if (!c) { reply(out, cap, "ERR\tusage o:<sid>:<full|wifi|wifi24|wifi5|ble|auto>"); return CMD_NONE; }
    *c = 0;
    uint8_t sid[6]; int i = sidFromHex(args, sid) ? findSid(sid) : -1;
    if (i < 0) { reply(out, cap, "ERR\tnot a member"); return CMD_NONE; }
    const char* rn = c + 1;
    uint8_t r = 0;                                               // 0 = auto
    if (strcmp(rn, "auto")) {
      for (uint8_t k = PLANROLE_FULL; k <= PLANROLE_BLE; k++) if (!strcmp(rn, fleet::planRoleName(k))) r = (uint8_t)(k + 1);
      if (!r) { reply(out, cap, "ERR\tunknown role"); return CMD_NONE; }
    }
    L->rt[i].roleOverride = r;
    reply(out, cap, "OK\trole %s for %s", r ? fleet::planRoleName(r - 1) : "auto", args);
    return CMD_NONE;
  }
  if (op == 'p') {                                               // plan mode: 0 auto (role template) / 3 off (all full)
    if (!canLead || !loadStore()) { reply(out, cap, "ERR\tthis scanner can only be a fleet client"); return CMD_NONE; }
    int m = atoi(args);
    if (m != PLANMODE_AUTO && m != PLANMODE_OFF) { reply(out, cap, "ERR\tmode must be 0 (auto) or 3 (off)"); return CMD_NONE; }
    L->planMode = (uint8_t)m;
    Preferences pr;
    if (pr.begin("cydscan", false)) { pr.putUChar("fltpmode", L->planMode); pr.end(); }
    reply(out, cap, "OK\tplan mode %d", m);
    return CMD_NONE;
  }
  if (op == 'c') {
    int ch = atoi(args);
    if (ch < 1 || ch > 13) { reply(out, cap, "ERR\tchannel must be 1-13"); return CMD_NONE; }
    if (role() == ROLE_CLIENT && active()) { reply(out, cap, "ERR\ta client follows its leader's channel"); return CMD_NONE; }
    if (webshare::active()) { reply(out, cap, "ERR\tstop the Wi-Fi download first (it shares the channel)"); return CMD_NONE; }
    if (L && active() && role() == ROLE_LEADER) {
      bool told = false;
      for (int i = 0; i < L->st.n; i++) {
        if (L->rt[i].state != M_JOINED) continue;
        clearSent(L->rt[i].mac);
        for (int k = 0; k < 3; k++) sendCtrl(i, CTRL_SETCH, (uint8_t)ch);   // 3 tries: a member that misses it is stranded
        waitSent(L->rt[i].mac);                                  // out on the OLD channel before anyone moves
        told = true;
      }
      if (told) {                                                // members move now; the leader follows 300 ms later (tick)
        L->chPending = (uint8_t)ch; L->chSwitchAtMs = millis() + 300;
        reply(out, cap, "OK\tchannel %d", ch);
        return CMD_NONE;
      }
    }
    if (!setChannel((uint8_t)ch)) { reply(out, cap, "ERR\tNVS write failed"); return CMD_NONE; }
    reply(out, cap, "OK\tchannel %d", ch);
    return CMD_NONE;
  }
  // The rest edit the member store: a leader-capable board only (a member list can be prepared before leading).
  if (!canLead) { reply(out, cap, "ERR\tthis scanner can only be a fleet client"); return CMD_NONE; }
  if (!loadStore()) { reply(out, cap, "ERR\tout of memory"); return CMD_NONE; }
  if (op == 'a') { cmdAdd(args, out, cap); return CMD_NONE; }
  uint8_t sid[6];
  if ((op == 'r' || op == 'x') && !sidFromHex(args, sid)) { reply(out, cap, "ERR\tbad sid"); return CMD_NONE; }
  int i = (op == 'r' || op == 'x') ? findSid(sid) : -1;
  if (op == 'r') {
    if (i < 0) { reply(out, cap, "ERR\tnot a member"); return CMD_NONE; }
    if (active() && L->rt[i].state == M_JOINED) {
      clearSent(L->rt[i].mac);
      sendCtrl(i, CTRL_UNJOIN, 0); sendCtrl(i, CTRL_UNJOIN, 0);
      waitSent(L->rt[i].mac);                                    // the frames must leave before the peer (LMK) goes
    }
    MemberRec gone = L->st.m[i];                                 // the event names it after the slots shift
    memset(gone.fk, 0, sizeof(gone.fk));                         // ...and needs no key copy
    if (!removeAt(i)) { reply(out, cap, "ERR\tNVS write failed"); return CMD_NONE; }
    leaderEventRec("member_remove", &gone, i);
    reply(out, cap, "OK\tremoved\t%s", args);
    return CMD_NONE;
  }
  if (op == 'x') {                                               // release: the member leaves fleet mode and reboots normal
    if (i < 0) { reply(out, cap, "ERR\tnot a member"); return CMD_NONE; }
    if (!active() || L->rt[i].state != M_JOINED) { reply(out, cap, "ERR\tnot joined"); return CMD_NONE; }
    clearSent(L->rt[i].mac);
    for (int k = 0; k < 3; k++) sendCtrl(i, CTRL_RELEASE, 0);    // three tries; each has its own seq
    waitSent(L->rt[i].mac);
    endSession(i, M_LOST);                                       // it is rebooting: show it gone right away
    L->rt[i].role = PLANROLE_FULL; L->rt[i].lostAtMs = 0;        // ...and its coverage goes back to the others now
    leaderEvent("member_release", i);
    reply(out, cap, "OK\treleased\t%s", args);
    return CMD_NONE;
  }
  reply(out, cap, "ERR\tunknown op");
  return CMD_NONE;
}
}  // namespace

CmdAction handlePhoneCmd(const char* cmd, char* out, size_t cap) {
  if (!strncmp(cmd, "M:", 2)) cmd += 2;
  char buf[160];
  strncpy(buf, cmd, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  CmdAction a = handleCmd(buf, out, cap);
  memset(buf, 0, sizeof(buf));                                   // an M:a carried a wrapped fleet key
  return a;
}

void statusField(char* out, size_t cap) {
  uint8_t e = 0, j = 0;
  if (role() == ROLE_LEADER) detail::leader::counts(e, j);
  else if (role() == ROLE_CLIENT) { e = 1; j = active() && detail::client::joined() ? 1 : 0; }
  snprintf(out, cap, "%u,%u/%u,%u,%u", (unsigned)role(), (unsigned)j, (unsigned)e, (unsigned)channel(),
           hostboard::fleetLeaderCapable() ? 1u : 0u);
}

bool dbgAddMember(const char* sidHex, const char* fkHex, const char* name) {
#ifdef FLEET_DEBUG
  if (!hostboard::fleetLeaderCapable() || !loadStore()) return false;
  MemberRec m = {};
  if (!sidFromHex(sidHex, m.sid) || !isHexN(fkHex, 32) || !nameOk(name)) { Serial.println("[FLT] usage: fleet add <sid 12hex> <fk 32hex> <name>"); return false; }
  hexToBytes(fkHex, m.fk, 16);
  strncpy(m.name, name, sizeof(m.name) - 1);
  int i = findSid(m.sid);
  Store next = L->st;
  const int slot = i < 0 ? next.n : i;
  if (i < 0) { if (next.n >= FLEET_MAX_MEMBERS) return false; next.n++; }
  next.m[slot] = m;
  if (!saveStore(next, L->addCtr)) return false;
  if (i >= 0) endSession(i, M_ENROLLED); else memset(&L->rt[slot], 0, sizeof(MemberRt));
  L->st = next;
  restartProbing();
  Serial.printf("[FLT] member %d stored (bench): %s %s\n", slot + 1, sidHex, name);
  return true;
#else
  (void)sidHex; (void)fkHex; (void)name;
  return false;
#endif
}

}  // namespace fleet

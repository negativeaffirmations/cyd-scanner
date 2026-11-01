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

using namespace fleet::detail;

namespace {

constexpr uint32_t PROBE_FAST_MS = 3000, PROBE_SLOW_MS = 15000, PROBE_BURST_MS = 120000;   // docs §6
constexpr uint32_t NONCE_LIFE_MS = 10000;   // a Hello may answer either of the last 2 probes for 10 s
constexpr uint32_t JOIN_TIMEOUT_MS = 5000, ENCRYPT_FALLBACK_MS = 500, SENT_WAIT_MS = 200;
constexpr uint32_t SYNC_MS = 2000, LOST_MS = 10000;
constexpr uint32_t REMAC_IDLE_MS = 15UL * 60 * 1000;   // nobody joined: rotate the leader MAC this often (tracking)
constexpr int      HELLOS_PER_S = 10;
constexpr uint32_t MEMBER_FIX_LIVE_MS = 10000;   // a member's own GPS fix counts as live this long

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
};

struct Leader {
  Store    st;
  uint32_t addCtr;                     // next valid enrollment counter (NVS fltactr): every M:a binds it -> no replay
  MemberRt rt[FLEET_MAX_MEMBERS];
  uint8_t  sid[6];
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

void endSession(int i, uint8_t newState) {
  MemberRt& r = L->rt[i];
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
    s->flags |= 0x01;
  }
  PosFix f = posSnapshot();
  s->psrc = f.src;
  if (f.src) {
    s->lat = (int32_t)lroundf(f.lat * 1e7f); s->lon = (int32_t)lroundf(f.lon * 1e7f);
    s->fixAgeMs = f.ageMs;
    s->flags |= 0x02 | (f.live ? 0x04 : 0);
  }
  uint8_t m[6];
  if (phone::ownMac(m))  addSelf(s, m);                            // this leader's BLE advert
  if (phone::peerMac(m)) addSelf(s, m);                            // the phone connected to it
  if (webshare::active()) { WiFi.softAPmacAddress(m); addSelf(s, m); }   // its download AP
  for (int k = 0; k < L->st.n; k++) if (L->rt[k].state == M_JOINED) addSelf(s, L->rt[k].bleMac);
  memcpy(s->leaderSid, L->sid, 6);
  strncpy(s->leaderName, devkey::unitName(), sizeof(s->leaderName) - 1);
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
    r.board = a.board; r.caps = a.caps;
    memcpy(r.bleMac, a.bleMac, 6);
    copyPrintable(r.fw, a.fw, sizeof(r.fw));                     // reads at most sizeof(r.fw)-1 = sizeof(a.fw) bytes
    Serial.printf("[FLT] member %d (%s) JOINED, fw %s\n", i + 1, L->st.m[i].name, r.fw);
    event("member_join");
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
  if (m.gpsFix && m.lat >= -900000000 && m.lat <= 900000000 && m.lon >= -1800000000 && m.lon <= 1800000000) {
    r.fixLat = m.lat / 1e7f; r.fixLon = m.lon / 1e7f; r.fixAtMs = r.lastRxMs;
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
  event("member_add");
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
    // Slots shift down; joined members keep the slot number from their Join until they rejoin (P4 must re-send it).
    for (int k = i; k < L->st.n - 1; k++) L->rt[k] = L->rt[k + 1];
    memset(&L->rt[L->st.n - 1], 0, sizeof(MemberRt));
    L->st = next;
  }
  memset(&next, 0, sizeof(next));
  return ok;
}

void cmdList(char* out, size_t cap) {
  const uint32_t now = millis();
  int n = snprintf(out, cap, "OK\t%u\t%u\t%u\t0\t%u\t%lu", (unsigned)fleet::role(), (unsigned)fleet::active(), (unsigned)g_ch,
                   (unsigned)g_maxPayload, L ? (unsigned long)L->addCtr : 0UL);
  if (!L) return;
  for (int i = 0; i < L->st.n && n > 0 && n < (int)cap; i++) {
    const MemberRec& m = L->st.m[i];
    const MemberRt& r = L->rt[i];
    char sh[13]; sidToHex(m.sid, sh);
    const bool live = r.state == M_JOINED;
    n += snprintf(out + n, cap - n, "\n%d\t%s\t%s\t%lu\t%u\t%d\t%lu\t0\t0\t-1\t-1\t0\t0\t%u\t%s", i + 1, sh, m.name,
                  (unsigned long)m.kg, (unsigned)r.state, live ? (int)r.rssi : 0,
                  live ? (unsigned long)((now - r.lastRxMs) / 1000) : 0UL, (unsigned)r.txFailPm, live ? r.fw : "");
  }
}

}  // namespace

// ------------------------------------------------------------------ role hooks (fleet_internal.h)
namespace fleet { namespace detail { namespace leader {

void begin() {
  if (!loadStore()) { Serial.println("[FLT] leader: out of memory"); return; }
  for (int i = 0; i < FLEET_MAX_MEMBERS; i++) { memset(&L->rt[i], 0, sizeof(MemberRt)); }
  L->probing = false;
  L->lastRemacMs = millis();
  Serial.printf("[FLT] leader: %u member(s) enrolled\n", (unsigned)L->st.n);
}

void onFrame(const Rx& f) {
  if (!L || f.data[0] != FLEET_PROTO_VERSION) return;
  switch ((Msg)f.data[1]) {
    case Msg::Hello:   onHello(f); break;
    case Msg::JoinAck: onJoinAck(f); break;
    case Msg::MStat:   onMStat(f); break;
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
        event("member_lost");
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
  if (L->chPending && (int32_t)(now - L->chSwitchAtMs) >= 0) {   // members were told CTRL_SETCH; follow them
    uint8_t ch = L->chPending; L->chPending = 0;
    fleet::setChannel(ch);
  }
}

void fillUi(Ui& u) {
  uint8_t e, j; counts(e, j);
  u.members = e; u.joinedMembers = j;
}

bool memberFix(float* lat, float* lon, uint32_t* ageMs) {
  if (!L) return false;
  const uint32_t now = millis();
  int best = -1;
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    if (r.state != M_JOINED || !r.fixAtMs || now - r.fixAtMs >= MEMBER_FIX_LIVE_MS) continue;
    if (best < 0 || r.fixAtMs > L->rt[best].fixAtMs) best = i;
  }
  if (best < 0) return false;
  *lat = L->rt[best].fixLat; *lon = L->rt[best].fixLon; *ageMs = now - L->rt[best].fixAtMs;
  return true;
}

bool isSelf(const uint8_t mac[6]) {
  if (!L) return false;
  for (int i = 0; i < L->st.n; i++)
    if (L->rt[i].state == M_JOINED && macSet(L->rt[i].bleMac) && !memcmp(L->rt[i].bleMac, mac, 6)) return true;
  return false;
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
  Serial.printf("[FLT] leader: %u member(s), probing %s, enroll ctr %lu\n", (unsigned)L->st.n, L->probing ? "yes" : "no",
                (unsigned long)L->addCtr);
  for (int i = 0; i < L->st.n; i++) {
    const MemberRt& r = L->rt[i];
    char sh[13]; sidToHex(L->st.m[i].sid, sh);
    Serial.printf("[FLT]   %d %-20s %s kg %lu  %s", i + 1, L->st.m[i].name, sh, (unsigned long)L->st.m[i].kg,
                  stateName(r.state));
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
      event("start");
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
    if (!removeAt(i)) { reply(out, cap, "ERR\tNVS write failed"); return CMD_NONE; }
    event("member_remove");
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
    event("member_release");
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

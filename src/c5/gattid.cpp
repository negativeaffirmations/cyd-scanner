// gattid.cpp — see gattid.h and docs/gatt-identify.md.
//
// Raw NimBLE host API, driven by a non-blocking state machine from loop(). The GAP/GATT callbacks run on the NimBLE host
// task and only copy a value + set volatile flags; every decision is made in tick().
#include "gattid.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_system.h>

using namespace link_protocol;

namespace gattid {
namespace {

constexpr int      SEEN_MAX = 64, HIST_MAX = 256, CACHE_MAX = 32, HOUR_MAX = 60;
constexpr int8_t   RSSI_DEFAULT = -75, PRIO_EXTRA = 10;       // priority devices may be this much weaker
constexpr uint32_t OP_MS = 1000, HARD_MS = 2500, CONNECT_MS = 2000, DISC_WAIT_MS = 1000, STOP_MS = 500, MARGIN_MS = 500;
constexpr uint32_t MIN_GAP_MS = 20000, HOUR_MS = 3600000UL, DAY_MS = 86400000UL, RETRY_HARD_MS = 900000UL, TRY_MIN_MS = 900000UL;
constexpr uint32_t HINT_FRESH_MS = 10000, SKIP_MS = 600000, STUCK_RESTART_MS = 30000, STUCK_RETRY_MS = 3000, TRY_PENDING_MS = 60000;
constexpr uint32_t FRESH_MS = 1500;
constexpr uint16_t PREF_MTU = 64;

// ------------------------------------------------------------------ denylist
// Never connected to: our own units, Apple / Microsoft / Samsung / Garmin / Google devices, Fast Pair, hearing aids,
// health / fitness / audio-stream services, and anything advertising a personal or medical appearance.
const uint8_t kCfloSvc[16] = {0x6f, 0x5e, 0x4d, 0x3c, 0x2b, 0x1a, 0x00, 0x9b, 0x1a, 0x4c, 0x7e, 0x2b, 0x00, 0x00, 0x1e, 0x9a};  // LE of phone.cpp SVC_UUID
const uint16_t kDenySvc[] = {0xFE2C, 0xFDF0, 0x1854, 0x1808, 0x181F, 0x183A, 0x1810, 0x180D, 0xFEBC,
                             0x1822, 0x1809, 0x181D, 0x181B, 0x183E, 0x1814, 0x1816, 0x1818,
                             0x184E, 0x1850, 0x184F, 0x1846, 0x1844, 0x1853};
bool denySvc16(uint16_t u) { for (uint16_t d : kDenySvc) if (d == u) return true; return false; }
bool denyCompany(uint16_t c) { return c == 0x004C || c == 0x0006 || c == 0x0075 || c == 0x0087 || c == 0x00E0; }

// Appearance category = value >> 6 (Bluetooth Assigned Numbers): phone (1), watch (3), thermometer, heart rate, blood
// pressure, glucose meter (12-14, 16), running / cycling sensors (17, 18), wearable audio (37), hearing aid (41), the
// medical range pulse oximeter .. medication delivery (49-55), outdoor sports (81); plus the computers worn or carried
// (wearable computer 0x0086, tablet 0x0087).
bool personalAppearance(uint16_t a) {
  const uint16_t c = a >> 6;
  return c == 1 || c == 3 || c == 12 || c == 13 || c == 14 || c == 16 || c == 17 || c == 18 || c == 37 || c == 41 ||
         (c >= 49 && c <= 55) || c == 81 || a == 0x0086 || a == 0x0087;
}

// Walk the advert's AD structures (adv + scan response): true when any denylist item is present.
bool denyAdvert(const std::vector<uint8_t>& p) {
  const size_t n = p.size();
  for (size_t i = 0; i + 1 < n;) {
    const uint8_t len = p[i];
    if (!len || i + 1 + len > n) break;
    const uint8_t type = p[i + 1];
    const uint8_t* d = &p[i + 2];
    const int dl = len - 1;
    if ((type == 0x02 || type == 0x03) && dl >= 2) {
      for (int j = 0; j + 1 < dl; j += 2) if (denySvc16((uint16_t)(d[j] | (d[j + 1] << 8)))) return true;
    } else if ((type == 0x06 || type == 0x07) && dl >= 16) {
      for (int j = 0; j + 16 <= dl; j += 16) if (!memcmp(d + j, kCfloSvc, 16)) return true;
    } else if (type == 0x16 && dl >= 2) {
      if (denySvc16((uint16_t)(d[0] | (d[1] << 8)))) return true;           // service data under a denied UUID
    } else if (type == 0xFF && dl >= 2) {
      if (denyCompany((uint16_t)(d[0] | (d[1] << 8)))) return true;
    } else if (type == 0x19 && dl >= 2) {
      if (personalAppearance((uint16_t)(d[0] | (d[1] << 8)))) return true;
    }
    i += 1 + len;
  }
  return false;
}

// ------------------------------------------------------------------ state
struct Seen {
  uint8_t  mac[6];
  uint8_t  at;                // 0 public, 1 random static
  int8_t   rssi;
  uint8_t  adverts;
  bool     used, hasInd, deny;
  uint16_t pollFirst;
  uint32_t first, last, indAt;   // indAt = last connectable undirected advert (ADV_IND)
};
struct Hist {
  uint8_t  mac[6];
  uint8_t  fails;
  bool     used, gaveUp, deny, hardRetried;
  uint32_t nextOk, skipUntil, last, lastTry;
};
struct Cached { DevInfo di; uint8_t sentCnt; bool used; uint32_t lastSent, t; };

Seen    g_seen[SEEN_MAX];
Hist    g_hist[HIST_MAX];
Cached  g_cache[CACHE_MAX];
SemaphoreHandle_t g_lk;                     // guards g_seen (NimBLE task writes, loop reads)

uint8_t  g_mode = 2;                        // 0 off, 1 on, 2 auto (follows the host's hint)
int8_t   g_rssiCon = 0;                     // console RSSI floor override (0 = none)
volatile bool g_off = false;                // disabled for this boot
bool     g_hintOn = false;
int8_t   g_hintFloor = 0;
uint32_t g_hintAt = 0;                      // 0 = no hint yet
uint8_t  g_prio[GATT_HINT_MAX][6];
uint8_t  g_nPrio = 0;
uint16_t g_polls = 0;

uint32_t g_attempts = 0, g_oks = 0, g_lastAttempt = 0, g_lastCycle = 0xFFFFFFFFu, g_slotMs = 0, g_slotT = 0;
uint32_t g_hour[HOUR_MAX];
uint8_t  g_hourN = 0, g_hourHead = 0;
bool     g_haveTry = false;
uint8_t  g_try[6];
uint32_t g_tryAt = 0;
const char* g_why = "no tick yet";

// state machine
enum State : uint8_t { S_IDLE, S_STOP, S_CONN, S_MTU, S_READ, S_CLOSE };
State    g_st = S_IDLE;
volatile bool g_hold = false;
uint8_t  g_gen = 0;
uint32_t g_t0, g_stT, g_opT, g_closeT;
uint8_t  g_opIdx;
bool     g_terminated, g_hardHit;
uint32_t g_termSince = 0, g_stuckTry = 0;   // first terminate of the current link (0 = none); last stuck-retry
enum Fail : uint8_t { F_NONE, F_REFUSED, F_TIMEOUT };
Fail     g_fail;
Seen     g_cand;
bool     g_candPrio, g_candTry;
DevInfo  g_res;
bool     g_personal, g_auth, g_connected;

// written by the NimBLE host task
volatile bool     g_evConn, g_evConnFail, g_evMtu, g_evSec;
volatile bool     g_linkOpen = false;
volatile uint16_t g_connHandle;
volatile bool     g_opDone, g_opGot;
volatile int      g_opStatus;
volatile uint8_t  g_opLen;
uint8_t           g_opBuf[64];

// ------------------------------------------------------------------ helpers
bool macEq(const uint8_t* a, const uint8_t* b) { return !memcmp(a, b, 6); }
void macStr(const uint8_t* m, char* o) { snprintf(o, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }
bool timeUp(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
bool isRpa(const uint8_t* mac) { return (mac[0] & 0xC0) == 0x40; }

// An entry that must outlive eviction: a back-off still running, a give-up, a deny, a live host skip.
bool protectedHist(const Hist& h, uint32_t now) {
  return h.gaveUp || h.deny || (h.nextOk && !timeUp(now, h.nextOk)) || (h.skipUntil && !timeUp(now, h.skipUntil));
}

// Find (or with `create`, add) the history entry of `mac`. A full table evicts only the oldest unprotected entry; with
// none evictable, create fails (null) and the caller must not proceed.
Hist* histFind(const uint8_t* mac, bool create, uint32_t now) {
  int freeIdx = -1, oldIdx = -1;
  for (int i = 0; i < HIST_MAX; i++) {
    if (!g_hist[i].used) { if (freeIdx < 0) freeIdx = i; continue; }
    if (macEq(g_hist[i].mac, mac)) return &g_hist[i];
    if (!protectedHist(g_hist[i], now) && (oldIdx < 0 || (int32_t)(g_hist[i].last - g_hist[oldIdx].last) < 0)) oldIdx = i;
  }
  if (!create) return nullptr;
  const int idx = freeIdx >= 0 ? freeIdx : oldIdx;
  if (idx < 0) return nullptr;
  Hist* h = &g_hist[idx];
  memset(h, 0, sizeof(*h));
  memcpy(h->mac, mac, 6);
  h->used = true;
  h->last = now;
  return h;
}
bool histRoom(const uint8_t* mac, uint32_t now) {
  for (int i = 0; i < HIST_MAX; i++) if (!g_hist[i].used || macEq(g_hist[i].mac, mac) || !protectedHist(g_hist[i], now)) return true;
  return false;
}

// Printable ASCII only; a NUL ends the value; trailing blanks dropped. Returns the length written (NUL-terminated).
size_t sanitize(const uint8_t* in, size_t n, char* out, size_t cap) {
  size_t w = 0;
  for (size_t i = 0; i < n && in[i] && w + 1 < cap; i++) out[w++] = (in[i] >= 0x20 && in[i] <= 0x7E) ? (char)in[i] : '?';
  while (w && out[w - 1] == ' ') w--;
  out[w] = 0;
  return w;
}

// A name that probably carries a person's name: an apostrophe form (' ` U+2019 U+02BC) + s / S, then the end of the
// name or a non-letter ("Bob's iPhone", "BOB'S-PC", "Sam`s").
bool nameLooksPersonal(const uint8_t* b, size_t n) {
  for (size_t i = 0; i < n && b[i]; i++) {
    size_t l = 0;
    if (b[i] == '\'' || b[i] == '`') l = 1;
    else if (i + 2 < n && b[i] == 0xE2 && b[i + 1] == 0x80 && b[i + 2] == 0x99) l = 3;
    else if (i + 1 < n && b[i] == 0xCA && b[i + 1] == 0xBC) l = 2;
    if (!l || i + l >= n || (b[i + l] | 0x20) != 's') continue;
    const size_t j = i + l + 1;
    if (j >= n || !b[j] || !isalpha(b[j])) return true;
  }
  return false;
}

bool hintFresh(uint32_t now) { return g_hintAt && now - g_hintAt <= HINT_FRESH_MS; }

// Reads run only with the console not `off`, and never against an explicit host "off": console `on` overrides a MISSING
// hint (bench use), not a host that said no.
bool enabled(uint32_t now) {
  if (g_off || g_mode == 0) return false;
  if (hintFresh(now) && !g_hintOn) return false;
  if (g_mode == 1) return true;
  return hintFresh(now) && g_hintOn;
}

int8_t floorFor(bool prio) {
  const int8_t base = g_rssiCon ? g_rssiCon : (g_hintFloor ? g_hintFloor : RSSI_DEFAULT);
  return prio ? (int8_t)(base - PRIO_EXTRA) : base;
}
bool isPrio(const uint8_t* mac) { for (int i = 0; i < g_nPrio; i++) if (macEq(g_prio[i], mac)) return true; return false; }

// Candidate rules (docs/gatt-identify.md). `forTry` (`gatt try`) ignores back-off and give-up only. Caller holds g_lk
// and passes a `now` read inside it.
bool eligible(const Seen& e, uint32_t now, bool forTry, bool* prio) {
  if (!e.used || !e.hasInd || e.deny) return false;
  const bool p = isPrio(e.mac);
  if (prio) *prio = p;
  if (e.rssi < floorFor(p)) return false;
  if (e.adverts < 2 || now - e.first < 6000 || (uint16_t)(g_polls - e.pollFirst) < 2 || now - e.last > FRESH_MS || now - e.indAt > FRESH_MS) return false;
  Hist* h = histFind(e.mac, false, now);
  if (h) {
    if (h->skipUntil && timeUp(now, h->skipUntil)) h->skipUntil = 0;
    if (h->nextOk && timeUp(now, h->nextOk)) h->nextOk = 0;
    if (h->deny) return false;
    if (h->skipUntil) return false;                                   // host said skip (whitelisted / own phone / own unit)
    if (!forTry && (h->gaveUp || h->nextOk)) return false;
  }
  return true;
}

bool budgetOk(uint32_t now, uint32_t cycleId) {
  if (cycleId == g_lastCycle) return false;                           // one attempt per scan cycle
  if (g_lastAttempt && now - g_lastAttempt < MIN_GAP_MS) return false;
  if (g_hourN >= HOUR_MAX && now - g_hour[g_hourHead] < HOUR_MS) return false;
  return true;
}
void noteAttempt(uint32_t now, uint32_t cycleId) {
  g_lastAttempt = now; g_lastCycle = cycleId; g_attempts++;
  if (g_hourN < HOUR_MAX) g_hour[(g_hourHead + g_hourN++) % HOUR_MAX] = now;
  else { g_hour[g_hourHead] = now; g_hourHead = (g_hourHead + 1) % HOUR_MAX; }
}

bool pickCandidate(Seen& out, bool& prio, bool& isTry) {
  bool found = false; bool bestPrio = false; int bestRssi = -128;
  xSemaphoreTake(g_lk, portMAX_DELAY);
  const uint32_t now = millis();                                      // read inside the lock: observe() can't make last > now
  for (int i = 0; i < SEEN_MAX; i++) {
    const Seen& e = g_seen[i];
    if (g_haveTry && !macEq(e.mac, g_try)) continue;
    bool p = false;
    if (!eligible(e, now, g_haveTry, &p)) continue;
    if (!found || (p && !bestPrio) || (p == bestPrio && e.rssi > bestRssi)) { out = e; found = true; bestPrio = p; bestRssi = e.rssi; }
  }
  xSemaphoreGive(g_lk);
  prio = bestPrio; isTry = g_haveTry && found;
  return found;
}

// ------------------------------------------------------------------ NimBLE callbacks (host task)
int gapCb(struct ble_gap_event* ev, void* arg) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) {
        g_connHandle = ev->connect.conn_handle;
        g_linkOpen = true;
        if ((uint8_t)(uintptr_t)arg == g_gen) g_evConn = true;
      } else if ((uint8_t)(uintptr_t)arg == g_gen) {
        g_evConnFail = true;
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      g_linkOpen = false;
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      g_evSec = true;                     // the SM is compiled out, so this should not happen: close the link (loop)
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      g_evSec = true;
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default: break;
  }
  return 0;
}

int mtuCb(uint16_t, const struct ble_gatt_error*, uint16_t, void* arg) {
  if ((uint8_t)(uintptr_t)arg == g_gen) g_evMtu = true;
  return 0;
}

// Read By Type: the first value is kept; the closing call (BLE_HS_EDONE or an ATT error) ends the op.
int readCb(uint16_t, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg) {
  if ((uint8_t)(uintptr_t)arg != g_gen) return 0;
  if (err->status == 0 && attr && attr->om) {
    if (!g_opGot) {
      const uint16_t n = os_mbuf_len(attr->om);
      const uint16_t c = n < sizeof(g_opBuf) ? n : sizeof(g_opBuf);
      if (os_mbuf_copydata(attr->om, 0, c, g_opBuf) == 0) { g_opLen = (uint8_t)c; g_opGot = true; }
    }
    return 0;
  }
  g_opStatus = (err->status == BLE_HS_EDONE) ? 0 : err->status;
  g_opDone = true;
  return 0;
}

// ------------------------------------------------------------------ state machine
const struct { uint16_t uuid; uint8_t bit; } kOps[] = {
  {0x2A01, DI_APPEAR}, {0x2A00, DI_NAME}, {0x2A29, DI_MFR}, {0x2A24, DI_MODEL}, {0x2A26, DI_FW}, {0x2A27, DI_HW},
};
constexpr uint8_t N_OPS = sizeof(kOps) / sizeof(kOps[0]);
constexpr uint8_t ALL_TRIED = DI_NAME | DI_APPEAR | DI_MFR | DI_MODEL | DI_FW | DI_HW;

void cachePut(const DevInfo& di, uint32_t now) {
  int idx = -1, freeIdx = -1, old = 0;
  for (int i = 0; i < CACHE_MAX; i++) {
    if (!g_cache[i].used) { if (freeIdx < 0) freeIdx = i; continue; }
    if (macEq(g_cache[i].di.mac, di.mac)) { idx = i; break; }
    if ((int32_t)(g_cache[i].t - g_cache[old].t) < 0) old = i;
  }
  if (idx < 0) idx = freeIdx >= 0 ? freeIdx : old;
  g_cache[idx].di = di; g_cache[idx].used = true; g_cache[idx].sentCnt = 0; g_cache[idx].lastSent = 0; g_cache[idx].t = now;
}

void beginClose(uint32_t) { g_st = S_CLOSE; g_terminated = false; }

// Slot over: record the outcome, free the radio.
void finish(uint32_t now, bool connected) {
  Hist* h = histFind(g_cand.mac, true, now);                           // room was checked before the attempt
  char ms[18]; macStr(g_cand.mac, ms);
  const bool gotAny = connected && g_res.have != 0;
  if (connected && (g_fail == F_NONE || gotAny)) {
    g_oks++;
    cachePut(g_res, now);
    if (h) {
      h->last = now; h->fails = 0;
      if (g_personal) h->deny = true;                                  // personal / medical appearance: never again
      else if (g_hardHit && (g_res.tried & ALL_TRIED) != ALL_TRIED && !h->hardRetried) { h->hardRetried = true; h->nextOk = now + RETRY_HARD_MS; }
      else h->nextOk = now + DAY_MS;
    }
    Serial.printf("[C5] gatt %s ok rssi=%d have=0x%02x tried=0x%02x name='%s' mfr='%s' model='%s' fw='%s' hw='%s'\n", ms, (int)g_res.rssi,
                  (unsigned)g_res.have, (unsigned)g_res.tried, g_res.name, g_res.mfr, g_res.model, g_res.fw, g_res.hw);
  } else {
    static const uint32_t kLadder[] = {900000UL, HOUR_MS, 4 * HOUR_MS};
    uint8_t fails = 0; bool gaveUp = false;
    if (h) {
      h->last = now; h->fails++;
      if (h->fails > 3) h->gaveUp = true; else h->nextOk = now + kLadder[h->fails - 1];
      fails = h->fails; gaveUp = h->gaveUp;
    }
    Serial.printf("[C5] gatt %s %s (fail %u%s)\n", ms, g_fail == F_REFUSED ? "refused" : "timeout", (unsigned)fails, gaveUp ? ", given up" : "");
  }
  g_slotMs = now - g_slotT;
  g_hold = false;
  g_st = S_IDLE;
}

void startOp(uint32_t now) {
  g_opGot = false; g_opLen = 0; g_opStatus = 0; g_opDone = false;
  g_opT = now;
  ble_uuid16_t u = BLE_UUID16_INIT(kOps[g_opIdx].uuid);
  g_res.tried |= kOps[g_opIdx].bit;
  const int rc = ble_gattc_read_by_uuid(g_connHandle, 1, 0xFFFF, &u.u, readCb, (void*)(uintptr_t)g_gen);
  if (rc != 0) { g_fail = F_TIMEOUT; g_opDone = true; g_opStatus = rc; }
}

// Store the value of the op that just finished.
void takeOp() {
  const uint8_t bit = kOps[g_opIdx].bit;
  if (g_opGot) {
    if (bit == DI_APPEAR) {
      if (g_opLen >= 2) { g_res.appearance = (uint16_t)(g_opBuf[0] | (g_opBuf[1] << 8)); g_res.have |= DI_APPEAR; g_personal = personalAppearance(g_res.appearance); }
    } else if (bit == DI_NAME) {
      if (nameLooksPersonal(g_opBuf, g_opLen)) g_res.have |= DI_NAME_WITHHELD;
      else if (sanitize(g_opBuf, g_opLen, g_res.name, sizeof(g_res.name))) g_res.have |= DI_NAME;
    } else {
      char* dst = bit == DI_MFR ? g_res.mfr : bit == DI_MODEL ? g_res.model : bit == DI_FW ? g_res.fw : g_res.hw;
      const size_t cap = bit == DI_MFR ? sizeof(g_res.mfr) : bit == DI_MODEL ? sizeof(g_res.model) : sizeof(g_res.fw);
      if (sanitize(g_opBuf, g_opLen, dst, cap)) g_res.have |= bit;
    }
  } else {
    const int s = g_opStatus;
    if (s == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) || s == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC) ||
        s == BLE_HS_ATT_ERR(0x08) || s == BLE_HS_ATT_ERR(0x0C)) { g_res.have |= DI_AUTH; g_auth = true; }   // never followed by pairing
    else if (s == BLE_HS_ENOTCONN || s == BLE_HS_ETIMEOUT) g_fail = F_TIMEOUT;
  }
}

void startAttempt(uint32_t now, const Seen& c, bool prio, bool isTry) {
  g_cand = c; g_candPrio = prio; g_candTry = isTry;
  memset(&g_res, 0, sizeof(g_res));
  memcpy(g_res.mac, c.mac, 6); g_res.addrType = c.at; g_res.rssi = c.rssi;
  g_fail = F_NONE; g_personal = false; g_auth = false; g_connected = false; g_hardHit = false;
  g_evConn = g_evConnFail = g_evMtu = g_evSec = false;
  g_gen++;
  g_hold = true;                                                   // publish first: blePaused() is now true, nothing re-arms BLE
  { NimBLEScan* s = NimBLEDevice::getScan(); if (s && s->isScanning()) s->stop(); }
  g_t0 = g_stT = g_slotT = now;
  g_st = S_STOP;
}

void tickMachine(uint32_t now) {
  switch (g_st) {
    case S_STOP: {
      if (ble_gap_disc_active()) {
        if (now - g_stT > STOP_MS) { g_slotMs = now - g_slotT; g_hold = false; g_st = S_IDLE; }   // scan won't stop: give the radio back, no attempt
        return;
      }
      ble_addr_t peer; peer.type = g_cand.at;
      for (int i = 0; i < 6; i++) peer.val[i] = g_cand.mac[5 - i];                // text order -> little-endian
      ble_gap_conn_params cp = {};
      cp.scan_itvl = 0x0010; cp.scan_window = 0x0010; cp.itvl_min = 24; cp.itvl_max = 40; cp.latency = 0; cp.supervision_timeout = 100;
      g_t0 = now;                                                             // hard deadline counts from the connect call
      char ms[18]; macStr(g_cand.mac, ms);
      const int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &peer, CONNECT_MS, &cp, gapCb, (void*)(uintptr_t)g_gen);
      if (rc != 0) { Serial.printf("[C5] gatt %s connect rc=%d\n", ms, rc); g_slotMs = now - g_slotT; g_hold = false; g_st = S_IDLE; return; }   // not the device's fault: no back-off
      Serial.printf("[C5] gatt %s connecting (rssi %d%s)\n", ms, (int)g_cand.rssi, g_candPrio ? ", priority" : "");
      g_st = S_CONN;
      return;
    }
    case S_CONN:
      if (g_evConn) {
        g_evConn = false;
        g_connected = true;
        g_opT = now;
        g_evMtu = false;
        if (ble_gattc_exchange_mtu(g_connHandle, mtuCb, (void*)(uintptr_t)g_gen) != 0) { g_opIdx = 0; startOp(now); g_st = S_READ; }
        else g_st = S_MTU;
      } else if (g_evConnFail || now - g_t0 > CONNECT_MS + 300) {
        g_fail = F_REFUSED;
        if (!g_evConnFail) ble_gap_conn_cancel();
        if (g_linkOpen) { beginClose(now); } else finish(now, false);
      }
      return;
    case S_MTU:
      if (g_evMtu || now - g_opT > OP_MS || now - g_t0 > HARD_MS) {
        if (!g_evMtu) g_fail = F_TIMEOUT;                                    // a peer that can't answer the MTU request is stuck
        if (g_fail != F_NONE) { beginClose(now); return; }
        g_opIdx = 0; startOp(now); g_st = S_READ;
      } else if (!g_linkOpen) { g_fail = F_TIMEOUT; beginClose(now); }
      return;
    case S_READ: {
      if (g_evSec) { g_res.have |= DI_AUTH; beginClose(now); return; }
      if (!g_opDone) {
        if (now - g_opT > OP_MS) { g_fail = F_TIMEOUT; beginClose(now); }
        else if (now - g_t0 > HARD_MS) { g_fail = F_TIMEOUT; g_hardHit = true; beginClose(now); }
        return;
      }
      takeOp();
      if (g_fail != F_NONE || !g_linkOpen) { if (g_fail == F_NONE) g_fail = F_TIMEOUT; beginClose(now); return; }
      if (g_personal) {                                                      // a personal / medical device: nothing more is read or kept
        g_res.have = DI_APPEAR | DI_NAME_WITHHELD;
        g_res.tried |= DI_NAME;                                              // the name is "attempted" (skipped for privacy)
        beginClose(now);
        return;
      }
      if (++g_opIdx >= N_OPS) { beginClose(now); return; }
      if (now - g_t0 > HARD_MS) { g_fail = F_TIMEOUT; g_hardHit = true; beginClose(now); return; }
      startOp(now);
      return;
    }
    case S_CLOSE:
      if (!g_linkOpen) { finish(now, g_connected); return; }
      if (!g_terminated) {
        ble_gap_terminate(g_connHandle, BLE_ERR_REM_USER_CONN_TERM);
        g_terminated = true; g_closeT = now; g_stuckTry = now;
        if (!g_termSince) g_termSince = now;
      }
      if (now - g_closeT > DISC_WAIT_MS) finish(now, g_connected);           // 'stuck': tick() keeps closing it, no new attempts
      return;
    default: return;
  }
}

}  // namespace

// ------------------------------------------------------------------ public
// Public or random-static only: resolvable / non-resolvable private addresses (phones, watches, our hosts) are skipped.
bool stableAddr(uint8_t type, const uint8_t mac[6]) { return type == BLE_ADDR_PUBLIC || (type == BLE_ADDR_RANDOM && (mac[0] & 0xC0) == 0xC0); }

uint32_t worstSlotMs() { return STOP_MS + HARD_MS + DISC_WAIT_MS + MARGIN_MS; }

void begin() {
  g_lk = xSemaphoreCreateMutex();
  Preferences p;
  if (p.begin("gatt", true)) { g_mode = p.getUChar("mode", 2); g_rssiCon = p.getChar("rssi", 0); p.end(); }
  if (g_mode == 1 || g_mode > 2) g_mode = 2;    // `on` is never persisted
  ble_att_set_preferred_mtu(PREF_MTU);          // the C5 has no GATT server: nothing else depends on it
}

void disable(const char* why) {
  g_off = true;
  Serial.printf("[C5] gatt disabled for this boot: %s\n", why);
}

void observe(const NimBLEAdvertisedDevice* dev, const uint8_t mac[6], int8_t rssi) {
  if (!g_lk || g_off) return;
  const uint8_t at = dev->getAddressType();
  if (!stableAddr(at, mac)) return;
  const bool connectable = dev->getAdvType() == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND;   // undirected + connectable (not scan responses)
  xSemaphoreTake(g_lk, portMAX_DELAY);
  const uint32_t now = millis();
  Seen* e = nullptr; Seen* oldest = &g_seen[0];
  for (int i = 0; i < SEEN_MAX; i++) {
    if (!g_seen[i].used) { if (!e || e->used) e = &g_seen[i]; continue; }
    if (macEq(g_seen[i].mac, mac)) { e = &g_seen[i]; break; }
    if (!oldest->used || (int32_t)(g_seen[i].last - oldest->last) < 0) oldest = &g_seen[i];
  }
  if (!e) e = oldest;
  if (!e->used || !macEq(e->mac, mac)) {
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, 6);
    e->used = true; e->at = at; e->first = now; e->pollFirst = g_polls;
  }
  e->rssi = rssi; e->last = now;
  if (e->adverts < 255) e->adverts++;
  if (connectable) { e->hasInd = true; e->indAt = now; }
  if (!e->deny && denyAdvert(dev->getPayload())) e->deny = true;
  xSemaphoreGive(g_lk);
}

void applyHint(const GattHint* h) {
  g_polls++;
  if (!h || !(h->flags & GH_VALID)) return;
  const uint32_t now = millis();
  g_hintOn = (h->flags & GH_ENABLE) != 0;
  g_hintFloor = h->rssiFloor;
  g_hintAt = now ? now : 1;
  const uint8_t ns = h->nSkip > GATT_HINT_MAX ? GATT_HINT_MAX : h->nSkip;
  const uint8_t np = (uint8_t)(h->nPrio > GATT_HINT_MAX - ns ? GATT_HINT_MAX - ns : h->nPrio);
  for (uint8_t i = 0; i < ns; i++) {                  // a skip lasts 10 min past the last hint that named it; private addresses are never read anyway
    if (isRpa(h->mac[i])) continue;
    Hist* e = histFind(h->mac[i], true, now);
    if (e) { e->skipUntil = now + SKIP_MS; e->last = now; }
  }
  g_nPrio = np;
  for (uint8_t i = 0; i < np; i++) memcpy(g_prio[i], h->mac[ns + i], 6);
}

// Close a link that did not go away: keep asking (every 3 s), and restart the C5 if it is still open 30 s after the
// first terminate. Runs even when gattid is disabled.
static void closeStuckLink(uint32_t now) {
  if (!g_termSince) { g_termSince = now; g_stuckTry = 0; }
  if (now - g_termSince > STUCK_RESTART_MS) {
    Serial.println("[C5] gatt: link still open 30 s after terminate -- restarting");
    Serial.flush();
    esp_restart();
  }
  if (!g_stuckTry || now - g_stuckTry > STUCK_RETRY_MS) { g_stuckTry = now ? now : 1; ble_gap_terminate(g_connHandle, BLE_ERR_REM_USER_CONN_TERM); }
}

void tick(uint32_t now, bool canStart, uint32_t cycleId) {
  if (!g_linkOpen) g_termSince = 0;
  else if (g_st == S_IDLE || g_off) closeStuckLink(now);
  if (g_off) { if (g_st != S_IDLE) { g_hold = false; g_st = S_IDLE; } g_why = "disabled for this boot"; return; }
  if (g_st != S_IDLE) { tickMachine(now); return; }
  if (g_linkOpen) { g_why = "a link is still closing"; return; }
  if (g_haveTry && now - g_tryAt > TRY_PENDING_MS) { g_haveTry = false; Serial.println("[C5] gatt: try expired (not eligible within 60 s)"); }
  if (!enabled(now)) {
    g_why = g_mode == 0 ? "off (console)" : !g_hintAt ? "no host hint yet" : !hintFresh(now) ? "host hint stale (>10 s)" :
            !g_hintOn ? "host has reads off" : "off";
    return;
  }
  if (!canStart) { g_why = "no window (radio busy or too little time before the next AP scan)"; return; }
  if (!budgetOk(now, cycleId)) { g_why = "budget (1 per cycle, 20 s apart, 60 per hour)"; return; }
  Seen c; bool prio = false, isTry = false;
  if (!pickCandidate(c, prio, isTry)) { g_why = "no eligible device"; return; }
  if (!histRoom(c.mac, now)) { g_why = "history table full of protected entries"; return; }
  if (isTry) {
    g_haveTry = false;
    Hist* h = histFind(c.mac, true, now);
    if (!h) return;
    h->lastTry = now; h->gaveUp = false; h->fails = 0;                      // an explicit try may clear a give-up (not a deny)
  }
  noteAttempt(now, cycleId);
  g_why = "reading";
  startAttempt(now, c, prio, isTry);
}

bool holdsRadio() { return g_hold; }
bool busy() { return g_st != S_IDLE || g_linkOpen; }
uint32_t attempts() { return g_attempts; }
uint32_t successes() { return g_oks; }
uint32_t lastSlotMs() { return g_slotMs; }

int collect(const Detection* live, int nLive, DevInfo* out, int max) {
  const uint32_t now = millis();
  if (!enabled(now)) return 0;                  // reads off (host or console): cached results stay home too
  int n = 0;
  for (int i = 0; i < CACHE_MAX && n < max; i++) {
    Cached& c = g_cache[i];
    if (!c.used) continue;
    if (c.sentCnt >= 3 && now - c.lastSent < 60000) continue;
    bool present = false;
    for (int k = 0; k < nLive; k++)
      if (live[k].source == (uint8_t)Source::BleScan && macEq(live[k].mac, c.di.mac)) { present = true; break; }
    if (!present) continue;
    out[n++] = c.di;
    if (c.sentCnt < 255) c.sentCnt++;
    c.lastSent = now;
  }
  return n;
}

// ------------------------------------------------------------------ console
static bool parseMac(const char* s, uint8_t* m) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) { if (v[i] > 255) return false; m[i] = (uint8_t)v[i]; }
  return true;
}

bool console(const char* line) {
  if (strncmp(line, "gatt", 4) || (line[4] && line[4] != ' ')) return false;
  const char* a = line + (line[4] ? 5 : 4);
  const uint32_t now = millis();
  if (!strcmp(a, "on"))        g_mode = 1;
  else if (!strcmp(a, "off"))  g_mode = 0;
  else if (!strcmp(a, "auto")) g_mode = 2;
  else if (!strcmp(a, "save")) {
    Preferences p;
    if (p.begin("gatt", false)) {
      p.putUChar("mode", g_mode == 1 ? 2 : g_mode);                 // `on` (force without a host hint) is bench-only: saved as auto
      p.putChar("rssi", g_rssiCon); p.end();
      Serial.printf("[C5] gatt: saved (%s)\n", g_mode == 0 ? "off" : "auto");
    }
    return true;
  } else if (!strcmp(a, "clear")) {
    memset(g_hist, 0, sizeof(g_hist)); memset(g_cache, 0, sizeof(g_cache));
    g_haveTry = false; g_lastAttempt = 0; g_hourN = 0; g_hourHead = 0;
    Serial.println("[C5] gatt: history and results cleared");
    return true;
  } else if (!strncmp(a, "rssi ", 5)) {
    const int v = atoi(a + 5);
    if (v >= -100 && v <= -30) g_rssiCon = (int8_t)v; else if (v == 0) g_rssiCon = 0;
  } else if (!strncmp(a, "try ", 4)) {
    uint8_t m[6];
    if (!parseMac(a + 4, m)) { Serial.println("[C5] gatt try <aa:bb:cc:dd:ee:ff>"); return true; }
    const Hist* h = histFind(m, false, now);
    if (h && h->deny) { Serial.println("[C5] gatt: that device is denied (personal / medical): not tried"); return true; }
    if (h && h->lastTry && now - h->lastTry < TRY_MIN_MS) { Serial.println("[C5] gatt: that device was tried < 15 min ago"); return true; }
    memcpy(g_try, m, 6); g_haveTry = true; g_tryAt = now;
    Serial.println("[C5] gatt: one try at the next slot (back-off and give-up ignored; expires in 60 s)");
    return true;
  } else if (strcmp(a, "stat") && *a) {
    Serial.println("[C5] gatt on|off|auto|save|stat|clear|rssi <dBm>|try <mac>");
    return true;
  }
  int ns = 0, el = 0, hn = 0, cn = 0;
  xSemaphoreTake(g_lk, portMAX_DELAY);
  const uint32_t t = millis();
  for (int i = 0; i < SEEN_MAX; i++) if (g_seen[i].used) { ns++; if (eligible(g_seen[i], t, false, nullptr)) el++; }
  xSemaphoreGive(g_lk);
  for (int i = 0; i < HIST_MAX; i++) if (g_hist[i].used) hn++;
  for (int i = 0; i < CACHE_MAX; i++) if (g_cache[i].used) cn++;
  Serial.printf("[C5] gatt: mode %s%s, %s (%s), rssi floor %d (prio %d), host hint %s (%lu ms ago), attempts %lu ok %lu, last slot %lu ms, "
                "seen %d (%d eligible), history %d, results %d, state %d%s\n",
                g_mode == 0 ? "off" : g_mode == 1 ? "on" : "auto", g_off ? " (disabled this boot)" : "", enabled(t) ? "ACTIVE" : "idle", g_why,
                (int)floorFor(false), (int)floorFor(true), g_hintOn ? "enabled" : "none/off", (unsigned long)(g_hintAt ? t - g_hintAt : 0),
                (unsigned long)g_attempts, (unsigned long)g_oks, (unsigned long)g_slotMs, ns, el, hn, cn, (int)g_st, g_linkOpen ? ", link open" : "");
  return true;
}

}  // namespace gattid

// phone.cpp — see phone.h.
#include "phone.h"
#include <NimBLEDevice.h>
#include "nimble/nimble/host/src/ble_hs_conn_priv.h"   // bhc_cb: NimBLE offers no way to read a connection's handler
#include "nimble/nimble/host/include/host/ble_hs_pvcy.h"  // ble_hs_set_rpa_timeout
#include "nimble/nimble/host/src/ble_hs_resolv_priv.h"   // peer records, see migrateBonds(); ble_hs_resolv_rpa
#include "nimble/nimble/host/src/ble_hs_pvcy_priv.h"     // ble_hs_pvcy_our_irk
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>
#include "devkey.h"
#include "fleet.h"

extern "C" void ble_hs_lock(void);     // host-internal: declared in ble_hs_priv.h, which doesn't build as C++
extern "C" void ble_hs_unlock(void);

namespace {

// Custom 128-bit UUIDs (base 9a1e000X-2b7e-4c1a-9b00-1a2b3c4d5e6f).
const char* SVC_UUID    = "9a1e0000-2b7e-4c1a-9b00-1a2b3c4d5e6f";
const char* TIME_UUID   = "9a1e0001-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // write: epoch secs
const char* GPS_UUID    = "9a1e0002-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // write: "lat,lon"
const char* CMD_UUID    = "9a1e0003-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // write: "1"/"0"/"R"/"L"
const char* STATUS_UUID = "9a1e0004-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // read/notify
const char* LOGDATA_UUID= "9a1e0005-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // notify: log chunks
const char* DETS_UUID   = "9a1e0006-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // notify: live detections
const char* AUTH_UUID   = "9a1e0007-2b7e-4c1a-9b00-1a2b3c4d5e6f";  // read/write: HMAC challenge-response (docs/ble-auth.md)

NimBLECharacteristic* g_status  = nullptr;
NimBLECharacteristic* g_logData = nullptr;
NimBLECharacteristic* g_dets    = nullptr;
NimBLECharacteristic* g_auth    = nullptr;
NimBLEServer*         g_server  = nullptr;
bool     g_connected  = false;

// --- BLE auth state (SEC-H3, docs/ble-auth.md). Written by the NimBLE task, read from loop(). ---
constexpr uint16_t NO_CONN = BLE_HS_CONN_HANDLE_NONE;
constexpr uint32_t AUTH_TIMEOUT_MS  = 30000;     // an unauthenticated connection is dropped after this
constexpr int      AUTH_MAX_TRIES   = 3;         // bad tries per connection before it is kicked
constexpr int      FAIL_RING        = 6;         // this many failures ...
constexpr uint32_t FAIL_WINDOW_MS   = 600000;    // ... within 10 min -> lockout
constexpr uint32_t LOCK_BASE_MS     = 60000;     // first lockout; doubles per further lockout
constexpr uint32_t LOCK_MAX_MS      = 960000;    // 16 min cap
volatile uint16_t g_connHandle = NO_CONN;        // the tracked central (one at a time)
volatile uint16_t g_authHandle = NO_CONN;        // == g_connHandle once it proved the key
volatile uint32_t g_connMs = 0;                  // written by the NimBLE task, read by service() on the loop task
int      g_connFails  = 0;
char     g_nonce[33]  = {0};                     // 16 B hex; only while the connection is still locked
char     g_dp[65]     = {0};                     // device proof (HMAC), shown once authed
uint32_t g_failAt[FAIL_RING] = {0};              // RAM-only ring of recent failure times (global, all connections)
int      g_failN = 0, g_failHead = 0;
bool     g_lockActive = false;
uint32_t g_lockUntil  = 0, g_lockMs = 0;
// Kicks are queued by the NimBLE callbacks and executed from loop() (never disconnect inside a callback). A small
// ring so two quick failures (e.g. a stray second central + the tracked one) can't overwrite each other.
constexpr int KICK_Q = 4;
uint16_t g_kickQ[KICK_Q];
int      g_kickN = 0;
portMUX_TYPE g_kickMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_authEpoch  = 0;              // bumped on every auth success and tracked disconnect (phone::authEpoch)
volatile bool     g_authDone   = false;          // authJustCompleted()
// --- Link encryption (Phase B/C, docs/ble-auth.md). Compiled out at PHONE_BLE_SEC 0. ---
constexpr int      kSec = PHONE_BLE_SEC;
constexpr uint32_t SEC_START_DELAY_MS = 250;     // let the app's post-HMAC AUTH read finish before the security request
constexpr uint32_t ENC_TIMEOUT_MS     = 35000;   // sec=2: the SM's own 30 s procedure timeout, plus the grace below
constexpr uint32_t ENC_FAIL_GRACE_MS  = 3000;    // sec=2: time for the app to read pf=1 before the kick
constexpr uint8_t  ENC_MIN_KEY_BYTES  = 16;      // a link negotiated down to a short key (KNOB) does not count
volatile bool     g_linkEnc  = false;            // the tracked connection is encrypted at full key size
volatile bool     g_encFail  = false;            // the last pairing / encryption attempt on it failed
volatile bool     g_secStart = false;            // service() owes this link a security request
uint32_t g_authMs    = 0;                        // millis() of the HMAC success (also re-armed by the sec=2 kick)
ble_gap_event_fn* g_gapCb = nullptr;             // NimBLEServer's own handler, wrapped by guardedGapEvent
uint32_t g_encFailMs = 0;
uint32_t g_epochBase  = 0;   // epoch secs at g_baseMillis
uint32_t g_baseMillis = 0;
bool     g_haveTime   = false;
int      g_tzOffMin   = 0;   // UTC-local offset (min); loaded from NVS at begin(), updated on sync
bool     g_tzKnown    = false;   // the offset came from a phone (now or a saved one), or a leader that knew it
// The phone fix is written by the NimBLE task and read from loop() on the other core: g_gpsMux keeps
// lat / lon / stamp together (see gpsFix()).
portMUX_TYPE g_gpsMux = portMUX_INITIALIZER_UNLOCKED;
float    g_lat = 0, g_lon = 0;
bool     g_haveGps    = false;
uint32_t g_gpsMs      = 0;      // millis() of the last phone GPS write (freshness, see hasGps())
bool     g_download   = false;
bool     g_reload     = false;
bool     g_logReq     = false;
bool     g_scanStart  = false;
bool     g_scanStop   = false;
bool     g_newSess    = false;
bool     g_listReq    = false;
bool     g_fileReq    = false;
char     g_dlFile[100] = {0};   // any SD path (file explorer), not just /logs/
bool     g_delReq     = false;
char     g_delFile[48]= {0};
bool     g_briReq     = false;
int      g_briVal     = 100;
bool     g_ledReq     = false;  // "I:<0-100>" status-LED brightness
int      g_ledVal     = 5;
bool     g_lockReq    = false;
bool     g_lockVal    = false;
bool     g_wlReload   = false;
bool     g_wlList     = false;
bool     g_wlAdd      = false;
char     g_wlLine[96] = {0};
bool     g_wlRem      = false;
int      g_wlIdx      = 0;
bool     g_expReq     = false;   // "T:<mode>[:<minutes>][:<path>]" time-filtered export
int      g_expMode    = 0;
int      g_expArg     = 0;
char     g_expPath[48]= {0};
bool     g_srcReq     = false;
uint8_t  g_srcVal     = 0;
bool     g_prefReq    = false;  // "P:<t><g>" time / GPS source preference
volatile uint8_t g_prefVal = 0;  // (t << 4) | g, one byte so the NimBLE task can't hand loop() a mixed pair
// File explorer: "Z:<op>:<arg>" ops + "U<raw bytes>" upload chunks. The phone waits for each reply
// (an FS= payload) before its next write, so one slot of each is enough; loop() does all the SD work.
volatile bool g_fsReq = false;
char     g_fsCmd[208] = {0};
// Fleet: "M:<op>[:args]" (fleet::handlePhoneCmd). The web app waits for each FLT= reply, so one slot is enough.
volatile bool g_fltReq = false;
char     g_fltCmd[160] = {0};
volatile bool g_upReq = false;
uint8_t  g_upBuf[phone::UPLOAD_CHUNK_MAX];
size_t   g_upLen      = 0;
uint8_t  g_upSeq      = 0;          // the chunk's request tag, echoed in its reply
volatile bool g_upOverrun = false;  // a chunk arrived before loop() took the previous one
uint8_t  g_upOverrunSeq = 0;
bool     g_upSkip = false;          // loop-side: drop the already-queued chunk after an overrun abort
// A new resolvable private address at every boot and every PHONE_RPA_ROTATE_S after; see useRpa(). The self filters
// resolve addresses with the IRK (ownIrk(), the fleet's shared keys), so an old address still in the C5's 30 s table is
// still ours. While a central is connected service() keeps re-arming the timer, so it never fires mid-connection.
// The stack's timer overflows 32 bits above 4294 s.
#ifndef PHONE_RPA_ROTATE_S
#define PHONE_RPA_ROTATE_S 900
#endif
constexpr uint32_t RPA_REARM_MS = 60000;
constexpr uint32_t ADV_CHECK_MS = 5000;
uint32_t g_advCheckMs = 0;
uint32_t g_rpaArmMs   = 0;
bool     g_rpaOn      = false;
uint8_t  g_ownIrk[16];                // ownIrk()'s cache; dropped when it stops resolving our address (bonds clear + host reset)
bool     g_ownIrkHave = false;
uint8_t  g_ownMac[6]  = {0};
bool     g_haveOwnMac = false;
uint8_t  g_peerMac[6] = {0};
bool     g_havePeer   = false;   // set ONLY once the connection authenticated (a hostile central must not hide itself from scans)

// ---- auth helpers ----
void queueKick(uint16_t h) {
  portENTER_CRITICAL(&g_kickMux);
  bool have = false;
  for (int i = 0; i < g_kickN; i++) if (g_kickQ[i] == h) have = true;
  if (!have && g_kickN < KICK_Q) g_kickQ[g_kickN++] = h;
  portEXIT_CRITICAL(&g_kickMux);
}
bool popKick(uint16_t* h) {
  portENTER_CRITICAL(&g_kickMux);
  bool any = g_kickN > 0;
  if (any) { *h = g_kickQ[0]; for (int i = 1; i < g_kickN; i++) g_kickQ[i - 1] = g_kickQ[i]; g_kickN--; }
  portEXIT_CRITICAL(&g_kickMux);
  return any;
}

void hexOf(const uint8_t* in, size_t n, char* out) {  // lowercase hex + NUL
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = kHex[in[i] >> 4]; out[2 * i + 1] = kHex[in[i] & 15]; }
  out[2 * n] = 0;
}
bool unhex(const char* in, size_t n, uint8_t* out) {  // n hex chars (either case) -> n/2 bytes
  for (size_t i = 0; i < n; i += 2) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      char c = in[i + k];
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + d;
    }
    out[i / 2] = (uint8_t)v;
  }
  return true;
}
void newNonce() {
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  hexOf(r, sizeof(r), g_nonce);
}
// HMAC(label|sid|n|cn[|sec]) -> out. All ASCII, lowercase hex; distinct labels stop a reflected proof being
// reused. sec >= 0 binds the security level into the proof, so a relay can't report a lower one to the app.
bool authMac(const char* label, const char* cnHex, uint8_t out[32], int sec = -1) {
  char msg[96];
  int n = snprintf(msg, sizeof(msg), "%s|%s|%s|%s", label, devkey::sid(), g_nonce, cnHex);
  if (sec >= 0 && n > 0 && n < (int)sizeof(msg)) n += snprintf(msg + n, sizeof(msg) - n, "|%d", sec);
  if (n <= 0 || n >= (int)sizeof(msg)) { memset(out, 0, 32); return false; }
  return devkey::hmac((const uint8_t*)msg, (size_t)n, out);
}
// Key tag for the AUTH read: HMAC(key, "CFLO-KID-1|" + n), first 8 bytes as hex. The app tries its stored keys
// against it to learn which scanner this is, because the read no longer names it. A fresh nonce gives a fresh tag, so
// strangers can't link two connections to one unit.
bool keyTag(char out[17]) {
  char msg[48];
  int n = snprintf(msg, sizeof(msg), "CFLO-KID-1|%s", g_nonce);
  uint8_t t[32];
  if (n <= 0 || n >= (int)sizeof(msg) || !devkey::hmac((const uint8_t*)msg, (size_t)n, t)) return false;
  hexOf(t, 8, out);
  return true;
}
uint32_t lockRemainingS() {
  if (!g_lockActive) return 0;
  int32_t rem = (int32_t)(g_lockUntil - millis());
  if (rem <= 0) { g_lockActive = false; return 0; }
  return (uint32_t)(rem + 999) / 1000;
}
// A failed attempt (any connection): start a lockout when FAIL_RING failures land inside FAIL_WINDOW_MS.
void recordFail() {
  uint32_t now = millis();
  g_failAt[g_failHead] = now;
  g_failHead = (g_failHead + 1) % FAIL_RING;
  if (g_failN < FAIL_RING) g_failN++;
  if (g_failN == FAIL_RING && now - g_failAt[g_failHead] <= FAIL_WINDOW_MS) {  // head = the oldest entry
    g_lockMs = g_lockMs ? (g_lockMs * 2 > LOCK_MAX_MS ? LOCK_MAX_MS : g_lockMs * 2) : LOCK_BASE_MS;
    g_lockUntil = now + g_lockMs;
    g_lockActive = true;
    g_failN = 0; g_failHead = 0;
    Serial.printf("[phone] auth lockout %lu s\n", (unsigned long)(g_lockMs / 1000));
  }
}
// THE gate: the handle that may send and receive data. HMAC proven, and at sec=2 also encrypted.
// (g_authHandle alone means "proved the key"; the AUTH state machine and the auth timeout use that.)
// g_linkEnc is read FIRST: the void in onAuthenticationComplete clears g_authHandle before it sets g_linkEnc, so
// seeing the flag set guarantees the handle read after it is no longer a proof made in the clear.
uint16_t gateHandle() {
  if (kSec >= 2 && !g_linkEnc) return NO_CONN;
  return g_authHandle;
}
bool isAuthed(const NimBLEConnInfo& ci) {
  uint16_t h = gateHandle();
  return h != NO_CONN && ci.getConnHandle() == h;
}
// Bonding is OFF except between an HMAC success and the end of that link's pairing: any central can start
// pairing, so with it always on a stranger could fill the bond store and evict the owner's phone. Pairing
// without it still encrypts, it just stores nothing. Called on the NimBLE host task only, which is also the
// task the SM reads this from. A pairing latches the flag when it starts (ble_sm.c), so clearing it in
// onAuthenticationComplete can't lose the keys.
// The keys are gated the same way: NimBLE hands its IRK and public identity address to any central that asks during
// pairing, bonded or not, and those would let a stranger follow the unit past its private address.
void setBonding(bool on) {
  if (kSec < 1) return;
  NimBLEDevice::setSecurityAuth(on, /*mitm=*/false, /*sc=*/true);
  const uint8_t dist = on ? (BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID) : 0;
  NimBLEDevice::setSecurityInitKey(dist);
  NimBLEDevice::setSecurityRespKey(dist);
}
void resetLinkSec() {
  g_linkEnc = false;
  g_encFail = false;
  g_secStart = false;
}
// NimBLEServer answers a repeat pairing by deleting the existing bond, and the SM asks before any HMAC, so a
// central replaying the owner's address could wipe the bond. Only a link that proved the key may do that.
int guardedGapEvent(struct ble_gap_event* e, void* arg) {
  if (e->type == BLE_GAP_EVENT_REPEAT_PAIRING && e->repeat_pairing.conn_handle != g_authHandle)
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  return g_gapCb(e, arg);
}
void guardConnection(uint16_t h) {
  if (kSec < 1) return;
  ble_hs_lock();
  struct ble_hs_conn* conn = ble_hs_conn_find(h);
  if (conn && conn->bhc_cb != guardedGapEvent) {
    g_gapCb = conn->bhc_cb;
    conn->bhc_cb = guardedGapEvent;
  }
  ble_hs_unlock();
}

// AUTH read value for connection `h` (docs/ble-auth.md "Read value"): rebuilt on every read so the nonce,
// fail count and lockout wait always describe THAT reader.
// Bounded append: a truncated snprintf parks n at cap (sticky), so a long unit name can't overrun the buffer.
void appf(char* b, size_t cap, size_t& n, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
void appf(char* b, size_t cap, size_t& n, const char* fmt, ...) {
  if (n >= cap) return;
  va_list ap; va_start(ap, fmt);
  int w = vsnprintf(b + n, cap - n, fmt, ap);
  va_end(ap);
  n = (w < 0 || n + (size_t)w >= cap) ? cap : n + (size_t)w;
}
size_t buildAuthValue(uint16_t h, char* b, size_t cap) {
  int st = (h != NO_CONN && h == g_authHandle) ? 1 : (lockRemainingS() > 0 ? 2 : 0);
  size_t n = 0;
  // v=2 never names the unit: the app knows it from the key that fits, and STATUS carries the rest after auth. The
  // value also stays in the attribute, where a Read Blob (offset > 0) returns it without calling onRead, so nothing
  // that identifies the unit may go in it. fw / sec only after auth, so they can't tell the owner's units apart.
  appf(b, cap, n, "v=2;st=%d", st);
  if (st == 1) appf(b, cap, n, ";fw=%s;sec=%d", devkey::fwVersion(), PHONE_BLE_SEC);
  char kt[17];
  if (st == 0 && h == g_connHandle && g_nonce[0] && keyTag(kt)) appf(b, cap, n, ";n=%s;kt=%s", g_nonce, kt);
  if (st == 0 && h == g_connHandle && g_connFails) appf(b, cap, n, ";f=%d", g_connFails);
  if (kSec >= 1 && st == 0 && h == g_connHandle) appf(b, cap, n, ";enc=%d", g_linkEnc ? 1 : 0);
  if (st == 2) appf(b, cap, n, ";wait=%lu", (unsigned long)lockRemainingS());
  if (st == 1) appf(b, cap, n, ";dp=%s;enc=%d%s", g_dp, g_linkEnc ? 1 : 0, g_encFail ? ";pf=1" : "");
  return n < cap ? n : 0;
}

const char AUTH_INIT[] = "v=2;st=0";
void resetAuthValue() { if (g_auth) g_auth->setValue((const uint8_t*)AUTH_INIT, sizeof(AUTH_INIT) - 1); }

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& ci) override {
    std::string mac = ci.getAddress().toString();  // connected phone's BLE address
    Serial.printf("[phone] connected (peer %s)\n", mac.c_str());
    guardConnection(ci.getConnHandle());
    resetAuthValue();                    // a Read Blob would return the previous connection's value
    if (g_rpaOn) { ble_hs_set_rpa_timeout(PHONE_RPA_ROTATE_S); g_rpaArmMs = millis(); }   // no new address under it, even if due now
    if (g_connHandle != NO_CONN) {  // a second central while one is tracked: the first keeps the slot, drop the newcomer
      queueKick(ci.getConnHandle());
      return;
    }
    g_connected = true;
    g_havePeer = false;                  // the peer address is only trusted (and filtered from scans) after auth
    g_authHandle = NO_CONN;
    g_connFails  = 0;
    g_dp[0]      = 0;
    g_connMs     = millis();
    resetLinkSec();
    setBonding(false);
    newNonce();
    g_connHandle = ci.getConnHandle();   // last: marks the slot taken once the state above is in place
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& ci, int) override {
    if (ci.getConnHandle() == g_connHandle) {   // (a kicked second central leaves the tracked state alone)
      g_connected  = false;
      g_connHandle = NO_CONN;
      g_authHandle = NO_CONN;
      g_nonce[0]   = 0;
      g_dp[0]      = 0;
      g_connFails  = 0;
      resetLinkSec();
      setBonding(false);
      g_download   = false;  // drop download mode if the phone leaves
      g_havePeer   = false;  // stop filtering the (now gone) phone
      g_authEpoch  = g_authEpoch + 1;   // in-flight transfers to this peer must stop
      // Nothing queued by this peer may stream to the NEXT one: drop pending transfer requests.
      g_logReq = g_fileReq = g_expReq = g_listReq = g_wlList = false;
      g_fsReq = false; g_upReq = false; g_upOverrun = false;
      g_fltReq = false;
    }
    Serial.println("[phone] disconnected");
    if (g_connHandle == NO_CONN) {       // a kicked second central leaves the owner's link (and its pairing window) alone
      resetAuthValue();
      NimBLEDevice::getAdvertising()->start();  // keep discoverable
    }
  }
  // Fires for a fresh pairing, a bonded phone's re-encryption, and a failure of either. A returning phone may
  // encrypt before it proves the key: that only sets g_linkEnc, the HMAC is still required.
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    if (kSec < 1 || ci.getConnHandle() != g_connHandle) return;
    bool ok = ci.isEncrypted() && ci.getSecKeySize() >= ENC_MIN_KEY_BYTES;
    setBonding(false);
    g_secStart = false;
    if (ok) {
      // sec=2: a proof made in the clear doesn't count. Whoever just paired may have hijacked the link after
      // it, so the phone proves the key again over the encrypted link. g_authHandle clears BEFORE g_linkEnc
      // sets, or gateHandle() would open for that instant.
      if (kSec >= 2 && g_authHandle != NO_CONN && !g_linkEnc) {
        g_connMs     = millis();   // before the handle clears: service() must not time this out on the old stamp
        g_authHandle = NO_CONN;
        g_authEpoch  = g_authEpoch + 1;
        g_dp[0]      = 0;
        g_connFails  = 0;
        newNonce();
      }
      g_linkEnc = true;
      g_encFail = false;
    } else {
      g_linkEnc = false;
      g_encFailMs = millis();
      g_encFail = true;
    }
    Serial.printf("[phone] link security: enc=%d key=%u bonded=%d\n", ci.isEncrypted() ? 1 : 0,
                  (unsigned)ci.getSecKeySize(), ci.isBonded() ? 1 : 0);
#ifdef PHONE_DEBUG_STACK
    Serial.printf("[phone] NimBLE host task stack high-water after pairing: %u words free\n",
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));   // LESC's P-256 runs on this task
#endif
  }
};

void setAuthValue(NimBLECharacteristic* c, uint16_t h) {
  static char b[300];   // NimBLE host task only (onRead / onWrite) -- keeps it off that task's small stack
  size_t n = buildAuthValue(h, b, sizeof(b));
  if (n) c->setValue((const uint8_t*)b, n);
}

// AUTH characteristic: read = state / nonce, write = "H:<cn 32hex>:<mac 64hex>". The value is refreshed
// both on every read and at the end of every write, so the app's immediate re-read after its
// write-with-response sees the new state (st=1 + dp, or the fresh nonce) without waiting for loop().
class AuthCB : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& ci) override { setAuthValue(c, ci.getConnHandle()); }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    uint16_t h = ci.getConnHandle();
    if (h != g_connHandle || g_authHandle != NO_CONN) return;  // not the tracked central, or already authed
    if (g_connFails >= AUTH_MAX_TRIES) return;   // already out of tries (kick pending): no more guesses
    if (!g_nonce[0]) { newNonce(); setAuthValue(c, h); return; }   // no challenge pending: reject
    if (lockRemainingS() > 0) {
      Serial.println("[phone] auth refused: locked out");
      setAuthValue(c, h);
      queueKick(h);
      return;
    }
    std::string val = c->getValue();
    // Strict shape: exactly 99 bytes "H:" + 32 hex + ":" + 64 hex.
    uint8_t cn[16], mac[32];
    bool ok = val.size() == 99 && val[0] == 'H' && val[1] == ':' && val[34] == ':' &&
              unhex(val.c_str() + 2, 32, cn) && unhex(val.c_str() + 35, 64, mac);
    if (ok) {
      char cnHex[33];
      hexOf(cn, sizeof(cn), cnHex);
      uint8_t want[32];
      if (!authMac("CFLO-AUTH-1", cnHex, want)) {   // crypto failure: reject, but it is not the peer's fault (no lockout count)
        Serial.println("[phone] auth: HMAC failed");
        newNonce(); setAuthValue(c, h);
        return;
      }
      ok = devkey::ctEqual(want, mac, sizeof(want));
      if (ok) {
        // Device proof: lets the app check THIS unit, and its security level, before it sends position.
        if (!(kSec >= 1 ? authMac("CFLO-DEV-2", cnHex, want, kSec) : authMac("CFLO-DEV-1", cnHex, want))) {
          Serial.println("[phone] auth: HMAC failed");
          newNonce(); setAuthValue(c, h);
          return;
        }
        hexOf(want, sizeof(want), g_dp);
      }
    }
    if (ok) {
      unsigned pb[6];   // the authenticated phone's BLE address: now (and only now) dropped from scan results
      std::string pm = ci.getAddress().toString();
      if (sscanf(pm.c_str(), "%x:%x:%x:%x:%x:%x", &pb[0], &pb[1], &pb[2], &pb[3], &pb[4], &pb[5]) == 6) {
        for (int i = 0; i < 6; i++) g_peerMac[i] = (uint8_t)pb[i];
        g_havePeer = true;
      }
      g_authEpoch = g_authEpoch + 1;
      g_nonce[0] = 0;                        // single use
      g_connFails = 0;
      g_failN = 0; g_failHead = 0; g_lockMs = 0; g_lockActive = false;
      g_authMs = millis();
      g_encFail = false;                     // a pre-auth failure (e.g. a stale phone bond) gets a fresh try below
      bool wantEnc = kSec >= 1 && !g_linkEnc;
      if (wantEnc) setBonding(true);
      g_authHandle = h;
      g_secStart = wantEnc;                  // after g_authHandle: service() drops a request with no handle
      if (kSec < 2 || g_linkEnc) g_authDone = true;   // loop() pushes STATUS (sec=2: once the gate is open)
      setAuthValue(c, h);
      Serial.println("[phone] authenticated");
#ifdef PHONE_DEBUG_STACK
      Serial.printf("[phone] NimBLE host task stack high-water: %u words free\n",
                    (unsigned)uxTaskGetStackHighWaterMark(nullptr));   // runs on the host task: size its stack from this
#endif
      return;
    }
    g_connFails++;
    recordFail();
    newNonce();                              // the failed challenge is dead
    setAuthValue(c, h);
    Serial.printf("[phone] auth FAILED (%d/%d)\n", g_connFails, AUTH_MAX_TRIES);
    if (g_connFails >= AUTH_MAX_TRIES || lockRemainingS() > 0) queueKick(h);
  }
};

// STATUS: when the authenticated app subscribes, ask loop() for a STATUS push so its first status isn't
// missed (the app subscribes only after auth, i.e. after the success push already went out to nobody).
class StatusCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& ci, uint16_t subValue) override {
    if (subValue && isAuthed(ci)) g_authDone = true;
  }
};

class WriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (!isAuthed(ci)) {   // TIME / GPS / CMD do nothing until the connection proved the device key
      static uint32_t lastLog = 0;
      if (millis() - lastLog > 2000) { lastLog = millis(); Serial.println("[phone] write ignored: not authenticated"); }
      return;
    }
    std::string uuid = c->getUUID().toString();
    std::string val  = c->getValue();
    if (uuid == TIME_UUID) {
      // "utcEpoch;tzOffsetMinutes" (offset = UTC-local, as JS getTimezoneOffset()).
      // Store the LOCAL epoch so the display reads local time. Bare "epoch" also works.
      long utc = 0, offMin = 0;
      int n = sscanf(val.c_str(), "%ld;%ld", &utc, &offMin);
      if (n >= 1 && utc > 1600000000L) {  // sanity: after 2020
        g_epochBase = (uint32_t)(utc - offMin * 60);
        g_baseMillis = millis();
        g_haveTime = true;
        if (n >= 2) g_tzKnown = true;
        if (n >= 2 && (int)offMin != g_tzOffMin) {   // remember the tz so a phone-less GPS boot reads local
          g_tzOffMin = (int)offMin;
          Preferences p;
          if (p.begin("cydscan", false)) { p.putInt("tzoff", g_tzOffMin); p.end(); }
        }
        Serial.printf("[phone] time sync: utc=%ld offMin=%ld -> local base %lu\n",
                      utc, offMin, (unsigned long)g_epochBase);
      }
    } else if (uuid == GPS_UUID) {
      float la, lo;
      // Reject nan/inf/out-of-range so the NDJSON log never gets a non-JSON lat/lon.
      if (sscanf(val.c_str(), "%f,%f", &la, &lo) == 2 && isfinite(la) && isfinite(lo) &&
          la >= -90.0f && la <= 90.0f && lo >= -180.0f && lo <= 180.0f) {
        uint32_t now = millis();
        portENTER_CRITICAL(&g_gpsMux);
        g_lat = la; g_lon = lo; g_haveGps = true; g_gpsMs = now;
        portEXIT_CRITICAL(&g_gpsMux);
        Serial.printf("[phone] gps: %.5f,%.5f\n", la, lo);
      }
    } else if (uuid == CMD_UUID) {
      char c0 = val.empty() ? 0 : val[0];
      if (c0 == 'R' || c0 == 'r') {
        g_reload = true;
        Serial.println("[phone] cmd reload signature DB");
      } else if (c0 == 'L' || c0 == 'l') {
        g_logReq = true;
        Serial.println("[phone] cmd BLE log download");
      } else if (c0 == 'G' || c0 == 'g') {
        g_scanStart = true;
        Serial.println("[phone] cmd start scan");
      } else if (c0 == 'X' || c0 == 'x') {
        g_scanStop = true;
        Serial.println("[phone] cmd stop scan");
      } else if (c0 == 'N' || c0 == 'n') {
        g_newSess = true;
        Serial.println("[phone] cmd new session");
      } else if (c0 == 'Q' || c0 == 'q') {
        g_listReq = true;
        Serial.println("[phone] cmd list sessions");
      } else if (c0 == 'F' || c0 == 'f') {           // "F:<path>" download a file
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          strncpy(g_dlFile, val.c_str() + colon + 1, sizeof(g_dlFile) - 1);
          g_dlFile[sizeof(g_dlFile) - 1] = 0;
          g_fileReq = true;
          Serial.printf("[phone] cmd download file %s\n", g_dlFile);
        }
      } else if (c0 == 'D' || c0 == 'd') {           // "D:<path>" delete a session file
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          strncpy(g_delFile, val.c_str() + colon + 1, sizeof(g_delFile) - 1);
          g_delFile[sizeof(g_delFile) - 1] = 0;
          g_delReq = true;
          Serial.printf("[phone] cmd delete file %s\n", g_delFile);
        }
      } else if (c0 == 'B' || c0 == 'b') {           // "B:<0-100>" brightness
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          g_briVal = atoi(val.c_str() + colon + 1);
          g_briReq = true;
          Serial.printf("[phone] cmd brightness %d\n", g_briVal);
        }
      } else if (c0 == 'I' || c0 == 'i') {           // "I:<0-100>" status-LED brightness ('L' is the log download)
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          g_ledVal = atoi(val.c_str() + colon + 1);
          g_ledReq = true;
          Serial.printf("[phone] cmd LED brightness %d\n", g_ledVal);
        }
      } else if (c0 == 'Z') {                        // "Z<seq>:<op>:<arg>" file-explorer op (see host_files.cpp)
        if (val.size() > 3 && !g_fsReq) {
          strncpy(g_fsCmd, val.c_str() + 1, sizeof(g_fsCmd) - 1);
          g_fsCmd[sizeof(g_fsCmd) - 1] = 0;
          g_fsReq = true;
          Serial.printf("[phone] cmd file op %.40s\n", g_fsCmd);
        }
      } else if (c0 == 'M') {                        // "M:<op>[:args]" fleet command (fleet::handlePhoneCmd)
        if (val.size() > 2 && val.size() < sizeof(g_fltCmd) && !g_fltReq) {
          memcpy(g_fltCmd, val.c_str(), val.size());
          g_fltCmd[val.size()] = 0;
          g_fltReq = true;
          Serial.printf("[phone] cmd fleet %.3s\n", g_fltCmd);   // args may carry wrapped key material: not logged
        }
      } else if (c0 == 'U') {                        // "U<seq byte><raw bytes>" one upload chunk (binary-safe: uses size())
        if (val.size() >= 2) {
          size_t n = val.size() - 2;
          if (g_upReq || n > sizeof(g_upBuf)) { g_upOverrunSeq = (uint8_t)val[1]; g_upOverrun = true; }
          else { memcpy(g_upBuf, val.data() + 2, n); g_upLen = n; g_upSeq = (uint8_t)val[1]; g_upReq = true; }
        }
      } else if (c0 == 'K' || c0 == 'k') {           // "K1"/"K0" lock / unlock the CYD display
        g_lockVal = (val.size() > 1 && val[1] == '1');
        g_lockReq = true;
        Serial.printf("[phone] cmd display %s\n", g_lockVal ? "LOCK" : "unlock");
      } else if (c0 == 'S' || c0 == 's') {           // "S:<n>" scan-source mask
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          g_srcVal = (uint8_t)atoi(val.c_str() + colon + 1);
          g_srcReq = true;
          Serial.printf("[phone] cmd src mask %u\n", g_srcVal);
        }
      } else if (c0 == 'P' || c0 == 'p') {           // "P:<t><g>" time / GPS source pref, digits 0 prefer phone / 1 prefer device / 2 device only
        if (val.size() == 4 && val[1] == ':' && val[2] >= '0' && val[2] <= '2' && val[3] >= '0' && val[3] <= '2') {
          g_prefVal = (uint8_t)(((val[2] - '0') << 4) | (val[3] - '0'));
          g_prefReq = true;
          Serial.printf("[phone] cmd source pref time=%c gps=%c\n", val[2], val[3]);
        }
      } else if (c0 == 'T' || c0 == 't') {           // "T:<mode>[:<minutes>][:<path>]" filtered export
        // mode 0 = whole file, 1 = past 24 h, 2 = past <minutes>. Optional /logs/ path (default =
        // the current session), or "*" = every log file. ('E' is taken by whitelist-remove, hence 'T'.)
        if (val.size() > 2 && val[1] == ':') {
          const char* s = val.c_str();
          g_expMode = atoi(s + 2);
          g_expArg = 0; g_expPath[0] = 0;
          for (const char* q = strchr(s + 2, ':'); q; ) {
            q++;
            if (*q == '/' || *q == '*') { strncpy(g_expPath, q, sizeof(g_expPath) - 1); g_expPath[sizeof(g_expPath) - 1] = 0; break; }
            if (*q >= '0' && *q <= '9') g_expArg = atoi(q);
            q = strchr(q, ':');
          }
          g_expReq = true;
          Serial.printf("[phone] cmd filtered export mode=%d arg=%d path=%s\n", g_expMode, g_expArg, g_expPath);
        }
      } else if (c0 == 'W' || c0 == 'w') {           // reload whitelist
        g_wlReload = true;
        Serial.println("[phone] cmd reload whitelist");
      } else if (c0 == 'Y' || c0 == 'y') {           // list whitelist
        g_wlList = true;
        Serial.println("[phone] cmd list whitelist");
      } else if (c0 == 'A' || c0 == 'a') {           // "A:<csv rule line>" add a rule
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          strncpy(g_wlLine, val.c_str() + colon + 1, sizeof(g_wlLine) - 1);
          g_wlLine[sizeof(g_wlLine) - 1] = 0;
          g_wlAdd = true;
          Serial.printf("[phone] cmd whitelist add %s\n", g_wlLine);
        }
      } else if (c0 == 'E' || c0 == 'e') {           // "E:<index>" remove the Nth rule
        size_t colon = val.find(':');
        if (colon != std::string::npos) {
          g_wlIdx = atoi(val.c_str() + colon + 1);
          g_wlRem = true;
          Serial.printf("[phone] cmd whitelist remove %d\n", g_wlIdx);
        }
      } else {
        g_download = (c0 == '1');
        Serial.printf("[phone] cmd download=%d\n", g_download);
      }
    }
  }
};

ServerCB g_serverCB;
WriteCB  g_writeCB;
AuthCB   g_authCB;
StatusCB g_statusCB;

}  // namespace

namespace phone {

void loadTz() {
  Preferences p;                         // restore the last known tz (for phone-less GPS local time)
  if (p.begin("cydscan", true)) { g_tzKnown = p.isKey("tzoff"); g_tzOffMin = p.getInt("tzoff", 0); p.end(); }
}

// A fleet client learns the tz from its leader's Sync (it has no phone of its own). NVS is written only on change.
void setTzOffsetMin(int offMin) {
  if (offMin < -840 || offMin > 840) return;
  g_tzKnown = true;
  if (offMin == g_tzOffMin) return;
  g_tzOffMin = offMin;
  Preferences p;
  if (p.begin("cydscan", false)) { p.putInt("tzoff", offMin); p.end(); }
}

// Hides the fixed public address from anyone listening. A bonded phone holds this unit's IRK, so it still recognises
// the unit; pairing sends the public address as the identity, inside the encrypted link. The IRK lives in NimBLE's NVS
// store. If privacy won't start, stay on the public address: a working phone link matters more.
void useRpa() {
  ble_hs_set_rpa_timeout(PHONE_RPA_ROTATE_S);
  g_rpaArmMs = millis();
  g_rpaOn = NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT);
  if (g_rpaOn) return;
  Serial.println("[phone] private address failed, using the public one");
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
}

// Host privacy finds a phone's rotating address through peer records. S3 builds before fw 0.32.0 ran without it and
// never wrote any, so their bonds would stop resolving: the LTK lookup fails and repairing the bond fails too. Build the
// missing records from the stored bonds, the way ble_sm.c does at pairing. Runs before advertising starts.
int migrateBond(int, union ble_store_value* val, void* added) {
  const ble_store_value_sec& sec = val->sec;
  uint8_t id[6];
  memcpy(id, sec.peer_addr.val, 6);
  if (!sec.irk_present || ble_rpa_find_peer_dev_rec(id)) return 0;
  if (ble_rpa_get_num_peer_dev_records() > MYNEWT_VAL(BLE_STORE_MAX_BONDS)) return 0;
  ble_rpa_resolv_add_peer_rec(id);
  ble_hs_dev_records* rec = ble_rpa_find_peer_dev_rec(id);
  if (!rec) return 0;
  for (int i = 0; i < 16; i++) rec->peer_sec.irk[i] = sec.irk[15 - i];
  rec->peer_sec.irk_present = 1;
  rec->peer_sec.peer_addr = sec.peer_addr;
  (*(int*)added)++;
  return 0;
}
void migrateBonds() {
  int added = 0;
  ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, migrateBond, &added);
  if (!added) return;
  ble_store_persist_peer_records();
  Serial.printf("[phone] migrated %d bond(s) for private addresses\n", added);
}

// Reads the address now in use (the C5 reports the same MSB-first text form).
bool readOwnMac(uint8_t out[6]) {
  std::string mac = NimBLEDevice::getAddress().toString();
  unsigned b[6];
  if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) return false;   // none during a host reset
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
  return true;
}

// Every unit advertises the same name, so the advert doesn't say which unit this is (the service UUID still says what
// it is). The unit's own name reaches the phone in STATUS, after authentication.
const char* ADV_NAME = "CFLO";

void begin(const char* devName) {
  loadTz();
  NimBLEDevice::init(ADV_NAME);           // also the GAP Device Name, readable by any central
  devkey::begin();                       // phone-link key (NVS), after init so the RNG is radio-fed
  if (kSec >= 1) {                       // just-works: the HMAC authenticates, this layer only encrypts
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    setBonding(false);
    ble_hs_cfg.sm_sc_only = 1;           // refuse legacy pairing: its just-works key falls to a passive sniffer
  }
  migrateBonds();
  useRpa();
  NimBLEServer* server = NimBLEDevice::createServer();
  g_server = server;
  server->setCallbacks(&g_serverCB);

  NimBLEService* svc = server->createService(SVC_UUID);
  svc->createCharacteristic(TIME_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(&g_writeCB);
  svc->createCharacteristic(GPS_UUID,  NIMBLE_PROPERTY::WRITE)->setCallbacks(&g_writeCB);
  svc->createCharacteristic(CMD_UUID,  NIMBLE_PROPERTY::WRITE)->setCallbacks(&g_writeCB);
  g_status = svc->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_logData = svc->createCharacteristic(LOGDATA_UUID, NIMBLE_PROPERTY::NOTIFY);
  g_dets    = svc->createCharacteristic(DETS_UUID, NIMBLE_PROPERTY::NOTIFY);
  g_auth    = svc->createCharacteristic(AUTH_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  g_auth->setCallbacks(&g_authCB);       // never ENC: the HMAC is the gate. Its presence tells the app this is secured firmware.
  // The attribute never carries live data: STATUS only goes out by notify to the authenticated connection.
  g_status->setValue("auth=0");
  g_status->setCallbacks(&g_statusCB);
  resetAuthValue();
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);       // service UUID in the adv packet (filterable)
  adv->setName(ADV_NAME);
  // The 128-bit service UUID fills the adv packet, so put the name in an explicit
  // scan response so Chrome shows "CFLO" instead of "Unknown device".
  NimBLEAdvertisementData scanResp;
  scanResp.setName(ADV_NAME);
  adv->setScanResponseData(scanResp);
  adv->enableScanResponse(true);
  adv->start();

  // Cache our own BLE address so the scanner can drop its own advertisement.
  g_haveOwnMac = readOwnMac(g_ownMac);
  Serial.printf("[phone] BLE advertising as '%s' for %s (own MAC %s)\n", ADV_NAME, devName,
                NimBLEDevice::getAddress().toString().c_str());
}

bool ownMac(uint8_t out[6]) {
  if (!g_haveOwnMac) return false;
  memcpy(out, g_ownMac, 6);
  return true;
}

bool rpaMatch(const uint8_t mac[6], const uint8_t irk[16]) {
  if ((mac[0] & 0xC0) != 0x40) return false;           // not a resolvable private address
  uint8_t v[6], k[16];
  for (int i = 0; i < 6; i++) v[i] = mac[5 - i];        // NimBLE keeps addresses LSB-first
  memcpy(k, irk, 16);
  return ble_hs_resolv_rpa(v, k) == 0;                 // all-zero key: EINVAL
}

// NimBLE's key, in whichever byte order resolves our own address, so the fleet's keys all have the form rpaMatch()
// takes. Found once (the key lives in NVS for the unit's life); until then, e.g. while a host reset left a static
// address, false.
bool ownIrk(uint8_t out[16]) {
  if (!g_ownIrkHave) {
    const uint8_t* p = nullptr;
    if (!g_rpaOn || !g_haveOwnMac || ble_hs_pvcy_our_irk(&p) != 0 || !p) return false;
    uint8_t a[16], b[16];
    for (int i = 0; i < 16; i++) { a[i] = p[i]; b[i] = p[15 - i]; }
    if (rpaMatch(g_ownMac, a)) memcpy(g_ownIrk, a, 16);
    else if (rpaMatch(g_ownMac, b)) memcpy(g_ownIrk, b, 16);
    else return false;
    g_ownIrkHave = true;
    Serial.println("[phone] BLE key checked against our own address");
  }
  memcpy(out, g_ownIrk, 16);
  return true;
}

bool peerMac(uint8_t out[6]) {
  if (!g_havePeer) return false;
  memcpy(out, g_peerMac, 6);
  return true;
}

void setStatus(const String& s) {
  uint16_t h = gateHandle();   // snapshot: NONE would make NimBLE broadcast to every peer
  if (!g_status || h == NO_CONN || !s.length()) return;
  // Send THIS value explicitly. A bare notify() only schedules one (ble_gatts_chr_updated) and the stack
  // reads the attribute when it transmits -- so pushStatus()'s two back-to-back parts collapsed into
  // part 2 twice and part 1 (link/time/gps/scan/sess…) never reached the phone. It also keeps the live
  // STATUS (SoftAP creds…) out of the attribute, where an unauthenticated central could read it.
  g_status->notify((const uint8_t*)s.c_str(), s.length(), h);
}

bool     connected()        { return g_connected; }
bool     authed()           { return gateHandle() != NO_CONN; }
bool     linkEncrypted()    { return g_authHandle != NO_CONN && g_linkEnc; }
bool     authJustCompleted(){ bool r = g_authDone; g_authDone = false; return r; }

void service() {
  // Re-entrancy: only called from the loop task (also from inside transfer loops via transferAlive), and
  // it touches nothing a transfer is using, so a nested call is harmless.
  uint16_t kh;
  while (popKick(&kh)) if (g_server) g_server->disconnect(kh);
  if (g_server && millis() - g_advCheckMs >= ADV_CHECK_MS) {
    g_advCheckMs = millis();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    const bool idle = g_connHandle == NO_CONN && g_server->getConnectedCount() == 0;
    if (g_rpaOn && !idle && millis() - g_rpaArmMs >= RPA_REARM_MS) {   // no new address under a connected phone
      ble_hs_set_rpa_timeout(PHONE_RPA_ROTATE_S);
      g_rpaArmMs = millis();
    }
    uint8_t m[6];
    if (readOwnMac(m)) {
      if (g_rpaOn && idle && (m[0] & 0xC0) != 0x40) {   // a host reset leaves a static address the phone can't resolve
        adv->stop();
        useRpa();
        readOwnMac(m);
      }
      if (memcmp(m, g_ownMac, 6)) {                     // the self filter needs the new address
        memcpy(g_ownMac, m, 6);
        if (g_ownIrkHave && (m[0] & 0xC0) == 0x40 && !rpaMatch(m, g_ownIrk)) g_ownIrkHave = false;   // a new key: find it again
        Serial.printf("[phone] BLE address changed to %s\n", NimBLEDevice::getAddress().toString().c_str());
      }
    }
    if (idle && !adv->isAdvertising()) adv->start();    // a rotation stops the advert and nothing restarts it
  }
  // A central that never proves the key is dropped (frees the single GATT slot for the owner's phone).
  if (g_connHandle != NO_CONN && g_authHandle == NO_CONN && millis() - g_connMs > AUTH_TIMEOUT_MS) {
    Serial.println("[phone] auth timeout -> disconnect");
    g_connMs = millis();   // re-armed: a slow disconnect isn't re-issued every loop
    if (g_server) g_server->disconnect(g_connHandle);
  }
  if (kSec >= 1 && g_secStart) {
    uint16_t h = g_authHandle;
    if (h == NO_CONN || g_linkEnc) g_secStart = false;
    else if (millis() - g_authMs >= SEC_START_DELAY_MS) {
      g_secStart = false;
      int rc = 0;
      if (!NimBLEDevice::startSecurity(h, &rc)) {
        Serial.printf("[phone] security request failed rc=%d\n", rc);
        g_encFailMs = millis();
        g_encFail = true;
      }
    }
  }
  // sec=2: a link that proved the key but won't encrypt gets no data, so don't let it hold the only slot.
  if (kSec >= 2 && g_authHandle != NO_CONN && !g_linkEnc) {
    bool fail = g_encFail;                // before millis(): a stamp written after `now` would wrap the age
    uint32_t now = millis();
    bool failed = fail && now - g_encFailMs > ENC_FAIL_GRACE_MS;
    if (failed || now - g_authMs > ENC_TIMEOUT_MS) {
      Serial.println(failed ? "[phone] pairing failed -> disconnect" : "[phone] encryption timeout -> disconnect");
      g_authMs = g_encFailMs = now;   // re-armed: a slow disconnect isn't re-issued every loop
      if (g_server) g_server->disconnect(g_authHandle);
    }
  }
}

uint32_t authEpoch() { return g_authEpoch; }
bool transferAlive(uint32_t epoch) {
  service();   // 30 s auth timeout / kicks keep running during a long transfer
  fleet::service();   // a long BLE transfer blocks loop(): keep draining fleet frames (docs/fleet-mode.md §3.4)
  return gateHandle() != NO_CONN && g_authEpoch == epoch;
}

int bondCount() { return g_server ? NimBLEDevice::getNumBonds() : -1; }   // no server = a CYD fleet client (no NimBLE)
int bondRecords() { return g_server ? ble_rpa_get_num_peer_dev_records() : -1; }

bool clearBonds() {
  if (!g_server) return false;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();                                 // NimBLE refuses to unpair a peer with an IRK while advertising
  bool ok = NimBLEDevice::deleteAllBonds();
  g_ownIrkHave = false;                        // that also deletes the stored key; a host reset would make a new one
  if (g_connHandle != NO_CONN) g_server->disconnect(g_connHandle);   // the link's own bond is gone with the rest
  else adv->start();
  return ok;
}

bool rekeyAndKick() {
  bool saved = devkey::rekey();   // also writes the phonecfg audit line
  g_authHandle = NO_CONN;   // the old key's session ends now, before the disconnect lands
  g_authEpoch = g_authEpoch + 1;
  newNonce();               // any challenge handed out under the old key is dead
  if (g_connHandle != NO_CONN && g_server) g_server->disconnect(g_connHandle);
  return saved;
}
bool     hasTime()          { return g_haveTime; }
uint32_t epochNow()         { return g_haveTime ? g_epochBase + (millis() - g_baseMillis) / 1000 : 0; }
void     restoreTime(uint32_t localEpoch) {
  if (localEpoch < 1700000000UL) return;
  g_epochBase = localEpoch; g_baseMillis = millis(); g_haveTime = true;
}
int      tzOffsetMin()      { return g_tzOffMin; }
bool     tzKnown()          { return g_tzKnown; }
// A phone fix is LIVE only while the phone is connected and wrote one within PHONE_GPS_STALE_MS: a
// pocketed (screen-off) Android Chrome page stops watchPosition, so without this its last fix would pin
// every later row. The webapp sends a fix every ~5 s, so 90 s won't flap on a live phone.
bool gpsFix(float* lat, float* lon, uint32_t* atMs) {
  portENTER_CRITICAL(&g_gpsMux);
  bool had = g_haveGps;
  *lat = g_lat; *lon = g_lon; *atMs = g_gpsMs;
  portEXIT_CRITICAL(&g_gpsMux);
  return had;
}
// Stamp read before millis() (via gpsFix) and clamped, so a write landing in between can't wrap the age.
uint32_t gpsAgeMs() {
  float la, lo; uint32_t at;
  if (!gpsFix(&la, &lo, &at)) return UINT32_MAX;
  int32_t age = (int32_t)(millis() - at);
  return age > 0 ? (uint32_t)age : 0;
}
bool     hasGps()           { return authed() && gpsAgeMs() < PHONE_GPS_STALE_MS; }
bool     hadGps()           { return g_haveGps; }
bool     downloadRequested(){ return g_download; }
void     cancelDownload()   { g_download = false; }
bool     reloadRequested()  { bool r = g_reload; g_reload = false; return r; }
bool     logRequested()       { bool r = g_logReq; g_logReq = false; return r; }
bool     scanStartRequested() { bool r = g_scanStart; g_scanStart = false; return r; }
bool     scanStopRequested()  { bool r = g_scanStop; g_scanStop = false; return r; }
bool     newSessionRequested(){ bool r = g_newSess; g_newSess = false; return r; }
bool     listRequested()      { bool r = g_listReq; g_listReq = false; return r; }

bool fileRequested(char* out, size_t cap) {
  if (!g_fileReq) return false;
  g_fileReq = false;
  strncpy(out, g_dlFile, cap - 1);
  out[cap - 1] = 0;
  return true;
}

bool deleteRequested(char* out, size_t cap) {
  if (!g_delReq) return false;
  g_delReq = false;
  strncpy(out, g_delFile, cap - 1);
  out[cap - 1] = 0;
  return true;
}

bool exportRequested(int* mode, int* minutes, char* path, size_t cap) {
  if (!g_expReq) return false;
  g_expReq = false;
  *mode = g_expMode;
  *minutes = g_expArg;
  strncpy(path, g_expPath, cap - 1);
  path[cap - 1] = 0;
  return true;
}

bool wlReloadRequested() { bool r = g_wlReload; g_wlReload = false; return r; }
bool wlListRequested()   { bool r = g_wlList; g_wlList = false; return r; }

bool wlAddRequested(char* out, size_t cap) {
  if (!g_wlAdd) return false;
  g_wlAdd = false;
  strncpy(out, g_wlLine, cap - 1);
  out[cap - 1] = 0;
  return true;
}

bool wlRemoveRequested(int* outIdx) {
  if (!g_wlRem) return false;
  g_wlRem = false;
  *outIdx = g_wlIdx;
  return true;
}

bool srcMaskRequested(uint8_t* out) {
  if (!g_srcReq) return false;
  g_srcReq = false;
  *out = g_srcVal;
  return true;
}

bool sourcePrefRequested(uint8_t* timePref, uint8_t* gpsPref) {
  if (!g_prefReq) return false;
  g_prefReq = false;
  uint8_t v = g_prefVal;
  *timePref = v >> 4; *gpsPref = v & 0x0F;
  return true;
}

bool brightnessRequested(int* outPct) {
  if (!g_briReq) return false;
  g_briReq = false;
  *outPct = g_briVal;
  return true;
}

bool fleetRequested(char* out, size_t cap) {
  if (!g_fltReq) return false;
  strncpy(out, g_fltCmd, cap - 1);
  out[cap - 1] = 0;
  memset(g_fltCmd, 0, sizeof(g_fltCmd));   // an M:a carries a wrapped fleet key: don't keep it in a static
  g_fltReq = false;   // after the copy: the NimBLE task only refills the slot once this clears
  return true;
}

bool fsRequested(char* out, size_t cap) {
  if (!g_fsReq) return false;
  strncpy(out, g_fsCmd, cap - 1);
  out[cap - 1] = 0;
  g_fsReq = false;   // after the copy: the NimBLE task only refills the slot once this clears
  return true;
}

bool uploadChunk(uint8_t* out, size_t cap, size_t* outLen, uint8_t* seq) {
  if (!g_upReq) return false;
  size_t n = g_upLen < cap ? g_upLen : cap;
  memcpy(out, g_upBuf, n);
  *outLen = n;
  *seq = g_upSeq;
  g_upReq = false;
  if (g_upSkip) { g_upSkip = false; return false; }   // the upload was aborted by an overrun: eat this chunk silently
  return true;
}

bool uploadOverrun(uint8_t* seq) {
  if (!g_upOverrun) return false;
  *seq = g_upOverrunSeq;
  g_upOverrun = false;
  // The upload is aborted, so the chunk still queued must not get a 2nd reply -- but only the consumer
  // (uploadChunk) clears g_upReq, so a new chunk can't be lost to a race with the writer.
  if (g_upReq) g_upSkip = true;
  return true;
}

bool ledBrightnessRequested(int* outPct) {
  if (!g_ledReq) return false;
  g_ledReq = false;
  *outPct = g_ledVal;
  return true;
}

bool lockRequested(bool* outLock) {
  if (!g_lockReq) return false;
  g_lockReq = false;
  *outLock = g_lockVal;
  return true;
}

// Explicit-buffer notify to the authenticated connection only (no attribute write, no broadcast to a
// stray central that merely subscribed).
void logNotify(const uint8_t* data, size_t len) {
  uint16_t h = gateHandle();   // snapshot (NONE would broadcast)
  if (!g_logData || h == NO_CONN || !len) return;
  g_logData->notify(data, len, h);
}

void detsNotify(const String& line) {
  uint16_t h = gateHandle();
  if (!g_dets || h == NO_CONN || !line.length()) return;
  g_dets->notify((const uint8_t*)line.c_str(), line.length(), h);
}

}  // namespace phone

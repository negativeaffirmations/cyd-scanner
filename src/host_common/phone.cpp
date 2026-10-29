// phone.cpp — see phone.h.
#include "phone.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>
#include "devkey.h"

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
uint32_t g_connMs     = 0;
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
uint32_t g_epochBase  = 0;   // epoch secs at g_baseMillis
uint32_t g_baseMillis = 0;
bool     g_haveTime   = false;
int      g_tzOffMin   = 0;   // UTC-local offset (min); loaded from NVS at begin(), updated on sync
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
volatile bool g_upReq = false;
uint8_t  g_upBuf[phone::UPLOAD_CHUNK_MAX];
size_t   g_upLen      = 0;
uint8_t  g_upSeq      = 0;          // the chunk's request tag, echoed in its reply
volatile bool g_upOverrun = false;  // a chunk arrived before loop() took the previous one
uint8_t  g_upOverrunSeq = 0;
bool     g_upSkip = false;          // loop-side: drop the already-queued chunk after an overrun abort
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
// HMAC(label|sid|n|cn) -> out. All ASCII, lowercase hex; distinct labels stop a reflected proof being reused.
bool authMac(const char* label, const char* cnHex, uint8_t out[32]) {
  char msg[96];
  int n = snprintf(msg, sizeof(msg), "%s|%s|%s|%s", label, devkey::sid(), g_nonce, cnHex);
  if (n <= 0 || n >= (int)sizeof(msg)) { memset(out, 0, 32); return false; }
  return devkey::hmac((const uint8_t*)msg, (size_t)n, out);
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
bool isAuthed(const NimBLEConnInfo& ci) {
  return g_authHandle != NO_CONN && ci.getConnHandle() == g_authHandle;
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
  appf(b, cap, n, "v=1;sid=%s;name=%s;fw=%s;sec=%d;kg=%lu;st=%d", devkey::sid(), devkey::unitName(),
       devkey::fwVersion(), PHONE_BLE_SEC, (unsigned long)devkey::gen(), st);
  if (st == 0 && h == g_connHandle && g_nonce[0]) appf(b, cap, n, ";n=%s", g_nonce);
  if (st == 0 && h == g_connHandle && g_connFails) appf(b, cap, n, ";f=%d", g_connFails);
  if (st == 2) appf(b, cap, n, ";wait=%lu", (unsigned long)lockRemainingS());
  if (st == 1) appf(b, cap, n, ";dp=%s;enc=0", g_dp);
  return n < cap ? n : 0;
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& ci) override {
    std::string mac = ci.getAddress().toString();  // connected phone's BLE address
    Serial.printf("[phone] connected (peer %s)\n", mac.c_str());
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
      g_download   = false;  // drop download mode if the phone leaves
      g_havePeer   = false;  // stop filtering the (now gone) phone
      g_authEpoch  = g_authEpoch + 1;   // in-flight transfers to this peer must stop
      // Nothing queued by this peer may stream to the NEXT one: drop pending transfer requests.
      g_logReq = g_fileReq = g_expReq = g_listReq = g_wlList = false;
      g_fsReq = false; g_upReq = false; g_upOverrun = false;
    }
    Serial.println("[phone] disconnected");
    NimBLEDevice::getAdvertising()->start();  // keep discoverable
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
        if (!authMac("CFLO-DEV-1", cnHex, want)) {   // device proof: lets the app check THIS unit before it sends position
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
      g_authHandle = h;
      g_authDone = true;                     // loop() pushes STATUS
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

void begin(const char* devName) {
  { Preferences p;                       // restore the last known tz (for phone-less GPS local time)
    if (p.begin("cydscan", true)) { g_tzOffMin = p.getInt("tzoff", 0); p.end(); } }
  NimBLEDevice::init(devName);
  devkey::begin();                       // phone-link key (NVS), after init so the RNG is radio-fed
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
  { static const char kInit[] = "v=1;st=0"; g_auth->setValue((const uint8_t*)kInit, sizeof(kInit) - 1); }
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);       // service UUID in the adv packet (filterable)
  adv->setName(devName);
  // The 128-bit service UUID fills the adv packet, so put the name in an explicit
  // scan response so Chrome shows the unit name (CFLO-<board>-<word>) instead of "Unknown device".
  NimBLEAdvertisementData scanResp;
  scanResp.setName(devName);
  adv->setScanResponseData(scanResp);
  adv->enableScanResponse(true);
  adv->start();

  // Cache our own BLE MAC (same MSB-first text form the C5 parses) so the scanner
  // can drop its own advertisement. Runtime value -> works on any CYD unit.
  std::string mac = NimBLEDevice::getAddress().toString();
  unsigned b[6];
  if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
    for (int i = 0; i < 6; i++) g_ownMac[i] = (uint8_t)b[i];
    g_haveOwnMac = true;
  }
  Serial.printf("[phone] BLE advertising as '%s' (own MAC %s)\n", devName, mac.c_str());
}

bool ownMac(uint8_t out[6]) {
  if (!g_haveOwnMac) return false;
  memcpy(out, g_ownMac, 6);
  return true;
}

bool peerMac(uint8_t out[6]) {
  if (!g_havePeer) return false;
  memcpy(out, g_peerMac, 6);
  return true;
}

void setStatus(const String& s) {
  uint16_t h = g_authHandle;   // snapshot: NONE would make NimBLE broadcast to every peer
  if (!g_status || h == NO_CONN || !s.length()) return;
  // Send THIS value explicitly. A bare notify() only schedules one (ble_gatts_chr_updated) and the stack
  // reads the attribute when it transmits -- so pushStatus()'s two back-to-back parts collapsed into
  // part 2 twice and part 1 (link/time/gps/scan/sess…) never reached the phone. It also keeps the live
  // STATUS (SoftAP creds…) out of the attribute, where an unauthenticated central could read it.
  g_status->notify((const uint8_t*)s.c_str(), s.length(), h);
}

bool     connected()        { return g_connected; }
bool     authed()           { return g_authHandle != NO_CONN; }
bool     linkEncrypted()    { return false; }   // Phase A: no link encryption yet (docs/ble-auth.md Phase B/C)
bool     authJustCompleted(){ bool r = g_authDone; g_authDone = false; return r; }

void service() {
  // Re-entrancy: only called from the loop task (also from inside transfer loops via transferAlive), and
  // it touches nothing a transfer is using, so a nested call is harmless.
  uint16_t kh;
  while (popKick(&kh)) if (g_server) g_server->disconnect(kh);
  // A central that never proves the key is dropped (frees the single GATT slot for the owner's phone).
  if (g_connHandle != NO_CONN && g_authHandle == NO_CONN && millis() - g_connMs > AUTH_TIMEOUT_MS) {
    Serial.println("[phone] auth timeout -> disconnect");
    g_connMs = millis();   // re-armed: a slow disconnect isn't re-issued every loop
    if (g_server) g_server->disconnect(g_connHandle);
  }
}

uint32_t authEpoch() { return g_authEpoch; }
bool transferAlive(uint32_t epoch) {
  service();   // 30 s auth timeout / kicks keep running during a long transfer
  return g_authHandle != NO_CONN && g_authEpoch == epoch;
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
int      tzOffsetMin()      { return g_tzOffMin; }
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
  uint16_t h = g_authHandle;   // snapshot (NONE would broadcast)
  if (!g_logData || h == NO_CONN || !len) return;
  g_logData->notify(data, len, h);
}

void detsNotify(const String& line) {
  uint16_t h = g_authHandle;
  if (!g_dets || h == NO_CONN || !line.length()) return;
  g_dets->notify((const uint8_t*)line.c_str(), line.length(), h);
}

}  // namespace phone

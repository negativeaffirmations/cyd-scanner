// fleet_internal.h — shared internals of the fleet implementation (fleet_link / fleet_leader / fleet_client).
// NOT part of the public facade (fleet.h). docs/fleet-mode.md §4-§5.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mbedtls/md.h>
#include "fleet_proto.h"

namespace fleet {
struct Ui;
namespace detail {

using namespace fleet_proto;

// --- transport state owned by fleet_link.cpp ---
extern uint8_t  g_ch;              // fleet channel (persisted)
extern bool     g_active, g_leader;
extern uint8_t  g_mac[6];          // our current (random) STA MAC
extern uint16_t g_maxPayload;

// Every frame after Join carries an app-level tag: HMAC-SHA256(SK, header + payload)[0..TAG_LEN), appended after
// the struct. It authenticates the header (incl. seq, so the replay window can't be fed forged numbers) whether or
// not ESP-NOW drops unencrypted frames that spoof an encrypted peer's MAC. ESP-NOW CCMP (LMK = SK) adds secrecy.
constexpr size_t   TAG_LEN = 8;
constexpr size_t   KEY_LEN = 16;   // FK, SK, LMK
constexpr size_t   NONCE_LEN = 16;

// Incremental HMAC-SHA256 with an arbitrary key (FK / SK). The device key itself stays inside devkey.
// finish() returns false on ANY crypto error (e.g. mbedtls can't allocate under heap pressure) and then fills `out`
// with a value no peer can predict. Every caller MUST check it: a zeroed digest must never be compared or used as a key.
class Hmac {
 public:
  explicit Hmac(const uint8_t* key, size_t klen);
  ~Hmac();
  Hmac& add(const void* p, size_t n);
  Hmac& add(const char* s);               // ASCII label, no terminator
  Hmac& add8(uint8_t v)  { return add(&v, 1); }
  Hmac& add16(uint16_t v) { return add(&v, 2); }   // little-endian on every target here
  [[nodiscard]] bool finish(uint8_t out[32]);
 private:
  mbedtls_md_context_t ctx_;
  bool ok_;
};

bool ownFleetKey(uint8_t out[KEY_LEN]);   // FK of THIS unit (client side): HMAC(devkey, "CFLO-FLT-KEY-1|" + sid)
void randomBytes(void* p, size_t n);
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n);
void macStr(const uint8_t* m, char* out);   // out: 18 B
bool sidFromHex(const char* hex, uint8_t out[6]);
void sidToHex(const uint8_t sid[6], char out[13]);

// Peers. lmk == nullptr: unencrypted. Adds the peer, or modifies it when it already exists.
bool peerSet(const uint8_t* mac, const uint8_t* lmk);
void peerDel(const uint8_t* mac);

bool sendClear(const uint8_t* mac, const void* frame, size_t len);
// True once a unicast to `mac` has LEFT the radio (send callback, success or no-ack) since the last call for that mac.
// esp_now_send() only queues: the leader waits for this before encrypting a peer or deleting it.
bool sendDone(const uint8_t* mac);
// Fill the header (ver/type/sess/++txSeq), append the tag and send. `frame` must have TAG_LEN spare bytes after len.
bool sendSealed(const uint8_t* mac, uint8_t* frame, size_t len, Msg type, uint16_t sess, uint32_t& txSeq,
                const uint8_t sk[KEY_LEN]);
// Verify the trailing tag of a received session frame (len includes the tag). Constant-time.
bool tagOk(const uint8_t* frame, size_t len, const uint8_t sk[KEY_LEN]);

// 32-wide sliding replay window over the per-direction session seq (starts at 1; 0 is never valid).
struct SeqWin {
  uint32_t top = 0, bits = 0;
  bool accept(uint32_t s);
  void reset() { top = bits = 0; }
};

void setRadioChannel(uint8_t ch);          // tune now, don't persist (client channel search)
void event(const char* op);                // -> the host's log sink (logFleetEvent), if registered
uint32_t eventCount();

// One received frame, already copied out of the Wi-Fi task.
struct Rx { const uint8_t* mac; int8_t rssi; const uint8_t* data; uint16_t len; };

namespace leader {
void begin();                              // load the member store, start probing
void onFrame(const Rx& f);
void tick();
void fillUi(Ui& u);
void printStatus();
void counts(uint8_t& enrolled, uint8_t& joined);
bool memberFix(float* lat, float* lon, uint32_t* ageMs, bool* live);   // a member's GPS fix (sticky choice; live < 10 s)
bool isSelf(const uint8_t mac[6]);                          // a member's BLE MAC
}
namespace client {
void begin();
void onFrame(const Rx& f);
void tick();
void fillUi(Ui& u);
void printStatus();
bool joined();
bool timeFix(uint32_t* utc, uint32_t* atMs);
bool timeFresh();
bool posFix(float* lat, float* lon, uint32_t* ageMs, bool* live);
bool isSelf(const uint8_t mac[6]);                          // in the leader's Sync.selfMacs
void dropSession(const char* why);         // back to searching (rekey, unjoin)
void forgetKey();                          // the device key changed: recompute FK on next use
}

}  // namespace detail
}  // namespace fleet

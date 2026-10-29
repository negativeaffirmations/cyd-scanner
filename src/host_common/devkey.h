// devkey.h — per-device phone-link key (SEC-H3, docs/ble-auth.md). A random 128-bit key lives in NVS and is
// delivered to the owner's phone by a QR / serial; phone.cpp proves it per connection with an HMAC-SHA256
// challenge-response before any characteristic does anything.
#pragma once
#include <Arduino.h>

namespace devkey {

// URL of the hosted control web app (GitHub Pages). The pairing deep link is this + "#devkey=...".
inline constexpr char APP_URL[] = "https://negativeaffirmations.github.io/cyd-scanner/webapp/";

// Load the key from NVS (cydscan/blekey, 16 B) or create it (esp_fill_random, generation 1). Call after
// NimBLEDevice::init so the radio is already feeding the hardware RNG.
void begin();

const char* keyHex();   // 32 lowercase hex chars (secret: print only on explicit request)
uint32_t    gen();      // key generation, bumped by every rekey() (shown on the sticker / in the URL)
// "<APP_URL>#devkey=<32hex>&sid=<12hex>&kg=<gen>" -- the QR / serial pairing link. Static buffer.
const char* pairUrl();
// Replace the key with a fresh random one and bump the generation. Caller kicks the phone (phone::rekeyAndKick).
// Returns false when the new key could not be written to NVS (it then lasts until reboot only).
bool rekey();
// True when the key currently in use is stored in NVS (false after a failed write: the QR / sticker would break at reboot).
bool persisted();

// Identity strings (host_log.h lives behind this header: phone.cpp can't include it without symbol clashes).
const char* sid();        // scannerIdHex(): 12 lowercase hex, the factory eFuse MAC
const char* unitName();   // scannerName()
const char* fwVersion();  // FW_VERSION

// HMAC-SHA256(key, msg); false (out zeroed) on a crypto error. Safe to call from the NimBLE task while loop() rekeys.
bool hmac(const uint8_t* msg, size_t len, uint8_t out[32]);
// Constant-time compare (no early exit on the first differing byte).
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n);

}  // namespace devkey

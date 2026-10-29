// phone.h — BLE GATT peripheral for the phone web app (Web Bluetooth).
//
// Exposes a small custom service the phone connects to over BLE to:
//   - sync wall-clock time (phone -> device)
//   - push GPS fix (phone -> device)
//   - request the Wi-Fi log-download mode (phone -> device)
//   - read live status (device -> phone, notify)
//
// The web app runs on HTTPS (required for Web Bluetooth + geolocation) and talks
// to this service. See webapp/index.html.
#pragma once

#include <Arduino.h>

namespace phone {

// Largest upload chunk ("U<seq><bytes>") the CMD characteristic accepts; the web app sends 240.
constexpr size_t UPLOAD_CHUNK_MAX = 500;

void begin(const char* devName);

// Push a status line to the app (throttle in the caller). Format is
// "key=val;key=val;..." parsed by the web app.
void setStatus(const String& s);

// Link security level advertised in the AUTH characteristic's sec= (docs/ble-auth.md): 0 = HMAC auth only
// (Phase A), 1 = + opportunistic bonding after auth (Phase B), 2 = authed AND encrypted (Phase C).
// Only 0 is implemented; B/C are not built yet.
#ifndef PHONE_BLE_SEC
#define PHONE_BLE_SEC 0
#endif

bool     connected();   // a central is on the GATT link (UI: grey BLE icon until it authenticates)
// The connected central proved the device key (HMAC challenge-response) -- THE gate for everything that
// sends data to / takes data from "the phone": transfers, DETS, STATUS, phone GPS freshness.
bool     authed();
bool     linkEncrypted();       // the authed link is AES-encrypted (always false in Phase A)
bool     authJustCompleted();   // true once per successful authentication (consumed on read)
// Loop-context housekeeping: the 30 s auth timeout and kicks queued by the NimBLE callbacks (which must
// never disconnect from inside a callback). Called from core::servicePhoneCommands.
void     service();
// Rekey (new random key, generation + 1), log the phonecfg audit line and drop the current central so the
// old key's holder must re-pair. Used by the CYD "Phone key" screen and the serial `rekey confirm`.
// Returns false when the new key could not be saved to NVS (it then lasts until reboot only).
bool     rekeyAndKick();
// Bumped on every authentication success and every disconnect of the tracked central. A transfer captures it
// when it starts and stops when transferAlive() says the session changed (a NEW peer must not receive the
// tail of the old peer's download). transferAlive() also runs service(), so the auth timeout / kicks keep
// working through a long chunk loop.
uint32_t authEpoch();
bool     transferAlive(uint32_t epoch);

// This device's own BLE MAC (as the C5 would report it, MSB-first). Lets the CYD
// filter out its own advertisement from the scan results. Returns false if unknown.
bool     ownMac(uint8_t out[6]);

// The connected phone's BLE address (from the GATT link), MSB-first. Best-effort
// self-ecosystem filter — returns false when no phone is connected. Note: phones use
// rotating random addresses, so this may not always match their scanned advertisements.
bool     peerMac(uint8_t out[6]);

// Time sync
bool     hasTime();
uint32_t epochNow();  // current LOCAL epoch seconds, or 0 if never synced
int      tzOffsetMin();  // last phone tz offset (UTC-local minutes, JS getTimezoneOffset); persisted
                         // in NVS so a GPS-only (phone-less) boot can still show local time. 0 until set.

// GPS
constexpr uint32_t PHONE_GPS_STALE_MS = 90000;  // phone fix older than this (or phone gone) is not live
bool     hasGps();    // a LIVE phone fix: connected + written within PHONE_GPS_STALE_MS
bool     hadGps();    // the phone has sent a fix at some point this boot (lat()/lon() hold the last one)
// The last phone fix as one consistent snapshot (written from the NimBLE task); false = none yet.
bool     gpsFix(float* lat, float* lon, uint32_t* atMs);
uint32_t gpsAgeMs();  // ms since the last phone fix (UINT32_MAX = none; never wraps negative)

// Wi-Fi log-download mode requested by the app.
bool downloadRequested();

// True once if the app asked to reload the signature DB (consumed on read).
bool reloadRequested();

// True once if the app asked to download the log over BLE (consumed on read).
bool logRequested();

// True once if the app asked to start / stop scanning (consumed on read).
bool scanStartRequested();
bool scanStopRequested();

// True once if the app asked for a fresh log session ("N"), consumed on read.
bool newSessionRequested();

// True once if the app asked for the list of session logs ("Q"), consumed on read.
bool listRequested();

// True once if the app asked to download a specific file ("F:<path>"); fills `out`.
bool fileRequested(char* out, size_t cap);

// True once if the app asked to delete a session file ("D:<path>"); fills `out`.
bool deleteRequested(char* out, size_t cap);

// True once if the app asked for a time-filtered log export ("T:<mode>[:<minutes>][:<path>]");
// fills mode (0 whole file / 1 past 24 h / 2 past <minutes>), minutes, and path ("" = current session).
bool exportRequested(int* mode, int* minutes, char* path, size_t cap);

// True once if the app set a brightness ("B:<0-100>"); fills the percent value.
bool brightnessRequested(int* outPct);

// File explorer (host_files.cpp). True once per "Z<seq>:<op>:<arg>" write; fills `out` with "<seq>:<op>:<arg>".
bool fsRequested(char* out, size_t cap);
// True once per "U<seq byte><bytes>" upload chunk; copies the raw bytes (binary-safe), their length and the tag.
bool uploadChunk(uint8_t* out, size_t cap, size_t* outLen, uint8_t* seq);
// True once if a chunk arrived before the previous one was consumed (the upload must be aborted); its tag.
bool uploadOverrun(uint8_t* seq);

// True once if the app set the status-LED brightness ("I:<0-100>"); fills the percent value.
bool ledBrightnessRequested(int* outPct);

// True once if the app asked to lock/unlock the CYD display ("K1"/"K0"); fills the desired state.
bool lockRequested(bool* outLock);

// True once if the app set the scan-source mask ("S:<decimal SourceMask>"); fills `out`.
bool srcMaskRequested(uint8_t* out);

// True once if the app set the time / GPS source preference ("P:<t><g>", each 0 prefer phone /
// 1 prefer device / 2 device only); fills both.
bool sourcePrefRequested(uint8_t* timePref, uint8_t* gpsPref);

// Whitelist commands, each true once (consumed on read): "W" reload, "Y" list,
// "A:<csv rule line>" add (fills `out`), "E:<index>" remove the Nth active rule.
bool wlReloadRequested();
bool wlListRequested();
bool wlAddRequested(char* out, size_t cap);
bool wlRemoveRequested(int* outIdx);

// Notify a chunk of log data to the authenticated phone (used during BLE log download); no-op unless authed().
void logNotify(const uint8_t* data, size_t len);

// Notify one line of the live detection snapshot (mirrors the CYD screen). The
// caller sends a "D:<count>" header then <count> rows each scan cycle.
void detsNotify(const String& line);

}  // namespace phone

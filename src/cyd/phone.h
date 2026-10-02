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

void begin(const char* devName);

// Push a status line to the app (throttle in the caller). Format is
// "key=val;key=val;..." parsed by the web app.
void setStatus(const String& s);

bool     connected();

// This device's own BLE MAC (as the C5 would report it, MSB-first). Lets the CYD
// filter out its own advertisement from the scan results. Returns false if unknown.
bool     ownMac(uint8_t out[6]);

// The connected phone's BLE address (from the GATT link), MSB-first. Best-effort
// self-ecosystem filter — returns false when no phone is connected. Note: phones use
// rotating random addresses, so this may not always match their scanned advertisements.
bool     peerMac(uint8_t out[6]);

// Time sync
bool     hasTime();
uint32_t epochNow();  // current UTC epoch seconds, or 0 if never synced

// GPS
bool  hasGps();
float lat();
float lon();

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

// True once if the app set a brightness ("B:<0-100>"); fills the percent value.
bool brightnessRequested(int* outPct);

// True once if the app set the scan-source mask ("S:<decimal SourceMask>"); fills `out`.
bool srcMaskRequested(uint8_t* out);

// Whitelist commands, each true once (consumed on read): "W" reload, "Y" list,
// "A:<csv rule line>" add (fills `out`), "E:<index>" remove the Nth active rule.
bool wlReloadRequested();
bool wlListRequested();
bool wlAddRequested(char* out, size_t cap);
bool wlRemoveRequested(int* outIdx);

// Notify a chunk of log data to the connected phone (used during BLE log download).
void logNotify(const uint8_t* data, size_t len);

// Notify one line of the live detection snapshot (mirrors the CYD screen). The
// caller sends a "D:<count>" header then <count> rows each scan cycle.
void detsNotify(const String& line);

}  // namespace phone

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

// Notify a chunk of log data to the connected phone (used during BLE log download).
void logNotify(const uint8_t* data, size_t len);

}  // namespace phone

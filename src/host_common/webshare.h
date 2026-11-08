// webshare.h — on-demand Wi-Fi SoftAP + HTTP server to download the SD log.
//
// Brought up when the phone requests download mode. The CYD screen shows a QR code
// that joins the phone to this AP; the phone then opens the URL to grab scanlog.jsonl.
#pragma once

#include <Arduino.h>

namespace webshare {

void        start();       // bring up SoftAP + web server
void        stop();        // tear down
bool        active();
bool        clientConnected();  // true if a phone has joined the SoftAP
int         stations();    // stations joined to the SoftAP (0 when it's down) -> STATUS apsta
void        handle();      // call frequently while active

void        setLogPath(const char* path);  // which SD file the download serves

const char* ssid();
const char* password();
const char* apPass();      // random NVS-persisted WPA2 PSK (valid after start(); same as password())
const char* url();         // e.g. "http://192.168.4.1"
const char* logPath();     // the file the "current log" button serves

// CYD download boot (fw 0.35.0): the CYD can't fit Wi-Fi beside the phone link, so it restarts with Bluetooth off.
bool        prepare();             // SSID + stored PSK, with nothing started (STATUS needs them before the restart);
                                   // false if the PSK couldn't be saved (the next boot would make another one)
void        setAnnounce(bool on);  // STATUS carries the creds + apboot=1 although the AP isn't up yet
bool        announcing();
void        setRebootMode(bool on);   // the page offers "Done: restart the scanner" and serves /stop
void        setStopToken(const char* tok);   // 8 hex, per download boot: /stop?t=<tok> (a stale tab can't restart a later one)
const char* stopToken();
bool        stopRequested();          // /stop with the right token was opened (consume on read)
uint32_t    lastActivityMs();         // last request started or finished, or station joined / left (millis)
String      wifiQr();      // WIFI: join string for a QR code

}  // namespace webshare

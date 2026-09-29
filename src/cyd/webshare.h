// webshare.h — on-demand Wi-Fi SoftAP + HTTP server to download the SD log.
//
// Brought up when the phone requests download mode. The CYD screen shows a QR code
// that joins the phone to this AP; the phone then opens the URL to grab scanlog.csv.
#pragma once

#include <Arduino.h>

namespace webshare {

void        start();       // bring up SoftAP + web server
void        stop();        // tear down
bool        active();
void        handle();      // call frequently while active

const char* ssid();
const char* password();
const char* url();         // e.g. "http://192.168.4.1"
String      wifiQr();      // WIFI: join string for a QR code

}  // namespace webshare

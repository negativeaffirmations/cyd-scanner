// webshare.cpp — see webshare.h.
#include "webshare.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>

namespace {

WebServer  g_server(80);
bool       g_active = false;
char       g_ssid[24] = {0};
char       g_pw[16]   = {0};
const char* kUrl      = "http://192.168.4.1";
const char* kLogPath  = "/scanlog.csv";

void handleRoot() {
  String h = "<!doctype html><html><head><meta name=viewport "
             "content='width=device-width,initial-scale=1'><title>CYD Scanner</title>"
             "<style>body{font-family:sans-serif;margin:2em;background:#111;color:#eee}"
             "a{display:inline-block;margin-top:1em;padding:.8em 1.2em;background:#2a7;"
             "color:#fff;text-decoration:none;border-radius:8px}</style></head><body>"
             "<h2>CYD Scanner</h2><p>Detection log on the microSD card.</p>"
             "<a href='/scanlog.csv'>Download scanlog.csv</a></body></html>";
  g_server.send(200, "text/html", h);
}

void handleLog() {
  if (!SD.exists(kLogPath)) { g_server.send(404, "text/plain", "no log yet"); return; }
  File f = SD.open(kLogPath, "r");
  if (!f) { g_server.send(500, "text/plain", "open failed"); return; }
  g_server.sendHeader("Content-Disposition", "attachment; filename=scanlog.csv");
  g_server.streamFile(f, "text/csv");
  f.close();
}

}  // namespace

namespace webshare {

void start() {
  if (g_active) return;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(g_ssid, sizeof(g_ssid), "CYD-Scan-%04X", (uint16_t)(mac & 0xFFFF));
  snprintf(g_pw, sizeof(g_pw), "scan%06X", (uint32_t)((mac >> 16) & 0xFFFFFF));

  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_ssid, g_pw);
  g_server.on("/", handleRoot);
  g_server.on("/scanlog.csv", handleLog);
  g_server.begin();
  g_active = true;
  Serial.printf("[webshare] AP up: SSID=%s PW=%s %s\n", g_ssid, g_pw, kUrl);
}

void stop() {
  if (!g_active) return;
  g_server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  g_active = false;
  Serial.println("[webshare] AP down");
}

bool        active()   { return g_active; }
bool        clientConnected() { return g_active && WiFi.softAPgetStationNum() > 0; }
void        handle()   { if (g_active) g_server.handleClient(); }
const char* ssid()     { return g_ssid; }
const char* password() { return g_pw; }
const char* url()      { return kUrl; }

String wifiQr() {
  // Standard Wi-Fi join QR payload.
  return String("WIFI:T:WPA;S:") + g_ssid + ";P:" + g_pw + ";;";
}

}  // namespace webshare

// webshare.cpp — see webshare.h.
#include "webshare.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>

namespace {

WebServer  g_server(80);
bool       g_active = false;
char       g_ssid[24]  = {0};
char       g_pw[16]    = {0};
const char* kUrl       = "http://192.168.4.1";
char       g_logPath[48] = "/scanlog.csv";  // current session log (set by setLogPath)

const char* logBasename() {
  const char* s = strrchr(g_logPath, '/');
  return s ? s + 1 : g_logPath;
}

// Index page: list every /logs/ session with size + a download link, so the phone can
// grab any past session over Wi-Fi (not just the current one). Current session marked.
void handleRoot() {
  Serial.println("[webshare] GET / (session index)");
  String h = "<!doctype html><html><head><meta name=viewport "
             "content='width=device-width,initial-scale=1'><title>CYD Scanner</title>"
             "<style>body{font-family:sans-serif;margin:1.2em;background:#111;color:#eee}"
             "h2{margin:.2em 0}ul{padding-left:1.1em;line-height:1.9}a{color:#6cf}"
             "small{color:#8b949e}</style></head><body>"
             "<h2>CYD Scanner &mdash; session logs</h2><ul>";
  int n = 0;
  File dir = SD.open("/logs");
  if (dir) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (e.isDirectory()) continue;
      String nm = e.name(); int sl = nm.lastIndexOf('/');
      String base = sl >= 0 ? nm.substring(sl + 1) : nm;
      String cur = (String("/logs/") + base == String(g_logPath)) ? " <small>(current)</small>" : "";
      h += "<li><a href='/dl?f=" + base + "'>" + base + "</a> <small>" +
           String(e.size() / 1024.0, 1) + " KB</small>" + cur + "</li>";
      n++;
    }
    dir.close();
  }
  if (!n) h += "<li><small>no sessions yet</small></li>";
  h += "</ul></body></html>";
  g_server.send(200, "text/html", h);
}

// Serve any /logs/ file by name (?f=<basename>), validated (no path traversal).
void handleDl() {
  String f = g_server.arg("f");
  if (!f.length() || f.indexOf('/') >= 0 || f.indexOf("..") >= 0) {
    g_server.send(400, "text/plain", "bad name"); return;
  }
  String path = "/logs/" + f;
  Serial.printf("[webshare] GET /dl f=%s exists=%d\n", f.c_str(), SD.exists(path));
  if (!SD.exists(path)) { g_server.send(404, "text/plain", "not found"); return; }
  File file = SD.open(path, "r");
  if (!file) { g_server.send(500, "text/plain", "open failed"); return; }
  g_server.sendHeader("Content-Disposition", "attachment; filename=" + f);
  size_t n = g_server.streamFile(file, "text/csv");
  file.close();
  Serial.printf("[webshare] streamed %u bytes of %s\n", (unsigned)n, path.c_str());
}

// Backward-compat: /scanlog.csv streams the current session directly.
void handleLog() {
  Serial.printf("[webshare] GET /scanlog.csv -> %s exists=%d\n", g_logPath, SD.exists(g_logPath));
  if (!SD.exists(g_logPath)) { g_server.send(404, "text/plain", "no log yet"); return; }
  File f = SD.open(g_logPath, "r");
  if (!f) { g_server.send(500, "text/plain", "open failed"); return; }
  g_server.sendHeader("Content-Disposition",
                      String("attachment; filename=") + logBasename());
  size_t n = g_server.streamFile(f, "text/csv");
  f.close();
  Serial.printf("[webshare] streamed %u bytes\n", (unsigned)n);
}

}  // namespace

namespace webshare {

void start() {
  if (g_active) return;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(g_ssid, sizeof(g_ssid), "CYD-Scan-%04X", (uint16_t)(mac & 0xFFFF));
  snprintf(g_pw, sizeof(g_pw), "scan%06X", (uint32_t)((mac >> 16) & 0xFFFFFF));

  WiFi.persistent(false);          // don't thrash NVS on every AP start
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(g_ssid, g_pw);
  IPAddress ip = WiFi.softAPIP();
  g_server.on("/", handleRoot);
  g_server.on("/dl", handleDl);
  g_server.on("/scanlog.csv", handleLog);
  g_server.onNotFound([]() {       // reveal whatever the phone actually requests
    Serial.printf("[webshare] 404 %s\n", g_server.uri().c_str());
    g_server.send(404, "text/plain", "not found");
  });
  g_server.begin();
  g_active = true;
  Serial.printf("[webshare] softAP=%s SSID=%s PW=%s ip=%s heap=%u serving=%s exists=%d\n",
                ok ? "OK" : "FAIL", g_ssid, g_pw, ip.toString().c_str(),
                (unsigned)ESP.getFreeHeap(), g_logPath, SD.exists(g_logPath));
}

void stop() {
  if (!g_active) return;
  g_server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  g_active = false;
  Serial.println("[webshare] AP down");
}

void        setLogPath(const char* path) {
  strncpy(g_logPath, path, sizeof(g_logPath) - 1);
  g_logPath[sizeof(g_logPath) - 1] = 0;
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

// webshare.cpp — see webshare.h.
#include "webshare.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_random.h>
#include "phone.h"
#include "logfilter.h"

namespace {

WebServer  g_server(80);
bool       g_active = false;
char       g_ssid[24]  = {0};
char       g_pw[20]    = {0};  // WPA2 PSK (random, NVS-persisted); 16 chars + NUL
const char* kUrl       = "http://192.168.4.1";
char       g_logPath[48] = "/scanlog.jsonl";  // current session log (set by setLogPath)

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
           String(e.size() / 1024.0, 1) + " KB</small>" + cur +
           " <small><a href='/dlf?f=" + base + "&mode=1'>24h</a></small></li>";
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
  size_t n = g_server.streamFile(file, f.endsWith(".jsonl") ? "application/x-ndjson" : "text/csv");
  file.close();
  Serial.printf("[webshare] streamed %u bytes of %s\n", (unsigned)n, path.c_str());
}

// Time-filtered download: /dlf?f=<basename|current>&mode=<0|1|2>&arg=<minutes>. mode 0 = whole file,
// 1 = past 24 h, 2 = past <arg> minutes. The CYD filters line-by-line (logfilter.h) and streams only
// the matching lines with chunked transfer encoding, so a filtered pull never sends the whole file.
void handleDlf() {
  String f = g_server.arg("f");
  String path;
  if (!f.length() || f == "current") {
    path = g_logPath;
  } else {
    if (f.indexOf('/') >= 0 || f.indexOf("..") >= 0) { g_server.send(400, "text/plain", "bad name"); return; }
    path = "/logs/" + f;
  }
  int mode = g_server.arg("mode").toInt();
  uint32_t arg = (uint32_t)g_server.arg("arg").toInt();
  Serial.printf("[webshare] GET /dlf %s mode=%d arg=%u\n", path.c_str(), mode, (unsigned)arg);
  if (!SD.exists(path)) { g_server.send(404, "text/plain", "not found"); return; }
  File file = SD.open(path, "r");
  if (!file) { g_server.send(500, "text/plain", "open failed"); return; }
  logfilter::Cutoff c = logfilter::make(mode, arg, phone::epochNow(), millis());
  String base = path.substring(path.lastIndexOf('/') + 1);
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);  // chunked: the filtered size isn't known up front
  g_server.sendHeader("Content-Disposition", "attachment; filename=" + base);
  g_server.send(200, base.endsWith(".jsonl") ? "application/x-ndjson" : "text/csv", "");
  char line[logfilter::LINE_CAP];
  char out[1024];
  size_t len, on = 0, total = 0;
  logfilter::LineReader rd(file);
  while (rd.next(line, len)) {
    if (!logfilter::keep(line, c)) continue;
    if (on + len > sizeof(out)) { g_server.sendContent(out, on); total += on; on = 0; }
    memcpy(out + on, line, len);
    on += len;
  }
  if (on) { g_server.sendContent(out, on); total += on; }
  g_server.sendContent("");  // terminating chunk
  file.close();
  Serial.printf("[webshare] filtered %u bytes of %s\n", (unsigned)total, path.c_str());
}

// /scanlog.jsonl streams the current session directly (/scanlog.csv kept as an alias).
void handleLog() {
  Serial.printf("[webshare] GET /scanlog -> %s exists=%d\n", g_logPath, SD.exists(g_logPath));
  if (!SD.exists(g_logPath)) { g_server.send(404, "text/plain", "no log yet"); return; }
  File f = SD.open(g_logPath, "r");
  if (!f) { g_server.send(500, "text/plain", "open failed"); return; }
  g_server.sendHeader("Content-Disposition",
                      String("attachment; filename=") + logBasename());
  size_t n = g_server.streamFile(f, "application/x-ndjson");
  f.close();
  Serial.printf("[webshare] streamed %u bytes\n", (unsigned)n);
}

// SEC-M1 fix: the SoftAP password used to be derived from the efuse MAC, which is broadcast as the
// AP's BSSID, so anyone in range could recompute it and pull /logs/. Now a random PSK is generated
// once (esp_random) and persisted in NVS so it is stable across boots (and for the QR). The SSID
// stays MAC-derived — it isn't a secret.
void loadOrCreatePsk() {
  static const char kCs[] = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no look-alikes
  const size_t kLen = 16;  // WPA2 PSK must be 8..63 chars
  Preferences p;
  bool open = p.begin("cydscan", false);
  if (open && p.getString("appsk", g_pw, sizeof(g_pw)) == kLen) { p.end(); return; }  // stored PSK ok
  for (size_t i = 0; i < kLen; i++) g_pw[i] = kCs[esp_random() % (sizeof(kCs) - 1)];
  g_pw[kLen] = 0;
  if (open) { p.putString("appsk", g_pw); p.end(); }  // if NVS is unavailable, use this boot's PSK only
}

}  // namespace

namespace webshare {

void start() {
  if (g_active) return;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(g_ssid, sizeof(g_ssid), "CYD-Scan-%04X", (uint16_t)(mac & 0xFFFF));
  loadOrCreatePsk();  // g_pw = stored random PSK (SEC-M1)

  WiFi.persistent(false);          // don't thrash NVS on every AP start
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(g_ssid, g_pw);
  IPAddress ip = WiFi.softAPIP();
  g_server.on("/", handleRoot);
  g_server.on("/dl", handleDl);
  g_server.on("/dlf", handleDlf);
  g_server.on("/scanlog.jsonl", handleLog);
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
const char* apPass()   { return g_pw; }
const char* url()      { return kUrl; }

String wifiQr() {
  // Standard Wi-Fi join QR payload.
  return String("WIFI:T:WPA;S:") + g_ssid + ";P:" + g_pw + ";;";
}

}  // namespace webshare

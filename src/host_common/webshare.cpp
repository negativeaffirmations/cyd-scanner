// webshare.cpp — see webshare.h.
#include "webshare.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <Preferences.h>
#include <esp_random.h>
#include <new>
#include "phone.h"
#include "fleet.h"
#include "logfilter.h"
#include "host_log.h"  // g_fg/g_bg (live files), sessionMetaLine()

namespace {

WebServer  g_server(80);
bool       g_active = false;
char       g_ssid[24]  = {0};
char       g_pw[20]    = {0};  // WPA2 PSK (random, NVS-persisted); 16 chars + NUL
const char* kUrl       = "http://192.168.4.1";
char       g_logPath[48] = "/scanlog.jsonl";  // current session log (set by setLogPath)
wifi_event_id_t g_evId = 0;   // AP station join/leave/IP logger, registered while the AP is up

// Console trace of the phone joining the AP, timestamped so a slow page load can be split into
// "the phone hadn't joined / got no IP yet" vs "the request never arrived" vs "the server was slow".
// Runs in the Wi-Fi event task. No MACs printed (it's the owner's phone, but keep logs identifier-free).
void onApEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.printf("[webshare] %lu ms: station joined (aid %u)\n", millis(), (unsigned)info.wifi_ap_staconnected.aid); break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.printf("[webshare] %lu ms: station left (aid %u, reason %u)\n", millis(),
                    (unsigned)info.wifi_ap_stadisconnected.aid, (unsigned)info.wifi_ap_stadisconnected.reason); break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.printf("[webshare] %lu ms: station got IP %s\n", millis(),
                    IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str()); break;
    default: break;
  }
}

const char* logBasename() {
  const char* s = strrchr(g_logPath, '/');
  return s ? s + 1 : g_logPath;
}

// Index page: the current log as a big button, then the newest INDEX_MAX /logs/ files, newest first (by
// FS write time, then name), each with size + a 24 h filtered link. A card holds hundreds of logs, so only
// the newest are kept (a bounded insertion sort on the heap, freed after the request; the CYD in normal mode
// has little heap left while the AP is up); older ones are reachable through the time-range links.
constexpr int INDEX_MAX = 40;
struct IndexEntry { char name[48]; uint32_t size; time_t t; };

bool newerThan(const IndexEntry& a, const IndexEntry& b) {
  return a.t != b.t ? a.t > b.t : strcmp(a.name, b.name) > 0;
}

void handleRoot() {
  const uint32_t t0 = millis();
  Serial.printf("[webshare] %lu ms: GET / (session index)\n", (unsigned long)t0);
  IndexEntry* ents = new (std::nothrow) IndexEntry[INDEX_MAX];
  int kept = 0, total = 0;
  uint32_t curSize = 0;
  bool curFound = false;
  File dir = SD.open("/logs");
  if (dir) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (e.isDirectory()) continue;
      const char* nm = e.name(); const char* sl = strrchr(nm, '/');
      const char* base = sl ? sl + 1 : nm;
      if (strlen(base) >= sizeof(IndexEntry::name)) continue;
      total++;
      if (!strcmp(base, logBasename())) { curFound = true; curSize = e.size(); }
      if (!ents) continue;
      IndexEntry x;
      strcpy(x.name, base); x.size = e.size(); x.t = e.getLastWrite();
      int i;
      if (kept < INDEX_MAX) i = kept++;
      else if (newerThan(x, ents[INDEX_MAX - 1])) i = INDEX_MAX - 1;
      else continue;
      while (i > 0 && newerThan(x, ents[i - 1])) { ents[i] = ents[i - 1]; i--; }   // keep newest-first order
      ents[i] = x;
    }
    dir.close();
  }
  String h = "<!doctype html><html><head><meta name=viewport "
             "content='width=device-width,initial-scale=1'><title>COINTEL-FLO</title>"
             "<style>body{font-family:sans-serif;margin:1.2em;background:#111;color:#eee}"
             "h2{margin:.2em 0}ul{padding-left:1.1em;line-height:1.9}a{color:#6cf}"
             "small{color:#8b949e}a.b{display:block;margin:.8em 0 1.2em;padding:.9em;border-radius:10px;"
             "background:#a0a;color:#fff;text-align:center;text-decoration:none;font-size:1.1em}"
             "</style></head><body><h2>COINTEL-FLO &mdash; logs</h2>";
  if (curFound)
    h += String("<a class=b href='/dl?f=") + logBasename() + "'>Download current log<br><small style='color:#fdf'>" +
         logBasename() + " &middot; " + String(curSize / 1024.0, 1) + " KB</small></a>";
  h += "<h3>Newest first</h3><ul>";
  for (int i = 0; i < kept; i++) {
    const IndexEntry& x = ents[i];
    char when[24] = "";
    if (x.t > 1600000000) { struct tm tmv; gmtime_r(&x.t, &tmv); strftime(when, sizeof(when), " &middot; %b %d %H:%M", &tmv); }
    h += String("<li><a href='/dl?f=") + x.name + "'>" + x.name + "</a> <small>" + String(x.size / 1024.0, 1) +
         " KB" + when + (strcmp(x.name, logBasename()) ? "" : " (current)") +
         "</small> <small><a href='/dlf?f=" + x.name + "&mode=1'>24h</a></small></li>";
  }
  if (!total) h += "<li><small>no logs yet</small></li>";
  if (total > kept)
    h += String("<li><small>") + (total - kept) + " older log" + (total - kept == 1 ? "" : "s") +
         " not listed &mdash; use the time ranges below.</small></li>";
  delete[] ents;
  h += "</ul><h3>All logs, by time range</h3><ul>"
       "<li><a href='/dlf?f=all&mode=1'>Past 24 hours (all logs)</a></li>"
       "<li><a href='/dlf?f=all&mode=0'>Since boot (all logs)</a></li></ul></body></html>";
  g_server.send(200, "text/html", h);
  Serial.printf("[webshare] index: %d files (%d listed), %u B, sent in %lu ms\n", total, kept, (unsigned)h.length(),
                millis() - t0);
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
// Range-export file name <first>-<last>.jsonl (YYYYMMDD-HHMMSS each, the log files' own stamp
// format, so saved exports sort chronologically), from the first/last dated row in the export;
// `fallback` when nothing is dated. epochs are local time, so gmtime gives the local fields.
String spanFileName(uint32_t lo, uint32_t hi, const char* fallback) {
  if (!hi) return fallback;
  char nm[48];
  struct tm a, b;
  time_t ta = (time_t)lo, tb = (time_t)hi;
  gmtime_r(&ta, &a); gmtime_r(&tb, &b);
  snprintf(nm, sizeof(nm), "%04d%02d%02d-%02d%02d%02d-%04d%02d%02d-%02d%02d%02d.jsonl",
           a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, a.tm_sec,
           b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min, b.tm_sec);
  return nm;
}
void spanOf(const char* line, uint32_t& lo, uint32_t& hi) {
  uint32_t e = logfilter::fieldU32(line, "\"epoch\":");
  if (e) { if (e < lo) lo = e; if (e > hi) hi = e; }
}

// /dlf?f=all&mode=&arg= : the window across EVERY /logs/ file, joined into one NDJSON download
// (logfilter::MultiExport -- same dating/filtering as the BLE "T:...:*" export).
void handleDlfAll(int mode, uint32_t arg) {
  const char* boot[12];
  int nb = thisBootFiles(boot, 12);
  logfilter::MultiExport mx(mode, arg, epochLocal(), millis(), boot, nb);
  int nf = mx.begin();
  uint32_t lo = 0xFFFFFFFFUL, hi = 0;  // span pre-pass: the download is named by its contents
  mx.run([&lo, &hi](const char* p, size_t) { spanOf(p, lo, hi); return true; });
  String fname = spanFileName(lo, hi, mode == 1 ? "all-last24h.jsonl" : mode == 2 ? "all-range.jsonl" : "all-sinceboot.jsonl");
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.sendHeader("Content-Disposition", "attachment; filename=" + fname);
  g_server.send(200, "application/x-ndjson", "");
  static struct Out { char b[1024]; size_t n; uint32_t total; } o;  // static: off the handler stack
  o.n = 0; o.total = 0;
  auto put = [](const char* p, size_t len) {
    if (o.n + len > sizeof(o.b)) { g_server.sendContent(o.b, o.n); o.total += o.n; o.n = 0; }
    memcpy(o.b + o.n, p, len);
    o.n += len;
    return (bool)g_server.client().connected();  // browser cancelled -> stop reading the card
  };
  char meta[160];
  int metaN = sessionMetaLine(meta, sizeof(meta));
  if (metaN) put(meta, metaN);
  mx.run(put);
  if (o.n) { g_server.sendContent(o.b, o.n); o.total += o.n; }
  g_server.sendContent("");
  Serial.printf("[webshare] /dlf all mode=%d: %d file(s), %u bytes\n", mode, nf, (unsigned)o.total);
}

void handleDlf() {
  String f = g_server.arg("f");
  String path;
  if (f == "all") { handleDlfAll(g_server.arg("mode").toInt(), (uint32_t)g_server.arg("arg").toInt()); return; }
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
  logfilter::Cutoff c = logfilter::make(mode, arg, epochLocal(), millis());
  String base = path.substring(path.lastIndexOf('/') + 1);
  if ((mode == 1 || mode == 2) && base.endsWith(".jsonl")) {  // name a range by its contents' span
    uint32_t lo = 0xFFFFFFFFUL, hi = 0;
    char ln[logfilter::LINE_CAP];
    size_t ll;
    logfilter::LineReader pre(file);
    while (pre.next(ln, ll)) if (logfilter::keep(ln, c)) spanOf(ln, lo, hi);
    file.seek(0);
    base = spanFileName(lo, hi, base.c_str());
  }
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
  // With a fleet up the STA (ESP-NOW) must stay alive and the AP must sit on the fleet channel (one radio).
  const bool flt = fleet::active();
  WiFi.mode(flt ? WIFI_AP_STA : WIFI_AP);
  // Fleet: one client at most (SoftAP + ESP-NOW share the hardware key slots, matters from P2).
  bool ok = WiFi.softAP(g_ssid, g_pw, flt ? fleet::channel() : 1, 0, flt ? 1 : 4);
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
  if (!g_evId) g_evId = WiFi.onEvent(onApEvent);
  g_active = true;
  Serial.printf("[webshare] softAP=%s SSID=%s ip=%s heap=%u serving=%s exists=%d\n",  // PSK deliberately not logged
                ok ? "OK" : "FAIL", g_ssid, ip.toString().c_str(),
                (unsigned)ESP.getFreeHeap(), g_logPath, SD.exists(g_logPath));
}

void stop() {
  if (!g_active) return;
  g_server.stop();
  if (g_evId) { WiFi.removeEvent(g_evId); g_evId = 0; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(fleet::active() ? WIFI_STA : WIFI_OFF);   // never tear down the fleet's STA
  fleet::retune();   // softAPdisconnect wrote AP channel 1 and retuned the radio: put the fleet back on its channel
  g_active = false;
  Serial.println("[webshare] AP down");
}

void        setLogPath(const char* path) {
  strncpy(g_logPath, path, sizeof(g_logPath) - 1);
  g_logPath[sizeof(g_logPath) - 1] = 0;
}
bool        active()   { return g_active; }
bool        clientConnected() { return g_active && WiFi.softAPgetStationNum() > 0; }
int         stations() { return g_active ? WiFi.softAPgetStationNum() : 0; }
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

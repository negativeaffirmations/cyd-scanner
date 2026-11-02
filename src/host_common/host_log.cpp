// host_log.cpp — see host_log.h. Code moved verbatim from the CYD main.cpp (docs/mini-build.md §4).
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <esp_random.h>
#include <stdarg.h>
#include "host_log.h"
#include "host_board.h"
#include "host_ui.h"
#include "host_files.h"
#include "logfilter.h"
#include "phone.h"
#include "fleet.h"
#include "webshare.h"
#include "whitelist.h"

// Copy a device-controlled free-text field, replacing delimiter-breaking characters
// (comma, tab, CR/LF and other control chars) with a space so plain delimiter-split
// parsing of the tab-delimited DETS rows stays correct (the log uses jsonEscape instead).
void sanitizeField(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; i + 1 < cap && src[i]; i++) {
    char c = src[i];
    dst[i] = (c == ',' || (uint8_t)c < 0x20 || c == 0x7F) ? ' ' : c;
  }
  dst[i] = 0;
}

// Escape a device-controlled string for use inside a JSON string literal (no surrounding
// quotes): " \ and control chars (<0x20, 0x7F) become escapes. Bounded and NUL-terminated;
// stops before a whole escape sequence would overflow, so output is always valid JSON.
void jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  for (size_t i = 0; src[i]; i++) {
    uint8_t c = (uint8_t)src[i];
    char esc[7];
    size_t n;
    if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; n = 2; }
    else if (c == '\n') { memcpy(esc, "\\n", 2); n = 2; }
    else if (c == '\r') { memcpy(esc, "\\r", 2); n = 2; }
    else if (c == '\t') { memcpy(esc, "\\t", 2); n = 2; }
    else if (c < 0x20 || c == 0x7F) { n = snprintf(esc, sizeof(esc), "\\u%04X", c); }
    else { esc[0] = (char)c; n = 1; }
    if (o + n + 1 > cap) break;
    memcpy(dst + o, esc, n);
    o += n;
  }
  if (cap) dst[o] = 0;
}

bool g_sdOk = false;
// Legacy CSV header (schema v5): only used to recognize header-only old .csv files in the sweep.
static const char* kLogHeader =
    "epoch,ms_since_boot,lat,lon,source,mac,rssi,channel,ie,cid,uuid,pan,name,score,tier,signature";

LogStream g_fg;  // zero-init (.bss); path set by openSession()
LogStream g_bg;
LogStream g_fleetLog;  // fleet leader: forwarded member lines (P6 item 6)
char g_streamingPath[100] = "";

// Blocking phone transfers (see XferScope below). Fleet event lines raised meanwhile (fleet::service() keeps running
// between chunks) are formatted at once but appended only after the transfer, so a file never grows between an
// export's counting pass and its send pass (the SIZE= header would undercount).
static uint8_t  g_xferDepth = 0;
static constexpr int XFER_DEFER_MAX = 8;
static char*    g_xferDefer = nullptr;            // XFER_DEFER_MAX x LOG_LINE, heap only while lines are held
static uint16_t g_xferDeferLen[XFER_DEFER_MAX];
static int      g_xferDeferN = 0, g_xferDeferDropped = 0;
static void appendActiveLine(const char* ln, int n);   // defined with logFleetEvent

// Keepalive for long non-sending passes (counting, directory scans) that serve a phone request: services the phone
// link + fleet (phone::transferAlive) at most every 20 ms and reports whether the requesting phone (ep = its
// authEpoch when the request started) is still the authenticated one. The send loops call transferAlive per chunk.
static bool xferAlive(uint32_t ep) {
  static uint32_t last = 0, lastEp = 0;
  static bool alive = true;
  if (ep == lastEp && millis() - last < 20) return alive;
  last = millis(); lastEp = ep;
  alive = phone::transferAlive(ep);
  return alive;
}
// Same servicing for sweeps no phone is waiting on (log cap, bulk delete).
static void xferKeepAlive() {
  static uint32_t last = 0;
  if (millis() - last < 20) return;
  last = millis();
  phone::service();
  fleet::service();
}

struct XferScope {
  XferScope() { g_xferDepth++; hostXferBegin(); }
  ~XferScope() {
    hostXferEnd();
    if (--g_xferDepth) return;
    for (int i = 0; i < g_xferDeferN; i++) appendActiveLine(g_xferDefer + i * LOG_LINE, g_xferDeferLen[i]);
    g_xferDeferN = 0;
    free(g_xferDefer); g_xferDefer = nullptr;
    if (g_xferDeferDropped) Serial.printf("[log] %d fleet line(s) dropped during a transfer (buffer full)\n", g_xferDeferDropped);
    g_xferDeferDropped = 0;
  }
};

bool logXferActive() { return g_xferDepth > 0; }

bool bgCtx() { return g_bgScan && !hostboard::foregroundScan(); }
LogStream& activeStream() { return bgCtx() ? g_bg : g_fg; }
void syncSharePath() {
  static char last[48] = "";
  const char* cur = activeStream().path;
  if (strcmp(last, cur) == 0) return;
  strncpy(last, cur, sizeof(last) - 1);
  webshare::setLogPath(cur);
}

// Short-address 15.4 devices are only unique within a PAN, so key them by panId too
// (pan = 0 for every other source, which keeps their key source+MAC).
static uint16_t seenPan(const Detection& d) {
  return (d.source == (uint8_t)Source::Ieee802154 && !(d.flags & FLAG_154_EXTENDED)) ? d.panId : 0;
}
// Index of this device in the stream's table, or -1.
static int seenFind(const LogStream& st, const Detection& d) {
  uint16_t pan = seenPan(d);
  for (int i = 0; i < st.seenCount; i++)
    if (st.seen[i].source == d.source && st.seen[i].pan == pan && memcmp(st.seen[i].mac, d.mac, 6) == 0) return i;
  return -1;
}
// Evicted entries still inside a presence period: their lastseen lines are written by
// logNewDetections() after it closes the cycle's file handle (one handle per file at a time).
// Bounded; in the (extreme) overflow case the oldest victims' lines are skipped, never their rows.
static constexpr int EVICT_Q = 16;  // a PRESENT victim means 400 devices heard within DEPART_MS: rare
static SeenKey g_evicted[EVICT_Q];
static int     g_evictedN = 0;

// Insert a device; when the table is full, evict the least-recently-SEEN entry (a device that is
// still in range is refreshed every cycle, so it is never the victim). Never fails. The new entry is
// "due" (lastLogged = ms - RELOG_MS); the caller stamps lastLogged once its row is actually written.
static int seenInsert(LogStream& st, const Detection& d, uint32_t ms) {
  if (!st.seen) return -1;  // table allocation failed at boot: can't dedup, so don't log (never spam)
  int idx = st.seenCount;
  if (idx < MAX_SEEN) {
    st.seenCount++;
  } else {
    idx = 0;
    for (int i = 1; i < MAX_SEEN; i++)
      if ((uint32_t)(ms - st.seen[i].lastSeen) > (uint32_t)(ms - st.seen[idx].lastSeen)) idx = i;
    if (st.seen[idx].present && g_evictedN < EVICT_Q) g_evicted[g_evictedN++] = st.seen[idx];
  }
  SeenKey& k = st.seen[idx];
  k.source = d.source;
  k.pan = seenPan(d);
  memcpy(k.mac, d.mac, 6);
  k.lastSeen = k.firstSeen = ms;
  k.lastLogged = ms - RELOG_MS;
  k.rssiMax = d.rssi;
  k.srcType = srcTypeOf(srcTag(d));
  k.present = 1;
  st.uniqCount++;
  return idx;
}

// Delete leftover header-only session logs (no detections were ever written). Past boots
// created a file up front, so a card can accumulate many ~90-byte empties; sweep them on
// startup. A file with even one data row is larger than the header and is kept.
static void sweepEmptySessions() {
  const size_t emptyMax = strlen(kLogHeader) + 2;  // legacy .csv: header + CRLF, nothing else
  File dir = SD.open("/logs");
  if (!dir) return;
  char victims[16][48];
  int nv = 0;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    // .jsonl has no header, so an empty one is 0 bytes; .csv is header-only.
    String en = e.name();
    bool jl = en.endsWith(".jsonl");
    if (!e.isDirectory() && e.size() <= (jl ? 0 : emptyMax) && nv < 16) {
      String nm = en;                             // may be a full path or a bare basename
      int slash = nm.lastIndexOf('/');
      String base = slash >= 0 ? nm.substring(slash + 1) : nm;
      snprintf(victims[nv++], sizeof(victims[0]), "/logs/%s", base.c_str());
    }
    e.close();
  }
  dir.close();
  for (int i = 0; i < nv; i++) {
    if (SD.remove(victims[i])) Serial.printf("[CYD] removed empty session log %s\n", victims[i]);
  }
}

// Start a fresh per-boot session log under /logs/. Named by a boot counter so it works
// before the phone has synced wall-clock time; renamed to /logs/YYYYMMDD-HHMMSS.jsonl once
// time is known (renameSessionOnSync). The file itself is created lazily on the first
// logged detection (ensureLogFile), so idle boots leave no empty file behind. Rows still
// carry absolute epoch after sync.
// Shared by boot + "New Session" + the background stream: bump the persistent counter (monotonic
// across boots and new sessions, so names never repeat; the bg stream has its own counter), adopt
// the counter name, reset that stream's dedup, and re-arm the time-sync rename (named=false) so the
// next renameSessionOnSync() gives it a date-time name if time is known.
// Session files that ENDED this boot (New Session / bg toggle). They keep valid ms-since-boot, so the
// time-window export can still date them, and renameSessionOnSync() appends a timesync anchor to
// any that ended before the clock was known (else their epoch:0 rows could never be dated).
struct BootFile { char path[48]; char fileId[9]; bool anchored; };
static constexpr int BOOT_FILES = 8;
static BootFile g_bootFiles[BOOT_FILES];
static int g_bootFileN = 0;  // ring write count
static void rememberBootFile(const LogStream& st) {
  if (!st.started || !st.path[0]) return;
  BootFile& b = g_bootFiles[g_bootFileN++ % BOOT_FILES];
  snprintf(b.path, sizeof(b.path), "%s", st.path);
  memcpy(b.fileId, st.fileId, sizeof(b.fileId));
  b.anchored = st.named;  // a date-named file was created/renamed after sync (rows dated or anchored)
}
int thisBootFiles(const char** out, int cap) {
  int n = 0;
  if (n < cap && g_fg.path[0]) out[n++] = g_fg.path;
  if (n < cap && g_bg.path[0]) out[n++] = g_bg.path;
  if (n < cap && g_fleetLog.path[0]) out[n++] = g_fleetLog.path;
  int have = g_bootFileN < BOOT_FILES ? g_bootFileN : BOOT_FILES;
  for (int i = 0; i < have && n < cap; i++) out[n++] = g_bootFiles[i].path;
  return n;
}

static void writeDepartures(LogStream& st, uint32_t ms, bool all);  // below, with the line writers
void beginSessionNamed(LogStream& st, bool bg) {
  writeDepartures(st, millis(), true);  // close out every presence period in the outgoing file
  rememberBootFile(st);
  if (!st.seen && (bg || !fleet::isClient())) {   // a fleet client never logs to fg: skip its ~9.6 KB dedup table
    st.seen = (SeenKey*)calloc(MAX_SEEN, sizeof(SeenKey));
    if (!st.seen) Serial.printf("[CYD] %s seen table: out of memory (logging disabled)\n", bg ? "bg" : "fg");
  }
  Preferences p; p.begin("cydscan", false);
  const char* key = bg ? "bgcnt" : "bootcnt";
  uint32_t n = p.getULong(key, 0) + 1;
  p.putULong(key, n);
  p.end();
  snprintf(st.path, sizeof(st.path), bg ? "/logs/bg-%05lu.jsonl" : "/logs/sess-%05lu.jsonl", (unsigned long)n);
  st.named        = false;   // re-arm rename-on-sync
  st.started      = false;   // file is created lazily on the first row (ensureLogFile)
  st.fileId[0]    = 0;       // a new file gets a new file_id when it is created
  st.seenCount    = 0;       // fresh per-session dedup
  st.uniqCount    = 0;
  st.rowCount     = 0;
  Serial.printf("[CYD] %s session log (created on first detection): %s\n", bg ? "bg" : "fg", st.path);
}

void openSession() {
  SD.mkdir("/logs");
  if (SD.exists("/scanlog.csv")) SD.remove("/scanlog.csv");  // retire the legacy single file (CSV era)
  sweepEmptySessions();
  beginSessionNamed(g_fg, false);
  resetFollow();          // a "following" judgement is per continuous trip
}

// Open the background stream's session (boot with bg on, and when bg is toggled on). A pending bg
// session that never wrote a row is reused rather than burning another counter value.
void openBgSession() {
  if (!g_sdOk) return;
  if (g_bg.path[0] && !g_bg.started) return;
  beginSessionNamed(g_bg, true);
}

// Start a fresh session without rebooting (Scan menu / phone CMD "N"). Rotates the stream the
// current screen logs to (the Scan menu passes forceFg so its New Session always means the manual
// foreground session). The writer opens with FILE_APPEND and closes every cycle, so there is no
// handle to flush. An unused previous session left no file (lazy create), so nothing is orphaned.
void startNewSession(bool forceFg) {
  if (!forceFg && bgCtx()) beginSessionNamed(g_bg, true);
  else                     beginSessionNamed(g_fg, false);
  resetFollow();
  enforceLogCap();        // rotation trigger: trim /logs/ to the cap
  syncSharePath();
}

// Create the (empty) session file the first time a row needs writing. NDJSON has no header, but we
// write a one-line {"evt":"meta"} provenance anchor as the FIRST line (scan-analyzer tags each
// session with the scanner + sig-DB generation that produced it; readers skip it by its "evt" like
// the timesync anchor). No-op once created. Returns false if SD is unavailable or can't be opened.
static void writeSessionMeta(File& f, const LogStream& st);  // defined with FW_VERSION/DB_GEN below
bool ensureLogFile(LogStream& st) {
  if (st.started) return true;
  if (!g_sdOk || !st.path[0]) return false;
  File f = SD.open(st.path, FILE_WRITE);
  if (!f) return false;
  if (!st.fileId[0]) {   // P6 item 6: the file's own random id, fixed for its life (survives the sync rename)
    uint8_t b[4];
    esp_fill_random(b, sizeof(b));
    snprintf(st.fileId, sizeof(st.fileId), "%02x%02x%02x%02x", b[0], b[1], b[2], b[3]);
  }
  writeSessionMeta(f, st);
  f.close();
  st.started = true;
  Serial.printf("[CYD] session log created: %s\n", st.path);
  return true;
}

// Once the phone provides wall-clock time, rename a boot-counter session file to a date-time name
// (fg: YYYYMMDD-HHMMSS.jsonl, bg: bg-YYYYMMDD-HHMMSS.jsonl). Runs once per session (even if the
// rename fails, it won't retry-spam).
static void renameStreamOnSync(LogStream& st, const char* prefix) {   // prefix: "" fg, "bg-", "fleet-"
  if (st.named || !st.path[0] || !g_sdOk || !timeHasSync()) return;
  time_t t = (time_t)epochLocal();
  struct tm tmv;
  gmtime_r(&t, &tmv);  // epoch is stored already-localized, so gmtime gives local fields
  char nn[48];
  snprintf(nn, sizeof(nn), "/logs/%s%04d%02d%02d-%02d%02d%02d.jsonl", prefix,
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  if (SD.exists(nn)) { st.named = true; return; }  // name taken; keep the current one
  if (!st.started) {
    // File not created yet (no detections logged): just adopt the date-time name so the
    // file is born correctly named when the first row is written.
    strncpy(st.path, nn, sizeof(st.path) - 1);
    st.path[sizeof(st.path) - 1] = 0;
    Serial.printf("[CYD] session log will use %s\n", st.path);
  } else {
    // This file already holds pre-sync rows (epoch:0, ms = ms-since-boot). Append a one-line
    // timesync ANCHOR so a reader can backfill their real time: epoch = sync.epoch +
    // (row.ms - sync.ms)/1000 (same formula epochNow() uses; valid because it's one boot).
    // (Not the fleet file: its rows are members' lines on foreign clocks, each origin's own forwarded timesync dates them.)
    File mf;
    if (&st != &g_fleetLog) mf = SD.open(st.path, FILE_APPEND);
    if (mf) {
      mf.printf("{\"epoch\":%lu,\"ms\":%lu,\"evt\":\"timesync\"}\n",
                (unsigned long)epochLocal(), (unsigned long)millis());
      mf.close();
    }
    if (SD.rename(st.path, nn)) {
      Serial.printf("[CYD] session log renamed %s -> %s\n", st.path, nn);
      strncpy(st.path, nn, sizeof(st.path) - 1);
      st.path[sizeof(st.path) - 1] = 0;
    }
  }
  st.named = true;
}
void renameSessionOnSync() {
  renameStreamOnSync(g_fg, "");
  renameStreamOnSync(g_bg, "bg-");
  renameStreamOnSync(g_fleetLog, "fleet-");
  if (g_sdOk && timeHasSync()) {  // earlier this-boot files that ended pre-sync: anchor them too
    int have = g_bootFileN < BOOT_FILES ? g_bootFileN : BOOT_FILES;
    for (int i = 0; i < have; i++) {
      BootFile& b = g_bootFiles[i];
      if (b.anchored) continue;
      b.anchored = true;
      if (!SD.exists(b.path)) continue;
      File mf = SD.open(b.path, FILE_APPEND);
      if (!mf) continue;
      mf.printf("{\"epoch\":%lu,\"ms\":%lu,\"evt\":\"timesync\"}\n",
                (unsigned long)epochLocal(), (unsigned long)millis());
      mf.close();
      Serial.printf("[CYD] timesync anchor appended to earlier session %s\n", b.path);
    }
  }
  syncSharePath();
}

static void reseedDbIfNeeded() {
  Preferences p;
  p.begin("cydscan", false);
  uint32_t gen = p.getULong("dbgen", 0);
  if (gen != DB_GEN) {
    if (SD.exists("/signatures.csv")) SD.remove("/signatures.csv");
    p.putULong("dbgen", DB_GEN);
    Serial.printf("[CYD] DB reseed (gen %lu -> %lu); signatures.csv rewritten from firmware\n",
                  (unsigned long)gen, (unsigned long)DB_GEN);
  }
  p.end();
}

// Stable per-unit scanner id: the 48-bit factory eFuse MAC as 12 lowercase hex chars (computed once).
// Globally unique and constant across reflashes. OPAQUE — this is the BASE MAC, so it won't equal any
// scanned `mac` row (the BLE/Wi-Fi interface MACs derive from it with offsets). First-party: the
// owner's own device, not bystander data.
const char* scannerIdHex() {
  static char id[13] = {0};
  if (!id[0]) {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(id, sizeof(id), "%04x%08x", (unsigned)(uint16_t)(mac >> 32), (unsigned)(uint32_t)mac);
  }
  return id;
}

const char* scannerName() {
  static char nm[24] = {0};
  if (!nm[0]) {
    static const char kCons[] = "BDFGHKLMNPRSTVZ";  // 15 (no C/J/Q/W/X/Y: unambiguous when read aloud)
    static const char kVow[]  = "AEIOU";
    uint64_t mac = ESP.getEfuseMac();
    uint32_t h = 2166136261u;                       // FNV-1a over the 6 MAC bytes
    for (int i = 0; i < 6; i++) h = (h ^ (uint8_t)(mac >> (8 * i))) * 16777619u;
    char w[7];
    for (int i = 0; i < 3; i++) {                   // 3 consonant-vowel syllables: 75^3 = 421,875 words
      w[2 * i]     = kCons[h % 15]; h /= 15;
      w[2 * i + 1] = kVow[h % 5];   h /= 5;
    }
    w[6] = 0;
    snprintf(nm, sizeof(nm), "CFLO-%s-%s", hostboard::boardTag(), w);
  }
  return nm;
}

// Provenance anchor written as the first line of every session stream (see ensureLogFile). scanner_name
// = scannerName() (the advertised unit name; scanner_id stays the stable key).
// P6 item 1: + fleet_role while a fleet role is set, and fleet_id / fleet_leader while in a fleet session. A client
// that joins after the file was created gets them from its "join" evt line instead.
// P6 item 6: + "stream":"fleet" on the leader's fleet file, and "file_id" (the file's random id) last on every stream.
static void writeSessionMeta(File& f, const LogStream& st) {
  f.printf("{\"evt\":\"meta\",\"scanner_id\":\"%s\",\"scanner_name\":\"%s\",\"fw\":\"%s\",\"db_gen\":%lu",
           scannerIdHex(), scannerName(), FW_VERSION, (unsigned long)DB_GEN);
  if (&st == &g_fleetLog) f.print(",\"stream\":\"fleet\"");
  if (fleet::role() != fleet::ROLE_OFF) {
    f.printf(",\"fleet_role\":\"%s\"", fleet::role() == fleet::ROLE_LEADER ? "leader" : "client");
    char id[17], leader[13];
    if (fleet::fleetIdent(id, leader)) f.printf(",\"fleet_id\":\"%s\",\"fleet_leader\":\"%s\"", id, leader);
  }
  if (st.fileId[0]) f.printf(",\"file_id\":\"%s\"", st.fileId);
  f.print("}\n");
}

void initSD() {
  g_sdOk = hostboard::sdBegin();  // board-specific SPI pins + SD.begin()
  if (!g_sdOk) { Serial.println("[CYD] SD unavailable - logging disabled"); return; }
  reseedDbIfNeeded();  // delete an outdated /signatures.csv so sigdb::begin() re-seeds it
  openSession();       // create this boot's /logs/sess-NNNNN.jsonl (renamed on time sync)
  if (g_bgScan) openBgSession();  // and the background stream /logs/bg-NNNNN.jsonl
  enforceLogCap();     // rotation trigger: boot
  syncSharePath();
  Serial.println("[CYD] SD ready");
}

// Append to an NDJSON line buffer. snprintf returns the would-be length, so a plain
// "n += snprintf(ln+n, sizeof-n, ...)" chain underflows once n passes the buffer; instead
// any truncation parks n at cap+1 (sticky), which the caller treats as "drop this line".
static void lnAppend(char* ln, int& n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void lnAppend(char* ln, int& n, const char* fmt, ...) {
  if (n > (int)LOG_LINE) return;  // already overflowed
  va_list ap; va_start(ap, fmt);
  int w = vsnprintf(ln + n, LOG_LINE - n, fmt, ap);
  va_end(ap);
  if (w < 0 || n + w >= (int)LOG_LINE) n = (int)LOG_LINE + 1;
  else n += w;
}

// Scanner position for a log line: ,"lat":..,"lon":.. when any fix exists, plus ,"fix_age":<s> when that
// fix is HELD (no live source; the last known one is reused) or the fix in use is > 10 s old (e.g. a
// phone between sparse updates), so readers can tell an inferred position from a live one. Omitted
// entirely with no fix ever.
static void appendPos(char* ln, int& n) {
  PosFix f = posSnapshot();  // one snapshot: lat, lon and fix_age always describe the same fix
  if (!f.src || !isfinite(f.lat) || !isfinite(f.lon)) return;
  lnAppend(ln, n, ",\"lat\":%.6f,\"lon\":%.6f", f.lat, f.lon);
  if (f.aged) lnAppend(ln, n, ",\"fix_age\":%lu", (unsigned long)(f.ageMs / 1000));
  // P6 item 4: whose fix this is. "fleet" = the leader's shared position (a client), or a member's GPS (a leader).
  if (f.from) lnAppend(ln, n, ",\"pos_src\":\"%s\"", f.from == 1 ? "phone" : f.from == 2 ? "gps" : "fleet");
}

int logNewDetections() {
  int n = 0;
  LogStream& st = activeStream();  // headless bg cycle -> bg stream; on-screen scan -> fg stream
  if (bgCtx() && !st.path[0]) openBgSession();
  File f;  // opened lazily on the first new detection so idle boots write no file
  uint32_t ms = millis();
  uint32_t epoch = epochLocal();
  for (int i = 0; i < g_detCount; i++) {
    if (!(g_detVia[i] & 1)) continue;   // fleet: a member's row is logged on ITS card, never twice (docs §9.5)
    const Detection& d = g_dets[i];
    int si = seenFind(st, d);
    if (si >= 0) {
      SeenKey& k = st.seen[si];
      if (!k.present) { k.present = 1; k.firstSeen = ms; k.rssiMax = d.rssi; }  // back: new presence period
      else if (d.rssi > k.rssiMax) k.rssiMax = d.rssi;
      k.lastSeen = ms;
      if (ms - k.lastLogged < RELOG_MS) continue;  // logged recently: no new row yet
    } else {                                                  // (else: a due re-sighting row)
      si = seenInsert(st, d, ms);
      if (si < 0) continue;
      n++;  // "new" = first sighting this session
    }
    if (!f && ensureLogFile(st)) f = SD.open(st.path, FILE_APPEND);
    if (!f) continue;
    char safe[97];  // escaped name (32 B name, worst case \uXXXX per byte truncates safely)
    jsonEscape(safe, sizeof(safe), d.name);
    const sigdb::ScoreResult& sc = g_score[i];
    char sig[49];
    jsonEscape(sig, sizeof(sig), detLabel(i));
    char flagStr[64]; flagTokens(d.flags, flagStr, sizeof(flagStr));
    char panStr[5]; panStr[0] = 0;  // hex PAN, 802.15.4 only
    if (d.source == (uint8_t)Source::Ieee802154 && d.panId) snprintf(panStr, sizeof(panStr), "%04X", d.panId);
    char uuidStr[33]; uuidStr[0] = 0;
    for (int k = 0; k < 16; k++)
      if (d.svc[k]) { for (int j = 0; j < 16; j++) sprintf(uuidStr + j * 2, "%02X", d.svc[j]); break; }
    // Whitelist state of this device, looked up per-group by MAC (buildGroups() ran this
    // cycle). Stamped into the log so downstream tools can filter muted detections.
    bool wl = false;
    for (int k = 0; k < g_groupCount; k++)
      if (memcmp(g_groups[k].mac, d.mac, 6) == 0) { wl = g_groups[k].whitelisted; break; }
    // One NDJSON object per line; optional keys are omitted when blank. Worst case (every
    // optional key + a fully escaped name): ~60 B fixed/numeric + 96 name + 48 sig + 32 uuid
    // + key text ~ 440-480 B, inside the 512 B buffer. lnAppend() is overflow-proof anyway.
    char ln[LOG_LINE];
    int n2 = 0;
    lnAppend(ln, n2, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epoch, (unsigned long)ms);
    appendPos(ln, n2);
    lnAppend(ln, n2,
             ",\"src\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d,\"ch\":%d",
             srcTag(d), d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5],
             d.rssi, d.channel);
    if (d.flags & FLAG_WIFI_DATA_CLIENT)  // the AP this associated client talks to (v11)
      lnAppend(ln, n2, ",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
               d.bssid[0], d.bssid[1], d.bssid[2], d.bssid[3], d.bssid[4], d.bssid[5]);
    if (d.ie_hash)   lnAppend(ln, n2, ",\"ie\":\"%08lX\"", (unsigned long)d.ie_hash);
    if (d.companyId) lnAppend(ln, n2, ",\"cid\":\"%04X\"", d.companyId);
    if (uuidStr[0])  lnAppend(ln, n2, ",\"uuid\":\"%s\"", uuidStr);
    if (panStr[0])   lnAppend(ln, n2, ",\"pan\":\"%s\"", panStr);
    if (safe[0])     lnAppend(ln, n2, ",\"name\":\"%s\"", safe);
    lnAppend(ln, n2, ",\"score\":%d,\"tier\":\"%s\"", sc.score, sigdb::tierName(sc.tier));
    if (sig[0])      lnAppend(ln, n2, ",\"sig\":\"%s\"", sig);
    if (flagStr[0])  lnAppend(ln, n2, ",\"flags\":\"%s\"", flagStr);
    lnAppend(ln, n2, ",\"wl\":%d", wl ? 1 : 0);
    if (n2 > (int)LOG_LINE - 2) continue;  // can't close the object in-buffer: drop the row, never write truncated JSON
    ln[n2++] = '}'; ln[n2++] = '\n';
    if (f.write((const uint8_t*)ln, n2) != (size_t)n2) continue;  // not written: stays due, retried next cycle
    st.seen[si].lastLogged = ms;
    st.rowCount++;
  }
  if (f) f.close();
  // Departure lines: LRU victims from this cycle, then devices quiet for DEPART_MS -- in BOTH streams
  // (the inactive stream's devices go quiet too and are closed out into its own file).
  writeDepartures(st, ms, false);
  LogStream& other = (&st == &g_fg) ? g_bg : g_fg;
  if (other.seenCount) writeDepartures(other, ms, false);
  return n;
}

// One {"evt":"lastseen"} line for a presence period (see DEPART_MS in host_log.h).
static void lastSeenLine(File& f, const SeenKey& k, uint32_t epoch, uint32_t ms) {
  static const char* const kTag[] = {"2.4", "5G", "BLE", "PRB", "154"};
  char ln[LOG_LINE];
  int n = 0;
  lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epoch, (unsigned long)ms);
  appendPos(ln, n);
  lnAppend(ln, n, ",\"evt\":\"lastseen\",\"src\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
           kTag[k.srcType < 5 ? k.srcType : 0], k.mac[0], k.mac[1], k.mac[2], k.mac[3], k.mac[4], k.mac[5]);
  if (k.source == (uint8_t)Source::Ieee802154 && k.pan) lnAppend(ln, n, ",\"pan\":\"%04X\"", k.pan);
  lnAppend(ln, n, ",\"first_ms\":%lu,\"last_ms\":%lu,\"rssi_max\":%d}\n",
           (unsigned long)k.firstSeen, (unsigned long)k.lastSeen, (int)k.rssiMax);
  if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
}

// Close out presence periods in `st`: the queued LRU victims (active stream only), plus every present
// entry quiet for >= DEPART_MS -- or ALL present entries when `all` (session end).
static void writeDepartures(LogStream& st, uint32_t ms, bool all) {
  bool evict = (&st == &activeStream()) && g_evictedN > 0;
  bool any = evict;
  for (int i = 0; i < st.seenCount && !any; i++)
    if (st.seen[i].present && (all || ms - st.seen[i].lastSeen >= DEPART_MS)) any = true;
  if (!any) return;
  if (!g_sdOk || !st.path[0] || !ensureLogFile(st)) { if (evict) g_evictedN = 0; return; }
  File f = SD.open(st.path, FILE_APPEND);
  if (!f) { if (evict) g_evictedN = 0; return; }
  uint32_t epoch = epochLocal();
  if (evict) {
    for (int i = 0; i < g_evictedN; i++) lastSeenLine(f, g_evicted[i], epoch, ms);
    g_evictedN = 0;
  }
  for (int i = 0; i < st.seenCount; i++) {
    SeenKey& k = st.seen[i];
    if (!k.present || !(all || ms - k.lastSeen >= DEPART_MS)) continue;
    lastSeenLine(f, k, epoch, ms);
    k.present = 0;
  }
  f.close();
}

// Append one event line to the current session log:
//   {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"deauth"|"eviltwin","count":N[,"ssid":..,"macs":".."]}
// For "eviltwin" we also name the suspect SSID + its BSSIDs (comma-joined, up to EVIL_MAX_BSSID),
// pulled from the live detections that currently carry FLAG_WIFI_EVILTWIN, so the map/log identifies
// the culprit APs instead of a bare count. (The "deauth" event has no per-AP address — the C5 only
// counts frames.) No "mac" key, so the Explore row parser skips it; the web Map renders it as a pin.
// Per-device "rid" and "follow" event lines have their own writers below (logRidEvents / logFollowEvents).
void logEvent(const char* evt, int count) {
  LogStream& st = activeStream();
  if (bgCtx() && !st.path[0]) openBgSession();
  if (!ensureLogFile(st)) return;
  File f = SD.open(st.path, FILE_APPEND);
  if (!f) return;
  char ln[LOG_LINE];
  int n = 0;
  lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epochLocal(), (unsigned long)millis());
  appendPos(ln, n);
  lnAppend(ln, n, ",\"evt\":\"%s\",\"count\":%d", evt, count);
  if (!strcmp(evt, "eviltwin")) {
    static constexpr int EVIL_MAX_BSSID = 8;
    char ssid[97] = {0};
    int emitted = 0;
    for (int i = 0; i < g_detCount; i++) {
      if (!(g_detVia[i] & 1) || !(g_dets[i].flags & FLAG_WIFI_EVILTWIN)) continue;   // this unit's own BSSIDs only
      if (!ssid[0] && g_dets[i].name[0]) jsonEscape(ssid, sizeof(ssid), g_dets[i].name);
      if (emitted >= EVIL_MAX_BSSID) continue;
      const uint8_t* m = g_dets[i].mac;
      if (emitted == 0) lnAppend(ln, n, ",\"macs\":\"");
      lnAppend(ln, n, "%s%02X:%02X:%02X:%02X:%02X:%02X", emitted ? "," : "",
               m[0], m[1], m[2], m[3], m[4], m[5]);
      emitted++;
    }
    if (emitted) lnAppend(ln, n, "\"");
    if (ssid[0]) lnAppend(ln, n, ",\"ssid\":\"%s\"", ssid);
  }
  lnAppend(ln, n, "}\n");
  if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
  f.close();
}

// Audit line for a change the PHONE made to device config / card files (SEC-H3, docs/ble-auth.md):
//   {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"phonecfg","op":"upload|delete|rename|mkdir|wladd|wlrm|reload|rekey"
//    [,"path":"..."[,"to":"..."]]}   (rename = path + to; path omitted when not applicable)
// Written to the active stream so the scan log records who-changed-what alongside the sightings.
void logPhoneCfg(const char* op, const char* path, const char* to) {
  LogStream& st = activeStream();
  if (bgCtx() && !st.path[0]) openBgSession();
  if (!g_sdOk || !ensureLogFile(st)) return;
  File f = SD.open(st.path, FILE_APPEND);
  if (!f) return;
  char ln[LOG_LINE];
  int n = 0;
  lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epochLocal(), (unsigned long)millis());
  appendPos(ln, n);
  lnAppend(ln, n, ",\"evt\":\"phonecfg\",\"op\":\"%s\"", op);
  char esc[200];
  if (path && path[0]) { jsonEscape(esc, sizeof(esc), path); lnAppend(ln, n, ",\"path\":\"%s\"", esc); }
  if (to && to[0])     { jsonEscape(esc, sizeof(esc), to);   lnAppend(ln, n, ",\"to\":\"%s\"", esc); }
  if (n <= (int)LOG_LINE - 2) {  // room to close the object; else drop (never write truncated JSON)
    lnAppend(ln, n, "}\n");
    if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
  }
  f.close();
}

// {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"fleet","op":"..","role":"client|leader"} -- fleet mode transitions
// (docs/fleet-mode.md §9.5). scan-analyzer needs it to explain a role-limited client file.
static void appendActiveLine(const char* ln, int n) {
  LogStream& st = activeStream();
  if (bgCtx() && !st.path[0]) openBgSession();
  if (!g_sdOk || !ensureLogFile(st)) return;
  File f = SD.open(st.path, FILE_APPEND);
  if (!f) return;
  f.write((const uint8_t*)ln, n);
  f.close();
}

void logFleetEvent(const char* op, const char* role, const fleet::EventInfo* x) {
  char ln[LOG_LINE];
  int n = 0;
  lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epochLocal(), (unsigned long)millis());
  appendPos(ln, n);
  lnAppend(ln, n, ",\"evt\":\"fleet\",\"op\":\"%s\",\"role\":\"%s\"", op,
           role ? role : fleet::role() == fleet::ROLE_LEADER ? "leader" : "client");
  char id[17], leader[13];
  if (fleet::fleetIdent(id, leader)) lnAppend(ln, n, ",\"fleet_id\":\"%s\",\"fleet_leader\":\"%s\"", id, leader);
  if (x) {
    if (x->peer[0]) lnAppend(ln, n, ",\"peer\":\"%s\"", x->peer);
    if (x->peerName[0]) { char esc[6 * 20 + 1]; jsonEscape(esc, sizeof(esc), x->peerName); lnAppend(ln, n, ",\"peer_name\":\"%s\"", esc); }
    if (x->slot >= 0) lnAppend(ln, n, ",\"slot\":%d", x->slot);
    if (x->n >= 0) lnAppend(ln, n, ",\"n\":%d", x->n);
  }
  if (!strcmp(op, "plan") || !strcmp(op, "start")) {   // not on a client join: its Plan arrives right after
    char plan[48]; fleet::planString(plan, sizeof(plan));
    lnAppend(ln, n, ",\"plan\":\"%s\"", plan);
  }
  if (n > (int)LOG_LINE - 2) return;   // no room to close the object: drop (never write truncated JSON)
  lnAppend(ln, n, "}\n");
  if (n > (int)LOG_LINE) return;
  // Mid-transfer: keep the line (its own epoch/ms) for after -- except radio_fail, which a reboot follows at once.
  if (g_xferDepth && strcmp(op, "radio_fail")) {
    if (!g_xferDefer) g_xferDefer = (char*)malloc(XFER_DEFER_MAX * LOG_LINE);
    if (!g_xferDefer || g_xferDeferN >= XFER_DEFER_MAX) { g_xferDeferDropped++; return; }
    memcpy(g_xferDefer + g_xferDeferN * LOG_LINE, ln, n);
    g_xferDeferLen[g_xferDeferN++] = (uint16_t)n;
    return;
  }
  appendActiveLine(ln, n);
}

// Log one "rid" event line per Remote-ID emitter, rate-limited per MAC to once per EVENT_LOG_GAP_MS.
//   {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"rid","mac":"..",["uas":..,"dlat":..,"dlon":..,"h":..,
//    "alt":..,"spd":..,"hdg":..,"olat":..,"olon":..,"oid":..]}  (unknown fields omitted)
// Worst case (two 20-char strings escaping to ~120 B each) stays inside LOG_LINE; lnAppend is overflow-safe.
void logRidEvents() {
  static uint8_t  ringMac[MAX_RID][6];
  static uint32_t ringLast[MAX_RID];
  static bool     ringUsed[MAX_RID];
  static int      ringNext = 0;
  uint32_t now = millis();
  for (int i = 0; i < g_ridStoreCount; i++) {
    if (!(g_ridVia[i] & 1)) continue;   // a member's Remote-ID is logged by that member
    const RemoteId& r = g_rids[i];
    int slot = -1;
    for (int k = 0; k < MAX_RID; k++)
      if (ringUsed[k] && memcmp(ringMac[k], r.mac, 6) == 0) { slot = k; break; }
    if (slot >= 0 && now - ringLast[slot] < EVENT_LOG_GAP_MS) continue;
    if (slot < 0) {  // first sighting: take the next ring slot (round-robin eviction)
      slot = ringNext; ringNext = (ringNext + 1) % MAX_RID;
      memcpy(ringMac[slot], r.mac, 6); ringUsed[slot] = true;
    }
    ringLast[slot] = now;

    LogStream& st = activeStream();
    if (bgCtx() && !st.path[0]) openBgSession();
    if (!ensureLogFile(st)) return;
    File f = SD.open(st.path, FILE_APPEND);
    if (!f) return;
    char uas[LOG_ID_ESC], oid[LOG_ID_ESC];
    jsonEscape(uas, sizeof(uas), r.uasId);
    jsonEscape(oid, sizeof(oid), r.operatorId);
    char ln[LOG_LINE];
    int n = 0;
    lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epochLocal(), (unsigned long)millis());
    appendPos(ln, n);
    lnAppend(ln, n, ",\"evt\":\"rid\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\"",
             r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
    if (uas[0]) lnAppend(ln, n, ",\"uas\":\"%s\"", uas);
    if (r.droneLat != RID_LL_UNK || r.droneLon != RID_LL_UNK)
      lnAppend(ln, n, ",\"dlat\":%.6f,\"dlon\":%.6f", r.droneLat / 1e7, r.droneLon / 1e7);
    if (r.heightM != RID_ALT_UNK) lnAppend(ln, n, ",\"h\":%d", r.heightM);
    if (r.altGeoM != RID_ALT_UNK) lnAppend(ln, n, ",\"alt\":%d", r.altGeoM);
    if (r.headingDeg != RID_HDG_UNK)
      lnAppend(ln, n, ",\"spd\":%.1f,\"hdg\":%u", r.speedCms / 100.0, (unsigned)r.headingDeg);
    if (r.operatorLat != RID_LL_UNK || r.operatorLon != RID_LL_UNK)
      lnAppend(ln, n, ",\"olat\":%.6f,\"olon\":%.6f", r.operatorLat / 1e7, r.operatorLon / 1e7);
    if (oid[0]) lnAppend(ln, n, ",\"oid\":\"%s\"", oid);
    if (n <= (int)LOG_LINE - 2) {  // room to close the object; else drop (never write truncated JSON)
      lnAppend(ln, n, "}\n");
      if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
    }
    f.close();
  }
}

// Log one "follow" event line per device currently FOLLOWING (ftier 2), rate-limited per MAC to once
// per EVENT_LOG_GAP_MS. Same ring-buffer idiom as logRidEvents().
//   {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"follow","mac":"..","score":N,"ftier":2
//    [,"name":"..","src":"..","flags":".."]}  (blank/unknown fields omitted)
// "flags" is the same +-joined detector list as the detection rows (tells a Find-My iPhone from a car).
void logFollowEvents() {
  if (followAlertN() <= 0) return;
  static uint8_t  ringMac[MAX_DET][6];
  static uint32_t ringLast[MAX_DET];
  static bool     ringUsed[MAX_DET];
  static int      ringNext = 0;
  uint32_t now = millis();
  for (int i = 0; i < g_followCount; i++) {
    const FollowState& fs = g_follow[i];
    if (fs.ftier != 2) continue;
    int slot = -1;
    for (int k = 0; k < MAX_DET; k++)
      if (ringUsed[k] && memcmp(ringMac[k], fs.mac, 6) == 0) { slot = k; break; }
    if (slot >= 0 && now - ringLast[slot] < EVENT_LOG_GAP_MS) continue;
    if (slot < 0) {  // first sighting: take the next ring slot (round-robin eviction)
      slot = ringNext; ringNext = (ringNext + 1) % MAX_DET;
      memcpy(ringMac[slot], fs.mac, 6); ringUsed[slot] = true;
    }
    ringLast[slot] = now;

    // Device facts from its DevGroup (same MAC lookup as computeFollowScores()).
    char safe[97]; safe[0] = 0;
    char flagStr[64]; flagStr[0] = 0;
    const char* src = "";
    for (int k = 0; k < g_groupCount; k++) {
      const DevGroup& g = g_groups[k];
      if (memcmp(g.mac, fs.mac, 6) != 0) continue;
      if (g.nameIdx >= 0 && g.nameIdx < g_detCount) jsonEscape(safe, sizeof(safe), g_dets[g.nameIdx].name);
      if (g.rep >= 0 && g.rep < g_detCount) src = srcTag(g_dets[g.rep]);
      flagTokens(g.flags, flagStr, sizeof(flagStr));
      break;
    }

    LogStream& st = activeStream();
    if (bgCtx() && !st.path[0]) openBgSession();
    if (!ensureLogFile(st)) return;
    File f = SD.open(st.path, FILE_APPEND);
    if (!f) return;
    char ln[LOG_LINE];
    int n = 0;
    lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epochLocal(), (unsigned long)millis());
    appendPos(ln, n);
    lnAppend(ln, n, ",\"evt\":\"follow\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"score\":%d,\"ftier\":2",
             fs.mac[0], fs.mac[1], fs.mac[2], fs.mac[3], fs.mac[4], fs.mac[5], (int)fs.score);
    if (safe[0])    lnAppend(ln, n, ",\"name\":\"%s\"", safe);
    if (src[0])     lnAppend(ln, n, ",\"src\":\"%s\"", src);
    if (flagStr[0]) lnAppend(ln, n, ",\"flags\":\"%s\"", flagStr);
    if (n <= (int)LOG_LINE - 2) {  // room to close the object; else drop (never write truncated JSON)
      lnAppend(ln, n, "}\n");
      if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
    }
    f.close();
  }
}

// Stream an SD file to the phone over BLE: a "SIZE=<n>" header, then the raw file in
// chunks. The web app reassembles and saves it (no Wi-Fi needed).
static void streamFileOverBle(const char* path) {
  strncpy(g_streamingPath, path, sizeof(g_streamingPath) - 1);  // guard: not rotated away mid-stream
  g_streamingPath[sizeof(g_streamingPath) - 1] = 0;
  if (!g_sdOk || !SD.exists(path)) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE dl: no file %s\n", path);
    g_streamingPath[0] = 0;
    return;
  }
  File f = SD.open(path, "r");
  if (!f) { phone::logNotify((const uint8_t*)"SIZE=0", 6); g_streamingPath[0] = 0; return; }
  size_t sz = f.size();
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SIZE=%u", (unsigned)sz);
  phone::logNotify((const uint8_t*)hdr, hn);
  delay(30);
  uint8_t buf[180];  // chunk < MTU-3 (Android negotiates a large MTU)
  size_t sent = 0;
  const uint32_t ep = phone::authEpoch();   // stop if the peer leaves / re-authenticates mid-transfer
  while (f.available() && phone::transferAlive(ep)) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    phone::logNotify(buf, n);
    sent += n;
    delay(15);
  }
  f.close();
  Serial.printf("[CYD] BLE sent %u/%u bytes of %s\n", (unsigned)sent, (unsigned)sz, path);
  g_streamingPath[0] = 0;
}

// The phone reassembles until it has exactly SIZE= bytes. If a send pass came up short (an SD read
// hiccup ended a file early), pad with newlines -- NDJSON readers skip blank lines -- so the phone's
// transfer completes instead of hanging. (A dropped link can't be helped; the phone times out.)
static void padToSize(uint32_t& sent, uint32_t total, uint32_t ep) {
  if (sent >= total || !phone::transferAlive(ep)) return;
  Serial.printf("[CYD] BLE export short by %u bytes: padding\n", (unsigned)(total - sent));
  uint8_t nl[180];
  memset(nl, '\n', sizeof(nl));
  while (sent < total && phone::transferAlive(ep)) {
    uint32_t k = total - sent > sizeof(nl) ? sizeof(nl) : total - sent;
    phone::logNotify(nl, k);
    sent += k;
    delay(15);
  }
}

// Time-filtered BLE export: filter ON the CYD, never stream the whole file. TWO passes over the
// file -- pass 1 sums the byte length of the lines inside the window (for the SIZE= header), pass 2
// streams only those lines. Same LineReader + predicate for both, so the totals always agree.
static void streamFilteredOverBle(const char* path, const logfilter::Cutoff& c) {
  const uint32_t ep = phone::authEpoch();   // the requesting phone: a new peer must never get this reply
  if (!g_sdOk || !SD.exists(path)) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  File f = SD.open(path, "r");
  if (!f) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  strncpy(g_streamingPath, path, sizeof(g_streamingPath) - 1);
  g_streamingPath[sizeof(g_streamingPath) - 1] = 0;
  char line[logfilter::LINE_CAP];
  size_t len;
  uint32_t total = 0, nlines = 0;
  bool alive = true;
  {
    logfilter::LineReader rd(f);
    while (rd.next(line, len) && (alive = xferAlive(ep))) if (logfilter::keep(line, c)) { total += len; nlines++; }
  }
  if (!alive) { f.close(); g_streamingPath[0] = 0; Serial.println("[CYD] BLE filtered: phone left while counting"); return; }
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SIZE=%u", (unsigned)total);
  phone::logNotify((const uint8_t*)hdr, hn);
  if (total == 0) { f.close(); g_streamingPath[0] = 0; Serial.printf("[CYD] BLE filtered: no rows in range (%s)\n", path); return; }
  delay(30);
  f.seek(0);
  uint8_t out[180];  // chunk < MTU-3
  size_t on = 0;
  uint32_t sent = 0;
  logfilter::LineReader rd2(f);
  while (rd2.next(line, len) && phone::transferAlive(ep)) {
    if (!logfilter::keep(line, c)) continue;
    for (size_t off = 0; off < len; ) {
      size_t rem = len - off, avail = sizeof(out) - on;
      size_t n = rem < avail ? rem : avail;  // (not min(): Arduino's min macro trips IntelliSense here)
      memcpy(out + on, line + off, n);
      on += n; off += n;
      if (on == sizeof(out)) { phone::logNotify(out, on); sent += on; on = 0; delay(15); }
    }
  }
  if (on) { phone::logNotify(out, on); sent += on; }
  padToSize(sent, total, ep);
  f.close();
  g_streamingPath[0] = 0;
  Serial.printf("[CYD] BLE filtered sent %u/%u bytes (%u lines) of %s\n", (unsigned)sent, (unsigned)total,
                (unsigned)nlines, path);
}

// The scan-provenance meta line (same shape as writeSessionMeta) into a buffer, for exports that
// join several files and so drop each file's own meta line.
int sessionMetaLine(char* out, size_t cap) {
  int w = snprintf(out, cap, "{\"evt\":\"meta\",\"scanner_id\":\"%s\",\"scanner_name\":\"%s\",\"fw\":\"%s\",\"db_gen\":%lu}\n",
                   scannerIdHex(), scannerName(), FW_VERSION, (unsigned long)DB_GEN);
  return (w > 0 && (size_t)w < cap) ? w : 0;
}

// Time-window export of ALL session files ("T:<mode>[:<minutes>]:*"): one joined NDJSON stream of
// every row in the window across every /logs/*.jsonl (fg + bg, New Session, earlier boots), dated
// and filtered on the CYD (logfilter::MultiExport). Two passes like the single-file export: count
// for the SIZE= header, then send. Both are deterministic, and no file grows in between (loop() is
// blocked; fleet lines raised meanwhile wait in XferScope's buffer), so the totals agree.
static void streamMultiOverBle(int mode, int minutes) {
  const uint32_t ep = phone::authEpoch();   // the requesting phone: a new peer must never get this reply
  if (!g_sdOk) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  const char* boot[BOOT_FILES + 3];
  int nb = thisBootFiles(boot, BOOT_FILES + 3);
  logfilter::MultiExport mx(mode, minutes < 1 ? 1 : (uint32_t)minutes, epochLocal(), millis(), boot, nb);
  int nf = mx.begin();
  char meta[160];
  int metaN = sessionMetaLine(meta, sizeof(meta));
  uint32_t rows = mx.run([ep](const char*, size_t) { return xferAlive(ep); });
  if (!phone::transferAlive(ep)) { Serial.println("[CYD] BLE multi export: phone left while counting"); return; }
  uint32_t total = rows ? rows + (uint32_t)metaN : 0;
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SIZE=%u", (unsigned)total);
  phone::logNotify((const uint8_t*)hdr, hn);
  Serial.printf("[CYD] BLE multi export mode=%d: %d file(s)%s, %u bytes\n", mode, nf,
                mx.dropped() ? " (oldest beyond cap skipped)" : "", (unsigned)total);
  if (!total) return;
  delay(30);
  struct Out { uint8_t b[180]; size_t n = 0; uint32_t sent = 0; } o;  // chunk < MTU-3
  auto put = [&o, ep](const char* p, size_t len) {
    for (size_t off = 0; off < len; ) {
      size_t rem = len - off, avail = sizeof(o.b) - o.n;
      size_t k = rem < avail ? rem : avail;
      memcpy(o.b + o.n, p + off, k);
      o.n += k; off += k;
      if (o.n == sizeof(o.b)) { phone::logNotify(o.b, o.n); o.sent += o.n; o.n = 0; delay(15); }
    }
    return phone::transferAlive(ep);
  };
  put(meta, metaN);
  mx.run(put);
  if (o.n) { phone::logNotify(o.b, o.n); o.sent += o.n; o.n = 0; }
  padToSize(o.sent, total, ep);
  Serial.printf("[CYD] BLE multi export sent %u/%u bytes\n", (unsigned)o.sent, (unsigned)total);
}

// "L": the current session = the stream matching the current screen.
void transferLog() { XferScope xs; streamFileOverBle(activeStream().path); }

// "T:<mode>[:<minutes>][:<path>]": mode 0 = whole file ("since boot"), 1 = past 24 h, 2 = past
// <minutes>. Path optional (restricted to /logs/); default = the current session; "*" = every log
// file (the window spans all sessions -- see streamMultiOverBle).
void transferFiltered(int mode, int minutes, const char* pathIn) {
  XferScope xs;
  char path[48];
  if (pathIn && pathIn[0] == '*') { streamMultiOverBle(mode, minutes); return; }
  if (pathIn && pathIn[0]) {
    if (strncmp(pathIn, "/logs/", 6) != 0 || strstr(pathIn, "..")) {
      phone::logNotify((const uint8_t*)"SIZE=0", 6);
      Serial.printf("[CYD] BLE export: rejected path %s\n", pathIn);
      return;
    }
    snprintf(path, sizeof(path), "%s", pathIn);
  } else {
    snprintf(path, sizeof(path), "%s", activeStream().path);
  }
  if (mode != 1 && mode != 2) { streamFileOverBle(path); return; }
  logfilter::Cutoff c = logfilter::make(mode, minutes < 1 ? 1 : (uint32_t)minutes, epochLocal(), millis());
  c.epochOnly = !strncasecmp(path, "/logs/fleet-", 12);   // a fleet file: members' lines, window by epoch only
  streamFilteredOverBle(path, c);
}

// The file's own random id from line 1's meta ("file_id":"<8 hex>", fw >= 0.25.0). false / "" when the first line has none.
// Reads the head of the file (any position; leaves it undefined), so callers seek afterwards.
static bool readFileId(File& f, char out[9]) {
  out[0] = 0;
  char b[512];
  if (!f.seek(0)) return false;
  int r = f.read((uint8_t*)b, sizeof(b) - 1);
  if (r <= 0) return false;
  b[r] = 0;
  return logfilter::MultiExport::fileIdOfHead(b, out);
}

// CRC-32 (IEEE, reflected, bitwise: 64 bytes at most) of the bytes [max(0, off-64), off) of f -- the tail request's proof
// that the phone's cached prefix and this file agree right before the join point. Same value as zlib crc32 / the web app's.
static bool prefixCrc(File& f, uint32_t off, uint32_t* out) {
  const uint32_t from = off > 64 ? off - 64 : 0;
  uint8_t b[64];
  const uint32_t n = off - from;
  if (n && (!f.seek(from) || f.read(b, n) != (int)n)) return false;
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) {
    c ^= b[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  *out = ~c;
  return true;
}

// "F:<path>@<offset>:<file_id>:<crc>" -- stream only the bytes [offset, size) of an append-only log the phone already holds
// the first <offset> bytes of. <crc> = 8 lowercase hex, CRC-32 of the 64 bytes before <offset> (fewer if offset < 64; 00000000 for
// offset 0): a file replaced under the same line 1 (SD explorer upload) fails it. Answers with ONE header, never confusable
// with a full send:
//   "TAIL=<n>@<offset>" then exactly <n> raw bytes (n may be 0 = nothing new), or
//   "TAILERR=path|id|off"  (unsafe/missing path | line 1 has a different file_id, or none, or the prefix CRC differs |
//   offset beyond the end = the file was replaced) -> the phone falls back to a full "F:<path>"; or
//   "SIZE=0" when the request does not even parse (an older phone's 3-field form).
// No newline padding on a short send (unlike the exports): the phone appends the tail to its cached copy only if it got
// exactly <n> bytes, so a short send just times out there and the cache stays untouched.
static void tailRefuse(const char* why) {
  char h[16];
  int n = snprintf(h, sizeof(h), "TAILERR=%s", why);
  phone::logNotify((const uint8_t*)h, n);
}
static void transferTail(const char* req) {
  char p[100];
  strncpy(p, req, sizeof(p) - 1);
  p[sizeof(p) - 1] = 0;
  char* c2 = strrchr(p, ':');                        // ":<crc>"
  char* c1 = nullptr;                                // ":<file_id>"
  char* at = nullptr;
  if (c2) { *c2 = 0; c1 = strrchr(p, ':'); }
  if (c1) { *c1 = 0; at = strrchr(p, '@'); }
  char id[9] = "";
  uint32_t off = 0, wantCrc = 0;
  bool ok = c2 && c1 && at && strlen(c1 + 1) == 8 && strlen(c2 + 1) == 8;
  if (ok) {
    for (int i = 0; i < 8; i++) {
      char c = c1[1 + i], d = c2[1 + i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) || !((d >= '0' && d <= '9') || (d >= 'a' && d <= 'f'))) { ok = false; break; }
      id[i] = c;
      wantCrc = (wantCrc << 4) | (uint32_t)(d <= '9' ? d - '0' : d - 'a' + 10);
    }
    id[8] = 0;
  }
  if (ok) {
    *at = 0;
    const char* d = at + 1;
    if (!*d || strlen(d) > 9) ok = false;            // <= 9 digits: files stay far below 1 GB (512 MB log cap)
    else for (; *d; d++) { if (*d < '0' || *d > '9') { ok = false; break; } off = off * 10 + (uint32_t)(*d - '0'); }
  }
  if (!ok) {                                         // does not parse (e.g. an older phone's 3-field form): plain empty reply
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE tail: malformed request %s\n", req);
    return;
  }
  if (!files::safePath(p) || files::isDir(p) || !g_sdOk || !SD.exists(p)) {
    tailRefuse("path");
    Serial.printf("[CYD] BLE tail: rejected %s\n", req);
    return;
  }
  File f = SD.open(p, "r");
  if (!f) { tailRefuse("path"); return; }
  const uint32_t sz = f.size();
  char cur[9];
  if (!readFileId(f, cur) || strcmp(cur, id) != 0) { f.close(); tailRefuse("id"); Serial.printf("[CYD] BLE tail: file_id mismatch %s\n", p); return; }
  if (off > sz) { f.close(); tailRefuse("off"); Serial.printf("[CYD] BLE tail: offset %u > size %u (%s)\n", (unsigned)off, (unsigned)sz, p); return; }
  uint32_t haveCrc = 0;
  if (!prefixCrc(f, off, &haveCrc) || haveCrc != wantCrc) { f.close(); tailRefuse("id"); Serial.printf("[CYD] BLE tail: prefix CRC mismatch %s\n", p); return; }
  strncpy(g_streamingPath, p, sizeof(g_streamingPath) - 1);   // guard: not rotated away mid-stream
  g_streamingPath[sizeof(g_streamingPath) - 1] = 0;
  const uint32_t n = sz - off;
  char hdr[32];
  int hn = snprintf(hdr, sizeof(hdr), "TAIL=%u@%u", (unsigned)n, (unsigned)off);
  phone::logNotify((const uint8_t*)hdr, hn);
  delay(30);
  uint32_t sent = 0;
  if (n && f.seek(off)) {
    const uint32_t ep = phone::authEpoch();            // stop if the peer leaves / re-authenticates mid-transfer
    uint8_t buf[180];
    while (sent < n && phone::transferAlive(ep)) {
      uint32_t want = n - sent > sizeof(buf) ? sizeof(buf) : n - sent;
      int r = f.read(buf, want);
      if (r <= 0) break;
      phone::logNotify(buf, r);
      sent += r;
      delay(15);
    }
  }
  f.close();
  Serial.printf("[CYD] BLE tail sent %u/%u bytes of %s from %u\n", (unsigned)sent, (unsigned)n, p, (unsigned)off);
  g_streamingPath[0] = 0;
}

// Download a file the phone requested ("F:<path>"): any file on the card (the web file explorer),
// gated by files::safePath (absolute, no traversal or FAT-illegal characters). A folder sends SIZE=0.
// "F:<path>@<offset>:<file_id>" is the tail form (transferTail); safePath forbids ':' so it can't be a plain path.
void transferFile(const char* path) {
  XferScope xs;
  if (strchr(path, ':')) { transferTail(path); return; }
  if (!files::safePath(path) || files::isDir(path)) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE dl: rejected path %s\n", path);
    return;
  }
  streamFileOverBle(path);
}


// Delete a session file the phone selected ("D:<path>"). Restricted to /logs/ (no
// traversal) and refuses the live session being written. Pushes the refreshed list back.
static int wipeAllLogs();       // fwd decl ("D:*" bulk delete)
void deleteSession(const char* path) {
  if (strcmp(path, "*") == 0 || strcmp(path, "/logs/*") == 0) {  // "D:*" = wipe all sessions
    if (wipeAllLogs() > 0) logPhoneCfg("delete", "/logs/*");
    sendSessionList();
    return;
  }
  // Same guards as the file explorer: a /logs/ file only, safePath (no aliases / traversal), and the
  // case-insensitive logPathProtected (live fg/bg, Explore-held, streaming, in-progress upload).
  if (strncasecmp(path, "/logs/", 6) != 0 || !files::safePath(path)) {
    Serial.printf("[CYD] delete rejected (bad path) %s\n", path);
  } else if (logPathProtected(path)) {
    Serial.println("[CYD] delete refused: live session / open / downloading");
  } else if (SD.exists(path) && SD.remove(path)) {
    Serial.printf("[CYD] deleted %s\n", path);
    logPhoneCfg("delete", path);   // only a delete that really happened is audited
  } else {
    Serial.printf("[CYD] delete failed %s\n", path);
  }
  sendSessionList();  // refresh the phone's picker either way
}

static String logBase(const String& nm) {  // dir entry -> bare basename (core may add a path)
  int slash = nm.lastIndexOf('/');
  return slash >= 0 ? nm.substring(slash + 1) : nm;
}

// Bulk-delete every /logs/ session except the live one and any file the Scan Viewer holds
// open (same guards as deleteSession). Two-pass per batch (collect names, then remove) since
// deleting while iterating a directory is unsafe; re-scans until a pass finds nothing left to
// delete, bounded so a huge card can't spin forever.
static int wipeAllLogs() {
  XferScope xs;                                  // up to 64 directory scans: keep the fleet serviced
  int removed = 0;
  for (int pass = 0; pass < 64; pass++) {       // up to 64 * 24 files
    File dir = SD.open("/logs");
    if (!dir) break;
    char victims[24][48];
    int nv = 0;
    for (File e = dir.openNextFile(); e && nv < 24; e = dir.openNextFile()) {
      xferKeepAlive();
      if (!e.isDirectory()) {
        char full[48];
        snprintf(full, sizeof(full), "/logs/%s", logBase(e.name()).c_str());
        if (strcmp(full, g_fg.path) != 0 && strcmp(full, g_bg.path) != 0 && strcmp(full, g_fleetLog.path) != 0 &&
            !hostui::exploreHolds(full))
          snprintf(victims[nv++], sizeof(victims[0]), "%s", full);
      }
      e.close();
    }
    dir.close();
    if (nv == 0) break;                          // only the live/held file(s) remain
    for (int i = 0; i < nv; i++)
      if (SD.remove(victims[i])) { removed++; Serial.printf("[CYD] wiped %s\n", victims[i]); }
  }
  Serial.printf("[CYD] wipe all logs: %d file(s) removed (live session kept)\n", removed);
  return removed;
}

// --- 512 MB /logs/ cap with oldest-first auto-rotation. Sums every /logs/ file and, while over the
// cap, deletes the OLDEST (by filesystem last-write time), never touching the live fg/bg files, a
// file the Explore viewer holds open, or one being streamed. Run only at low frequency (boot, each
// session rotation, a ~10 min loop() timer) -- never per scan cycle. Bounded per call. ---
static constexpr uint64_t LOG_CAP_BYTES = 512ULL * 1024 * 1024;
// Case-insensitive: FAT resolves "/LOGS/SESS-00007.JSONL" to the same file, and the web file explorer
// can send any spelling (files::safePath already rejects the trailing-dot/space aliases).
bool logPathProtected(const char* full) {
  return strcasecmp(full, g_fg.path) == 0 || strcasecmp(full, g_bg.path) == 0 ||
         (g_fleetLog.path[0] && strcasecmp(full, g_fleetLog.path) == 0) || hostui::exploreHolds(full) ||
         (g_streamingPath[0] && strcasecmp(full, g_streamingPath) == 0) || files::uploadHolds(full);
}
void enforceLogCap() {
  if (!g_sdOk) return;
  XferScope xs;                                  // up to 64 full directory scans on a full card
  int removed = 0;
  for (int pass = 0; pass < 64; pass++) {
    File dir = SD.open("/logs");
    if (!dir) return;
    uint64_t total = 0;
    time_t oldT = 0;
    char oldest[48] = "";
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      xferKeepAlive();
      if (!e.isDirectory()) {
        total += (uint64_t)e.size();
        char full[48];
        snprintf(full, sizeof(full), "/logs/%s", logBase(e.name()).c_str());
        // Eviction order = oldest filesystem last-write time. NOTE: before the phone syncs the clock
        // the RTC sits at the FAT epoch (2016-ish), so files written this pre-sync boot carry
        // near-identical timestamps and their relative eviction order is arbitrary. That's benign:
        // the cap still trims deterministically down to size, and real wall-clock times (hence a
        // meaningful oldest-first order) apply to every file written after sync.
        time_t t = e.getLastWrite();
        if (!logPathProtected(full) && (!oldest[0] || t < oldT)) { oldT = t; memcpy(oldest, full, sizeof(oldest)); }
      }
      e.close();
    }
    dir.close();
    if (total <= LOG_CAP_BYTES || !oldest[0]) break;
    if (!SD.remove(oldest)) break;
    removed++;
    Serial.printf("[CYD] log cap: removed oldest %s (was %llu MB total)\n", oldest, (unsigned long long)(total >> 20));
  }
  if (removed) Serial.printf("[CYD] log cap: %d file(s) removed\n", removed);
}

// Send the list of session logs to the phone: a "SESS=<n>" header then <n> bytes of
// "<path>\t<size>\t<flag>\t<firstEpoch>\t<lastEpoch>\t<file_id>\n" lines (the epochs = the file's dated span,
// 0 = unknown; <file_id> = line 1's "file_id" (fw 0.26.0), empty for older files; older web apps read only the
// leading fields and ignore the rest). <flag> marks the newest file: "current" while a scan
// is running (its file is being written), else "latest"; blank for all other rows. The web
// app parses it into a picker and labels/floats the flagged row.
void sendSessionList() {
  XferScope xs;
  const uint32_t ep = phone::authEpoch();   // the requesting phone: a new peer must never get this reply
  bool alive = true;
  // The newest file is the current session when its file exists this boot; otherwise the
  // most recently written prior file (by FS timestamp).
  const LogStream& cs = activeStream();   // the stream "current"/"L" refers to right now
  String curBase = logBase(String(cs.path));
  String newestBase;
  if (cs.started) {
    newestBase = curBase;
  } else {
    time_t newestT = -1;
    File d = SD.open("/logs");
    if (d) {
      for (File e = d.openNextFile(); e && (alive = xferAlive(ep)); e = d.openNextFile()) {
        if (!e.isDirectory()) {
          time_t t = e.getLastWrite();
          if (t >= newestT) { newestT = t; newestBase = logBase(String(e.name())); }
        }
        e.close();
      }
      d.close();
    }
  }
  const char* flagWord = (cs.started && (hostboard::foregroundScan() || (g_bgScan && g_scanActive))) ? "current" : "latest";

  String list;
  list.reserve(4096);
  File dir = SD.open("/logs");
  if (dir) {
    for (File e = dir.openNextFile(); e && alive && (alive = xferAlive(ep)); e = dir.openNextFile()) {
      if (e.isDirectory()) continue;
      String base = logBase(String(e.name()));
      const char* flag = (newestBase.length() && base == newestBase) ? flagWord : "";
      uint32_t first = 0, last = 0;  // dated span (local epoch; 0 = unknown) -> the phone groups a scan's parts
      char fid[9] = "";
      if (base.endsWith(".jsonl")) logfilter::MultiExport::fileSpan(e, first, last, fid);   // file_id from the same head read
      list += "/logs/" + base + "\t" + String((uint32_t)e.size()) + "\t" + flag + "\t" + String(first) +
              "\t" + String(last) + "\t" + fid + "\n";
    }
    dir.close();
  }
  if (!alive) { Serial.println("[CYD] session list: phone left"); return; }
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SESS=%u", (unsigned)list.length());
  phone::logNotify((const uint8_t*)hdr, hn);
  delay(30);
  const char* p = list.c_str();
  size_t rem = list.length();
  while (rem && phone::transferAlive(ep)) {
    size_t n = rem > 180 ? 180 : rem;
    phone::logNotify((const uint8_t*)p, n);
    p += n; rem -= n;
    delay(15);
  }
  Serial.printf("[CYD] session list sent (%u bytes)\n", (unsigned)list.length());
}

// Send the active whitelist rules to the phone, same framing as the session list: a
// "WL=<bytes>" header then <bytes> bytes of "<csv rule line>\n" lines (line order = the
// index "E:<n>" removes).
void sendWhitelist() {
  String list;
  whitelist::listAll(list);
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "WL=%u", (unsigned)list.length());
  phone::logNotify((const uint8_t*)hdr, hn);
  delay(30);
  const char* p = list.c_str();
  size_t rem = list.length();
  while (rem) {
    size_t n = rem > 180 ? 180 : rem;
    phone::logNotify((const uint8_t*)p, n);
    p += n; rem -= n;
    delay(15);
  }
  Serial.printf("[CYD] whitelist sent (%d rules, %u bytes)\n", whitelist::count(), (unsigned)list.length());
}

// Session basename: the stream's path without the "/logs/" dir and ".jsonl" extension.
void sessBase(const LogStream& st, char* out, size_t cap) {
  const char* b = strrchr(st.path, '/');
  b = b ? b + 1 : st.path;
  strncpy(out, b, cap - 1); out[cap - 1] = 0;
  char* dot = strrchr(out, '.');
  if (dot) *dot = 0;
}

// P6 item 3 -- {"epoch","ms",["lat","lon",...],"evt":"devpos","scanner":"<12hex>","dlat","dlon","sats":N,"used":0|1}
// While in a fleet, a unit with a LIVE fix of its own (onboard GPS) logs it every DEVPOS_MS or after DEVPOS_M of
// movement: lat/lon = the position its rows are stamped with (the fleet position on a client), dlat/dlon = its own fix,
// used = 1 when that own fix IS the fleet position (leader: its rows use its GPS; client: the leader picked its GPS).
void logDevPos() {
  static constexpr uint32_t DEVPOS_MS = 30000, ONB_LIVE_MS = 10000;
  static constexpr float    DEVPOS_M = 25.0f;
  static uint32_t lastMs = 0;
  static float    lastLat = 0, lastLon = 0;
  char fid[17], fld[13];
  if (!fleet::fleetIdent(fid, fld) || !g_onbValid || !g_onbAtMs || millis() - g_onbAtMs >= ONB_LIVE_MS) return;
  if (!isfinite(g_onbLat) || !isfinite(g_onbLon)) return;
  const uint32_t now = millis();
  if (lastMs) {
    const float k = 111320.0f, dy = (g_onbLat - lastLat) * k;
    const float dx = (g_onbLon - lastLon) * k * cosf(g_onbLat * (float)M_PI / 180.0f);
    if (now - lastMs < DEVPOS_MS && dx * dx + dy * dy < DEVPOS_M * DEVPOS_M) return;
  }
  LogStream& st = activeStream();
  if (bgCtx() && !st.path[0]) openBgSession();
  if (!g_sdOk || !ensureLogFile(st)) return;
  File f = SD.open(st.path, FILE_APPEND);
  if (!f) return;
  const bool used = fleet::role() == fleet::ROLE_LEADER ? posSnapshot().from == 2 : fleet::gpsIsFleetPosition();
  char ln[LOG_LINE];
  int n = 0;
  lnAppend(ln, n, "{\"epoch\":%lu,\"ms\":%lu", (unsigned long)epochLocal(), (unsigned long)now);
  appendPos(ln, n);
  lnAppend(ln, n, ",\"evt\":\"devpos\",\"scanner\":\"%s\",\"dlat\":%.6f,\"dlon\":%.6f,\"sats\":%u,\"used\":%d}\n",
           scannerIdHex(), g_onbLat, g_onbLon, (unsigned)g_onbSats, used ? 1 : 0);
  if (n <= (int)LOG_LINE) f.write((const uint8_t*)ln, n);
  f.close();
  lastMs = now; lastLat = g_onbLat; lastLon = g_onbLon;
}

// ---- P6 item 6 (docs/fleet-mode.md §22): fleet store-and-forward -------------------------------------------------
// CLIENT side: which of this unit's own log files to forward, and a raw reader for it (the packing/ack logic lives in
// fleet_client.cpp; this file only knows the streams and the SD card).
int logFwdList(FwdFile* out, int cap) {
  int n = 0;
  auto add = [&](const char* id, const char* path, bool live) {
    if (n >= cap || !id[0] || !path[0]) return;
    for (int i = 0; i < n; i++) if (!strcmp(out[i].id, id)) return;
    memcpy(out[n].id, id, 9);
    snprintf(out[n].path, sizeof(out[n].path), "%s", path);
    out[n++].live = live;
  };
  LogStream& act = activeStream();
  LogStream& oth = (&act == &g_fg) ? g_bg : g_fg;
  if (act.started) add(act.fileId, act.path, true);
  if (oth.started) add(oth.fileId, oth.path, true);
  const int have = g_bootFileN < BOOT_FILES ? g_bootFileN : BOOT_FILES;
  for (int i = 0; i < have; i++) {                                   // newest rotated file first
    const BootFile& bf = g_bootFiles[(g_bootFileN - 1 - i + BOOT_FILES * 2) % BOOT_FILES];
    add(bf.fileId, bf.path, false);
  }
  return n;
}

int logFwdRead(const char* path, uint32_t off, uint8_t* buf, size_t cap, uint32_t* size) {
  if (!g_sdOk) return 0;
  File f = SD.open(path, "r");
  if (!f) return 0;
  const uint32_t sz = (uint32_t)f.size();
  if (size) *size = sz;
  int n = 0;
  if (off < sz && f.seek(off)) n = f.read(buf, cap);
  f.close();
  return n > 0 ? n : 0;
}

// LEADER side. The fleet file is created on the first forwarded line (one per leader boot), named by its own NVS
// counter, and renamed + anchored on time sync like fg/bg (renameStreamOnSync). Counter names never repeat, but a
// wiped NVS could reuse one: skip any that exist (FILE_WRITE would truncate it).
static void fleetLogBegin() {
  Preferences p; p.begin("cydscan", false);
  uint32_t n = p.getULong("fltfcnt", 0);
  do { n++; snprintf(g_fleetLog.path, sizeof(g_fleetLog.path), "/logs/fleet-%05lu.jsonl", (unsigned long)n); }
  while (SD.exists(g_fleetLog.path) && n < 99999);
  p.putULong("fltfcnt", n);
  p.end();
  g_fleetLog.named = false; g_fleetLog.started = false; g_fleetLog.fileId[0] = 0;
  g_fleetLog.uniqCount = g_fleetLog.rowCount = 0;
  Serial.printf("[CYD] fleet log (created on first forwarded line): %s\n", g_fleetLog.path);
  renameStreamOnSync(g_fleetLog, "fleet-");   // clock already known: be born date-named
}

const char* fleetLogPath() { return g_fleetLog.path; }

// `lines` = `len` bytes of complete '\n'-terminated lines (the leader validated them). Each stored line is the member's
// text with {"scanner":"<sid>","ofile":"<id>","orow":N, in place of its opening brace.
bool fleetLogStore(const char* sidHex, const char* ofile, uint32_t firstRow, bool anchor, const char* lines, size_t len,
                   uint32_t* nextRow, uint32_t* stored) {
  if (g_xferDepth || !g_sdOk) return false;   // a transfer is counting/sending file sizes: the client resends later
  if (!g_fleetLog.path[0]) fleetLogBegin();
  if (!ensureLogFile(g_fleetLog)) return false;
  File f = SD.open(g_fleetLog.path, FILE_APPEND);
  if (!f) return false;
  bool ok = true;
  uint32_t row = firstRow;
  for (size_t off = 0; off < len; row++) {
    const char* ln = lines + off;
    const char* nl = (const char*)memchr(ln, '\n', len - off);
    if (!nl) { ok = false; break; }
    const size_t ll = (size_t)(nl - ln) + 1;
    off += ll;
    if (row < *nextRow) continue;   // already stored this boot (a resent frame, or an anchor re-sent on a rejoin)
    char pre[64];
    const int pn = snprintf(pre, sizeof(pre), "{\"scanner\":\"%s\",\"ofile\":\"%s\",\"orow\":%lu,", sidHex, ofile, (unsigned long)row);
    if (pn <= 0 || pn >= (int)sizeof(pre) || f.write((const uint8_t*)pre, pn) != (size_t)pn ||
        f.write((const uint8_t*)ln + 1, ll - 1) != ll - 1) {
      f.write((const uint8_t*)"\n", 1);        // end a torn line so the next append starts clean
      ok = false;
      break;
    }
    if (!anchor) *nextRow = row + 1;
    (*stored)++;
    g_fleetLog.rowCount++;
  }
  f.close();
  return ok;
}

// host_log.cpp — see host_log.h. Code moved verbatim from the CYD main.cpp (docs/mini-build.md §4).
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#include <time.h>
#include <stdarg.h>
#include "host_log.h"
#include "host_board.h"
#include "host_ui.h"
#include "logfilter.h"
#include "phone.h"
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
char g_streamingPath[48] = "";

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
struct BootFile { char path[48]; bool anchored; };
static constexpr int BOOT_FILES = 8;
static BootFile g_bootFiles[BOOT_FILES];
static int g_bootFileN = 0;  // ring write count
static void rememberBootFile(const LogStream& st) {
  if (!st.started || !st.path[0]) return;
  BootFile& b = g_bootFiles[g_bootFileN++ % BOOT_FILES];
  snprintf(b.path, sizeof(b.path), "%s", st.path);
  b.anchored = st.named;  // a date-named file was created/renamed after sync (rows dated or anchored)
}
int thisBootFiles(const char** out, int cap) {
  int n = 0;
  if (n < cap && g_fg.path[0]) out[n++] = g_fg.path;
  if (n < cap && g_bg.path[0]) out[n++] = g_bg.path;
  int have = g_bootFileN < BOOT_FILES ? g_bootFileN : BOOT_FILES;
  for (int i = 0; i < have && n < cap; i++) out[n++] = g_bootFiles[i].path;
  return n;
}

static void writeDepartures(LogStream& st, uint32_t ms, bool all);  // below, with the line writers
void beginSessionNamed(LogStream& st, bool bg) {
  writeDepartures(st, millis(), true);  // close out every presence period in the outgoing file
  rememberBootFile(st);
  if (!st.seen) {
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
static void writeSessionMeta(File& f);  // defined with FW_VERSION/DB_GEN below
bool ensureLogFile(LogStream& st) {
  if (st.started) return true;
  if (!g_sdOk || !st.path[0]) return false;
  File f = SD.open(st.path, FILE_WRITE);
  if (!f) return false;
  writeSessionMeta(f);
  f.close();
  st.started = true;
  Serial.printf("[CYD] session log created: %s\n", st.path);
  return true;
}

// Once the phone provides wall-clock time, rename a boot-counter session file to a date-time name
// (fg: YYYYMMDD-HHMMSS.jsonl, bg: bg-YYYYMMDD-HHMMSS.jsonl). Runs once per session (even if the
// rename fails, it won't retry-spam).
static void renameStreamOnSync(LogStream& st, bool bg) {
  if (st.named || !st.path[0] || !g_sdOk || !timeHasSync()) return;
  time_t t = (time_t)epochLocal();
  struct tm tmv;
  gmtime_r(&t, &tmv);  // epoch is stored already-localized, so gmtime gives local fields
  char nn[48];
  snprintf(nn, sizeof(nn), "/logs/%s%04d%02d%02d-%02d%02d%02d.jsonl", bg ? "bg-" : "",
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
    File mf = SD.open(st.path, FILE_APPEND);
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
  renameStreamOnSync(g_fg, false);
  renameStreamOnSync(g_bg, true);
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
static void writeSessionMeta(File& f) {
  f.printf("{\"evt\":\"meta\",\"scanner_id\":\"%s\",\"scanner_name\":\"%s\",\"fw\":\"%s\",\"db_gen\":%lu}\n",
           scannerIdHex(), scannerName(), FW_VERSION, (unsigned long)DB_GEN);
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
}

int logNewDetections() {
  int n = 0;
  LogStream& st = activeStream();  // headless bg cycle -> bg stream; on-screen scan -> fg stream
  if (bgCtx() && !st.path[0]) openBgSession();
  File f;  // opened lazily on the first new detection so idle boots write no file
  uint32_t ms = millis();
  uint32_t epoch = epochLocal();
  for (int i = 0; i < g_detCount; i++) {
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
      if (!(g_dets[i].flags & FLAG_WIFI_EVILTWIN)) continue;
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
  while (f.available()) {
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
static void padToSize(uint32_t& sent, uint32_t total) {
  if (sent >= total || !phone::connected()) return;
  Serial.printf("[CYD] BLE export short by %u bytes: padding\n", (unsigned)(total - sent));
  uint8_t nl[180];
  memset(nl, '\n', sizeof(nl));
  while (sent < total && phone::connected()) {
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
  if (!g_sdOk || !SD.exists(path)) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  File f = SD.open(path, "r");
  if (!f) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  strncpy(g_streamingPath, path, sizeof(g_streamingPath) - 1);
  g_streamingPath[sizeof(g_streamingPath) - 1] = 0;
  char line[logfilter::LINE_CAP];
  size_t len;
  uint32_t total = 0, nlines = 0;
  {
    logfilter::LineReader rd(f);
    while (rd.next(line, len)) if (logfilter::keep(line, c)) { total += len; nlines++; }
  }
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
  while (rd2.next(line, len) && phone::connected()) {
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
  padToSize(sent, total);
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
// for the SIZE= header, then send. Both are deterministic, and nothing logs while this runs (same
// loop task), so the totals agree.
static void streamMultiOverBle(int mode, int minutes) {
  if (!g_sdOk) { phone::logNotify((const uint8_t*)"SIZE=0", 6); return; }
  const char* boot[BOOT_FILES + 2];
  int nb = thisBootFiles(boot, BOOT_FILES + 2);
  logfilter::MultiExport mx(mode, minutes < 1 ? 1 : (uint32_t)minutes, epochLocal(), millis(), boot, nb);
  int nf = mx.begin();
  char meta[160];
  int metaN = sessionMetaLine(meta, sizeof(meta));
  uint32_t rows = mx.run([](const char*, size_t) { return true; });
  uint32_t total = rows ? rows + (uint32_t)metaN : 0;
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SIZE=%u", (unsigned)total);
  phone::logNotify((const uint8_t*)hdr, hn);
  Serial.printf("[CYD] BLE multi export mode=%d: %d file(s)%s, %u bytes\n", mode, nf,
                mx.dropped() ? " (oldest beyond cap skipped)" : "", (unsigned)total);
  if (!total) return;
  delay(30);
  struct Out { uint8_t b[180]; size_t n = 0; uint32_t sent = 0; } o;  // chunk < MTU-3
  auto put = [&o](const char* p, size_t len) {
    for (size_t off = 0; off < len; ) {
      size_t rem = len - off, avail = sizeof(o.b) - o.n;
      size_t k = rem < avail ? rem : avail;
      memcpy(o.b + o.n, p + off, k);
      o.n += k; off += k;
      if (o.n == sizeof(o.b)) { phone::logNotify(o.b, o.n); o.sent += o.n; o.n = 0; delay(15); }
    }
    return phone::connected();
  };
  put(meta, metaN);
  mx.run(put);
  if (o.n) { phone::logNotify(o.b, o.n); o.sent += o.n; o.n = 0; }
  padToSize(o.sent, total);
  Serial.printf("[CYD] BLE multi export sent %u/%u bytes\n", (unsigned)o.sent, (unsigned)total);
}

// "L": the current session = the stream matching the current screen.
void transferLog() { streamFileOverBle(activeStream().path); }

// "T:<mode>[:<minutes>][:<path>]": mode 0 = whole file ("since boot"), 1 = past 24 h, 2 = past
// <minutes>. Path optional (restricted to /logs/); default = the current session; "*" = every log
// file (the window spans all sessions -- see streamMultiOverBle).
void transferFiltered(int mode, int minutes, const char* pathIn) {
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
  streamFilteredOverBle(path, c);
}

// Download a specific session file requested by the phone ("F:<path>"). Restricted to
// /logs/ (no path traversal) so the phone can't pull arbitrary SD files.
void transferFile(const char* path) {
  if (strncmp(path, "/logs/", 6) != 0 || strstr(path, "..")) {
    phone::logNotify((const uint8_t*)"SIZE=0", 6);
    Serial.printf("[CYD] BLE dl: rejected path %s\n", path);
    return;
  }
  streamFileOverBle(path);
}


// Delete a session file the phone selected ("D:<path>"). Restricted to /logs/ (no
// traversal) and refuses the live session being written. Pushes the refreshed list back.
static void wipeAllLogs();      // fwd decl ("D:*" bulk delete)
void deleteSession(const char* path) {
  if (strcmp(path, "*") == 0 || strcmp(path, "/logs/*") == 0) {  // "D:*" = wipe all sessions
    wipeAllLogs();
    sendSessionList();
    return;
  }
  if (strncmp(path, "/logs/", 6) != 0 || strstr(path, "..")) {
    Serial.printf("[CYD] delete rejected (bad path) %s\n", path);
  } else if (strcmp(path, g_fg.path) == 0 || strcmp(path, g_bg.path) == 0) {
    Serial.println("[CYD] delete refused: that's a live session");
  } else if (g_streamingPath[0] && strcmp(path, g_streamingPath) == 0) {
    Serial.println("[CYD] delete refused: being downloaded");   // defense-in-depth (also covered by logProtected)
  } else if (hostui::exploreHolds(path)) {
    Serial.println("[CYD] delete refused: open in the scan viewer");
  } else {
    if (SD.exists(path)) SD.remove(path);
    Serial.printf("[CYD] deleted %s\n", path);
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
static void wipeAllLogs() {
  int removed = 0;
  for (int pass = 0; pass < 64; pass++) {       // up to 64 * 24 files
    File dir = SD.open("/logs");
    if (!dir) break;
    char victims[24][48];
    int nv = 0;
    for (File e = dir.openNextFile(); e && nv < 24; e = dir.openNextFile()) {
      if (!e.isDirectory()) {
        char full[48];
        snprintf(full, sizeof(full), "/logs/%s", logBase(e.name()).c_str());
        if (strcmp(full, g_fg.path) != 0 && strcmp(full, g_bg.path) != 0 && !hostui::exploreHolds(full))
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
}

// --- 512 MB /logs/ cap with oldest-first auto-rotation. Sums every /logs/ file and, while over the
// cap, deletes the OLDEST (by filesystem last-write time), never touching the live fg/bg files, a
// file the Explore viewer holds open, or one being streamed. Run only at low frequency (boot, each
// session rotation, a ~10 min loop() timer) -- never per scan cycle. Bounded per call. ---
static constexpr uint64_t LOG_CAP_BYTES = 512ULL * 1024 * 1024;
static bool logProtected(const char* full) {
  return strcmp(full, g_fg.path) == 0 || strcmp(full, g_bg.path) == 0 || hostui::exploreHolds(full) ||
         (g_streamingPath[0] && strcmp(full, g_streamingPath) == 0);
}
void enforceLogCap() {
  if (!g_sdOk) return;
  int removed = 0;
  for (int pass = 0; pass < 64; pass++) {
    File dir = SD.open("/logs");
    if (!dir) return;
    uint64_t total = 0;
    time_t oldT = 0;
    char oldest[48] = "";
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
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
        if (!logProtected(full) && (!oldest[0] || t < oldT)) { oldT = t; memcpy(oldest, full, sizeof(oldest)); }
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
// "<path>\t<size>\t<flag>\t<firstEpoch>\t<lastEpoch>\n" lines (the epochs = the file's dated span,
// 0 = unknown; older web apps read only the first three fields). <flag> marks the newest file: "current" while a scan
// is running (its file is being written), else "latest"; blank for all other rows. The web
// app parses it into a picker and labels/floats the flagged row.
void sendSessionList() {
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
      for (File e = d.openNextFile(); e; e = d.openNextFile()) {
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
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (e.isDirectory()) continue;
      String base = logBase(String(e.name()));
      const char* flag = (newestBase.length() && base == newestBase) ? flagWord : "";
      uint32_t first = 0, last = 0;  // dated span (local epoch; 0 = unknown) -> the phone groups a scan's parts
      if (base.endsWith(".jsonl")) logfilter::MultiExport::fileSpan(e, first, last);
      list += "/logs/" + base + "\t" + String((uint32_t)e.size()) + "\t" + flag + "\t" + String(first) +
              "\t" + String(last) + "\n";
    }
    dir.close();
  }
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "SESS=%u", (unsigned)list.length());
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

// host_files.cpp — see host_files.h. All SD access happens here on the loop() task; the NimBLE write
// callback (phone.cpp) only queues the command / copies the chunk.
//
// Every guard compares paths case-insensitively and safePath rejects "." parts and trailing dots/spaces:
// FAT resolves "/LOGS/x", "/logs." and "/logs/./x" to the same objects, so a case-sensitive or
// unnormalised check could be talked past (security review 2026-10-28, SEC-H1/H2).
#include <Arduino.h>
#include <SD.h>
#include "host_files.h"
#include "host_log.h"
#include "phone.h"

namespace {

constexpr uint32_t UPLOAD_MAX_BYTES = 8u * 1024 * 1024;  // BLE moves a few KB/s; bigger is impractical anyway
constexpr uint32_t UPLOAD_IDLE_MS   = 30000;             // no chunk for this long -> abort + drop the temp file
constexpr size_t   LIST_PAGE_BYTES  = 4096;              // one listing page (heap String; no PSRAM on the CYD)

File     s_up;
bool     s_upActive = false;
char     s_upPath[files::PATH_MAX_LEN + 4];
char     s_upTmp[files::PATH_MAX_LEN + 8];
uint32_t s_upSize = 0, s_upGot = 0, s_upLastMs = 0;
uint8_t  s_seq = 0;  // tag of the request being answered; echoed so the phone drops a late reply

// One reply = an "FS=<n>:<seq>" header then <n> bytes, same framing as SESS=/WL=. A reply that fits one
// notify (every upload ack) skips the inter-packet delays so a chunk round trip doesn't stall loop().
void reply(const String& body) {
  char hdr[24];
  int hn = snprintf(hdr, sizeof(hdr), "FS=%u:%u", (unsigned)body.length(), (unsigned)s_seq);
  phone::logNotify((const uint8_t*)hdr, hn);
  const bool small = body.length() <= 180;
  if (!small) delay(30);
  const char* p = body.c_str();
  size_t rem = body.length();
  const uint32_t ep = phone::authEpoch();
  while (rem && phone::transferAlive(ep)) {
    size_t n = rem > 180 ? 180 : rem;
    phone::logNotify((const uint8_t*)p, n);
    p += n; rem -= n;
    if (!small) delay(15);
  }
}
void replyErr(const char* why) {
  Serial.printf("[FS] error: %s\n", why);
  reply(String("ERR\t") + why);
}
void replyOk(const String& detail = "") { reply(String("OK\t") + detail); }

// The root and the session-log folder itself: renaming/deleting either would break logging.
bool reserved(const char* p) { return strcmp(p, "/") == 0 || strcasecmp(p, "/logs") == 0; }

bool endsWithCi(const char* s, const char* suf) {
  size_t n = strlen(s), k = strlen(suf);
  return n >= k && strcasecmp(s + n - k, suf) == 0;
}
// Names the firmware uses for its own temp files (upload staging, whitelist/sigdb .tmp recovery,
// the replace-backup): never valid upload or rename targets.
bool tempName(const char* p) { return endsWithCi(p, ".upl") || endsWithCi(p, ".tmp") || endsWithCi(p, ".bak"); }
// The log code (cap rotation, wipe, session list, D:) builds /logs/ paths in 48-byte buffers; a longer
// name there would be truncated, could never be removed, and would wedge the 512 MB cap (review REV-004).
bool logsNameTooLong(const char* p) { return strncasecmp(p, "/logs/", 6) == 0 && strlen(p) >= 48; }

// Copy `in` to `out` with a trailing '/' dropped (except the root), then validate.
bool normPath(const char* in, char* out, size_t cap) {
  size_t n = strlen(in);
  if (n >= cap) return false;
  memcpy(out, in, n + 1);
  if (n > 1 && out[n - 1] == '/') out[n - 1] = 0;
  return files::safePath(out);
}

String baseName(const char* nm) {  // dir entry -> bare name (the core may hand back a full path)
  const char* s = strrchr(nm, '/');
  return String(s ? s + 1 : nm);
}

// "Z:l:[<skip>:]<dir>": one page of a folder, entries <skip>.. in directory order. A full page ends
// with "MORE\t<next skip>" and the phone asks again, so a big /logs/ never needs one large buffer.
void listDir(char* arg) {
  uint32_t skip = 0;
  if (arg[0] >= '0' && arg[0] <= '9') {
    char* end;
    skip = (uint32_t)strtoul(arg, &end, 10);
    if (*end != ':') { replyErr("bad list"); return; }
    arg = end + 1;
  }
  char dir[files::PATH_MAX_LEN + 4];
  if (!normPath(arg[0] ? arg : "/", dir, sizeof(dir))) { replyErr("bad path"); return; }
  File d = SD.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); replyErr("not a folder"); return; }
  String out;
  // One allocation up front (an entry adds < 256 B past the page check), so the page never re-grows
  // on a fragmented heap; fail cleanly rather than send a silently truncated listing.
  if (!out.reserve(LIST_PAGE_BYTES + 256)) { d.close(); replyErr("low memory"); return; }
  out += "OK\t"; out += dir; out += "\n";
  uint32_t idx = 0;
  bool more = false;
  for (File e = d.openNextFile(); e; e = d.openNextFile(), idx++) {
    if (idx < skip) { e.close(); continue; }
    if (out.length() > LIST_PAGE_BYTES) { more = true; e.close(); break; }
    String nm = baseName(e.name());
    String full = strcmp(dir, "/") == 0 ? "/" + nm : String(dir) + "/" + nm;
    bool isD = e.isDirectory();
    out += nm; out += '\t';
    out += String(isD ? 0u : (uint32_t)e.size()); out += '\t';
    out += isD ? 'd' : 'f'; out += '\t';
    out += String((uint32_t)e.getLastWrite()); out += '\t';
    if (!isD && logPathProtected(full.c_str())) out += "live";
    out += '\n';
    e.close();
  }
  d.close();
  if (more) { out += "MORE\t"; out += String(idx); out += "\n"; }
  Serial.printf("[FS] list %s from %u: %u entries%s\n", dir, (unsigned)skip, (unsigned)(idx - skip),
                more ? " (more)" : "");
  reply(out);
}

void makeDir(const char* p) {
  if (SD.exists(p)) { replyErr("already exists"); return; }
  if (!SD.mkdir(p)) { replyErr("mkdir failed"); return; }
  Serial.printf("[FS] mkdir %s\n", p);
  logPhoneCfg("mkdir", p);
  replyOk();
}

void removePath(const char* p) {
  if (reserved(p)) { replyErr("that folder is protected"); return; }
  if (logPathProtected(p)) { replyErr("in use (live log / open / downloading)"); return; }
  if (!SD.exists(p)) { replyErr("not found"); return; }
  bool dir = files::isDir(p);
  if (!(dir ? SD.rmdir(p) : SD.remove(p))) { replyErr(dir ? "folder not empty" : "delete failed"); return; }
  Serial.printf("[FS] deleted %s\n", p);
  logPhoneCfg("delete", p);
  replyOk();
}

void renamePath(char* arg) {
  char* tab = strchr(arg, '\t');
  if (!tab) { replyErr("bad rename"); return; }
  *tab = 0;
  char from[files::PATH_MAX_LEN + 4], to[files::PATH_MAX_LEN + 4];
  if (!normPath(arg, from, sizeof(from)) || !normPath(tab + 1, to, sizeof(to))) { replyErr("bad path"); return; }
  if (reserved(from) || reserved(to)) { replyErr("that folder is protected"); return; }
  if (logPathProtected(from) || logPathProtected(to)) { replyErr("in use (live log / open / downloading)"); return; }
  if (tempName(to)) { replyErr("that name is reserved for temp files"); return; }
  if (logsNameTooLong(to)) { replyErr("name too long for /logs (47 chars max)"); return; }
  size_t fl = strlen(from);
  if (strncasecmp(to, from, fl) == 0 && to[fl] == '/') { replyErr("can't move a folder into itself"); return; }
  if (!SD.exists(from)) { replyErr("not found"); return; }
  // A case-only rename ("a.txt" -> "A.txt") names the same FAT entry, so exists() is true; allow it.
  if (strcasecmp(from, to) != 0 && SD.exists(to)) { replyErr("target already exists"); return; }
  if (!SD.rename(from, to)) { replyErr("rename failed"); return; }
  Serial.printf("[FS] renamed %s -> %s\n", from, to);
  logPhoneCfg("rename", from, to);
  replyOk();
}

void abortUpload(const char* why) {
  if (!s_upActive) return;
  s_up.close();
  SD.remove(s_upTmp);
  s_upActive = false;
  Serial.printf("[FS] upload %s aborted: %s\n", s_upPath, why);
}

// Move the finished temp file into place. FAT rename can't overwrite, so an existing file is first
// renamed to <path>.bak and only deleted once the new one is in; if that fails it is put back.
void finishUpload() {
  s_up.close();
  s_upActive = false;
  if (logPathProtected(s_upPath)) { SD.remove(s_upTmp); replyErr("destination became in use"); return; }
  char bak[files::PATH_MAX_LEN + 8];
  snprintf(bak, sizeof(bak), "%s.bak", s_upPath);
  bool hadOld = SD.exists(s_upPath);
  if (hadOld) {
    SD.remove(bak);  // a stale backup from an interrupted earlier replace
    if (!SD.rename(s_upPath, bak)) { SD.remove(s_upTmp); replyErr("can't replace the old file"); return; }
  }
  if (!SD.rename(s_upTmp, s_upPath)) {
    if (hadOld) SD.rename(bak, s_upPath);  // roll back
    SD.remove(s_upTmp);
    replyErr("rename failed (old file kept)");
    return;
  }
  if (hadOld) SD.remove(bak);
  Serial.printf("[FS] upload %s done (%u bytes)\n", s_upPath, (unsigned)s_upSize);
  logPhoneCfg("upload", s_upPath);
  reply(String("DONE\t") + String(s_upSize));
}

void beginUpload(char* arg) {  // "<size>:<path>"
  abortUpload("replaced by a new upload");
  char* end;
  uint32_t size = (uint32_t)strtoul(arg, &end, 10);
  if (end == arg || *end != ':' || arg[0] < '0' || arg[0] > '9') { replyErr("bad upload size"); return; }
  // 4 chars short of the limit so <path>.upl / <path>.bak stay valid paths (deletable from the explorer).
  if (!normPath(end + 1, s_upPath, files::PATH_MAX_LEN - 4)) { replyErr("bad path (too long?)"); return; }
  if (size > UPLOAD_MAX_BYTES) { replyErr("file too large (8 MB max)"); return; }
  if (reserved(s_upPath) || files::isDir(s_upPath)) { replyErr("that's a folder"); return; }
  if (logPathProtected(s_upPath)) { replyErr("in use (live log / open / downloading)"); return; }
  if (tempName(s_upPath)) { replyErr("that name is reserved for temp files"); return; }
  if (logsNameTooLong(s_upPath)) { replyErr("name too long for /logs (47 chars max)"); return; }
  snprintf(s_upTmp, sizeof(s_upTmp), "%s.upl", s_upPath);
  s_up = SD.open(s_upTmp, FILE_WRITE);
  if (!s_up) { replyErr("can't create file (does the folder exist?)"); return; }
  s_upActive = true;
  s_upSize = size; s_upGot = 0; s_upLastMs = millis();
  Serial.printf("[FS] upload %s (%u bytes) started\n", s_upPath, (unsigned)size);
  if (size == 0) { finishUpload(); return; }  // an empty file: no chunks will follow
  replyOk("0");
}

void uploadChunk(const uint8_t* b, size_t n) {
  if (!s_upActive) { replyErr("no upload in progress"); return; }
  if (s_upGot + n > s_upSize) { abortUpload("too many bytes"); replyErr("more data than announced"); return; }
  if (n && s_up.write(b, n) != n) { abortUpload("SD write failed"); replyErr("SD write failed (card full?)"); return; }
  s_upGot += n;
  s_upLastMs = millis();
  if (s_upGot == s_upSize) finishUpload();
  else replyOk(String(s_upGot));
}

}  // namespace

bool files::safePath(const char* p) {
  size_t n = strlen(p);
  if (n == 0 || n >= PATH_MAX_LEN || p[0] != '/') return false;
  for (const char* q = p; *q; q++) {
    unsigned char c = (unsigned char)*q;
    if (c < 0x20 || c >= 0x7F || strchr("\\:*?\"<>|", c)) return false;
  }
  // Each part between slashes: non-empty, not "." / "..", and not ending in '.' or ' ' (FAT strips
  // those, so "/logs." would alias "/logs"). The root "/" alone has no parts.
  if (n == 1) return true;
  for (const char* s = p + 1; ; ) {
    const char* e = strchr(s, '/');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len == 0 || s[len - 1] == '.' || s[len - 1] == ' ') return false;
    if (!e) break;
    s = e + 1;
  }
  return true;
}

bool files::isDir(const char* p) {
  File f = SD.open(p);
  bool d = f && f.isDirectory();
  if (f) f.close();
  return d;
}

bool files::uploadHolds(const char* p) { return s_upActive && strcasecmp(p, s_upTmp) == 0; }

void files::service() {
  // Overrun first (it also drops the queued chunk), then a waiting chunk, THEN the idle timeout --
  // a chunk that sat in the slot through a long loop() stall still counts as activity.
  if (phone::uploadOverrun(&s_seq)) { abortUpload("chunk overrun"); replyErr("upload out of sync — try again"); }
  {
    static uint8_t buf[phone::UPLOAD_CHUNK_MAX];
    size_t n;
    if (phone::uploadChunk(buf, sizeof(buf), &n, &s_seq)) uploadChunk(buf, n);
  }
  if (s_upActive && (!phone::authed() || millis() - s_upLastMs > UPLOAD_IDLE_MS))
    abortUpload(phone::authed() ? "timed out" : "phone disconnected");
  char cmd[208];
  if (!phone::fsRequested(cmd, sizeof(cmd))) return;
  // "<seq>:<op>:<arg>"
  char* end;
  s_seq = (uint8_t)strtoul(cmd, &end, 10);
  if (end == cmd || *end != ':') { s_seq = 0; replyErr("bad command"); return; }
  char* rest = end + 1;
  if (!g_sdOk) { replyErr("no SD card"); return; }
  if (strlen(rest) < 2 || rest[1] != ':') { replyErr("bad command"); return; }
  char op = rest[0];
  char* arg = rest + 2;
  if (op == 'l') { listDir(arg); return; }
  if (op == 'r') { renamePath(arg); return; }
  if (op == 'u') { beginUpload(arg); return; }
  if (op == 'a') { abortUpload("cancelled by phone"); replyOk(); return; }
  char path[PATH_MAX_LEN + 4];
  if (!normPath(arg, path, sizeof(path))) { replyErr("bad path"); return; }
  switch (op) {
    case 'm': makeDir(path); break;
    case 'd': removePath(path); break;
    default:  replyErr("unknown op"); break;
  }
}

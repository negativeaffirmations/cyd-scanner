// whitelist.cpp — see whitelist.h.
#include "whitelist.h"
#include <SD.h>
#include "link_protocol.h"  // MASK_*

using namespace link_protocol;

namespace {

constexpr int MAX_RULES = 48;
constexpr int NAME_LEN  = 24;
constexpr int LABEL_LEN = 16;
constexpr int LINE_BUF  = 128;

const char* kPath    = "/whitelist.csv";
const char* kTmpPath = "/whitelist.tmp";  // removeAt() rewrite (replace after complete)

enum Kind : uint8_t { K_MAC, K_MACPFX, K_NEXACT, K_NCONTAINS, K_CID, K_UUID };

struct WlRule {
  uint8_t kind;
  uint8_t srcMask;
  union {
    struct { uint8_t mac[6]; uint8_t len; } m;  // K_MAC (len 6) / K_MACPFX (len 3..5)
    char     name[NAME_LEN];                    // K_NEXACT / K_NCONTAINS
    uint16_t cid;                               // K_CID
    uint8_t  uuid[16];                          // K_UUID (canonical 128-bit, big-endian)
  } v;
  char label[LABEL_LEN];
};

WlRule g_rules[MAX_RULES];
int    g_n = 0;

const char* kSeedCsv =
    "# cyd-scanner whitelist (known, not-a-threat devices; shown muted, never 'following').\n"
    "# kind,pattern,srcmask,label    srcmask: W=wifi B=ble 4=802.15.4 A=any\n"
    "#   mac,AA:BB:CC:DD:EE:FF,A,My phone      exact MAC\n"
    "#   macpfx,AA:BB:CC,A,Car stereo OUI      MAC/OUI prefix (3-5 bytes)\n"
    "#   nexact,Pixel 8,B,My phone             exact name\n"
    "#   ncontains,Buds,B,Earbuds              name substring\n"
    "#   blecid,004C,B,Apple                   BLE company ID (hex)\n"
    "#   bleuuid,<uuid16-or-128>,B,Service     BLE service UUID\n";

// Same canonicalisation as sigdb's bleuuid layer (kept private there): 16-bit short UUIDs
// expand to the Bluetooth base UUID; full 128-bit accepts optional dashes.
bool parseUuid(const char* s, uint8_t out[16]) {
  char hex[33];
  int  n = 0;
  for (const char* p = s; *p && n < 32; ++p) {
    if (*p == '-') continue;
    if (!isxdigit((unsigned char)*p)) return false;
    hex[n++] = *p;
  }
  hex[n] = 0;
  static const uint8_t base[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00,0x10,0x00,
    0x80,0x00,0x00,0x80, 0x5F,0x9B,0x34,0xFB};
  if (n == 4) {
    uint16_t v = (uint16_t)strtoul(hex, nullptr, 16);
    memcpy(out, base, 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v & 0xFF);
    return true;
  }
  if (n == 32) {
    for (int i = 0; i < 16; i++) {
      char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
      out[i] = (uint8_t)strtoul(b, nullptr, 16);
    }
    return true;
  }
  return false;
}

void trim(char* s) {
  int n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    s[--n] = 0;
}

uint8_t srcMaskFromChar(char c) {
  switch (c) {
    case 'W': case 'w': return MASK_WIFI24;
    case 'B': case 'b': return MASK_BLE;
    case '4':           return MASK_154;
    default:            return MASK_ALL;
  }
}

// Parse "AA:BB:CC[:..]" into up to 6 bytes; returns the byte count (0 = invalid).
int parseMac(const char* s, uint8_t out[6]) {
  int n = 0;
  while (*s && n < 6) {
    char* end;
    unsigned long b = strtoul(s, &end, 16);
    if (end == s || end - s > 2 || b > 255) return 0;
    out[n++] = (uint8_t)b;
    s = end;
    if (*s == ':') s++;
    else if (*s) return 0;
  }
  return *s ? 0 : n;
}

// Parse one line into r. Returns true only for a valid rule (comments/blank/bad -> false).
bool parseLine(char* line, WlRule& r) {
  trim(line);
  if (line[0] == 0 || line[0] == '#') return false;

  // kind,pattern,srcmask,label — first 3 commas delimit, remainder is the label.
  char* f[4];
  int nf = 0;
  char* s = line;
  for (int i = 0; i < 3; i++) {
    char* comma = strchr(s, ',');
    if (!comma) return false;
    *comma = 0;
    f[nf++] = s;
    s = comma + 1;
  }
  f[nf++] = s;

  memset(&r, 0, sizeof(r));
  r.srcMask = srcMaskFromChar(f[2][0]);
  strncpy(r.label, f[3], LABEL_LEN - 1);

  if (strcmp(f[0], "mac") == 0 || strcmp(f[0], "macpfx") == 0) {
    int n = parseMac(f[1], r.v.m.mac);
    bool full = f[0][3] == 0;
    if (full ? n != 6 : (n < 3 || n > 5)) return false;
    r.kind = full ? K_MAC : K_MACPFX;
    r.v.m.len = (uint8_t)n;
    return true;
  }
  if (strcmp(f[0], "nexact") == 0 || strcmp(f[0], "ncontains") == 0) {
    if (!f[1][0] || strlen(f[1]) > NAME_LEN - 1) return false;  // match() compares NAME_LEN-1 chars
    r.kind = f[0][1] == 'e' ? K_NEXACT : K_NCONTAINS;
    strncpy(r.v.name, f[1], NAME_LEN - 1);
    return true;
  }
  if (strcmp(f[0], "blecid") == 0) {
    uint16_t cid = (uint16_t)strtoul(f[1], nullptr, 16);
    if (cid == 0) return false;  // 0 = "no company ID" sentinel
    r.kind = K_CID;
    r.v.cid = cid;
    return true;
  }
  if (strcmp(f[0], "bleuuid") == 0) {
    if (!parseUuid(f[1], r.v.uuid)) return false;
    r.kind = K_UUID;
    return true;
  }
  return false;
}

bool readLine(File& f, char* buf, int cap) {
  int n = 0;
  while (f.available()) {
    char c = (char)f.read();
    if (c == '\n') { buf[n] = 0; return true; }
    if (n < cap - 1) buf[n++] = c;
  }
  buf[n] = 0;
  return n > 0;
}

}  // namespace

namespace whitelist {

bool begin() {
  // Recover from an interrupted removeAt() (remove succeeded, rename didn't).
  if (!SD.exists(kPath) && SD.exists(kTmpPath)) SD.rename(kTmpPath, kPath);
  if (!SD.exists(kPath)) {
    File f = SD.open(kPath, FILE_WRITE);
    if (f) { f.print(kSeedCsv); f.close(); }
  }
  return reload();
}

bool reload() {
  g_n = 0;
  File f = SD.open(kPath, "r");
  if (!f) { Serial.println("[wl] no whitelist file"); return false; }
  char line[LINE_BUF];
  while (readLine(f, line, LINE_BUF)) {
    if (g_n >= MAX_RULES) break;  // cap: extra rules skipped
    if (parseLine(line, g_rules[g_n])) g_n++;
  }
  f.close();
  Serial.printf("[wl] loaded %d rules from %s\n", g_n, kPath);
  return true;
}

int count() { return g_n; }

bool match(const uint8_t mac[6], const char* name, uint16_t companyId,
           const uint8_t svc[16], uint8_t srcBits) {
  if (g_n == 0) return false;
  // A probe request is still a Wi-Fi frame: W/A rules match it (as in sigdb).
  if (srcBits & MASK_PROBE) srcBits |= MASK_WIFI24;
  bool haveName = name && name[0];
  bool haveSvc = false;
  if (svc) for (int k = 0; k < 16; k++) if (svc[k]) { haveSvc = true; break; }
  for (int i = 0; i < g_n; i++) {
    const WlRule& r = g_rules[i];
    if (!(r.srcMask & srcBits)) continue;
    switch (r.kind) {
      case K_MAC:
      case K_MACPFX:
        if (mac && memcmp(mac, r.v.m.mac, r.v.m.len) == 0) return true;
        break;
      case K_NEXACT:    if (haveName && strcmp(name, r.v.name) == 0)   return true; break;
      case K_NCONTAINS: if (haveName && strstr(name, r.v.name))        return true; break;
      case K_CID:       if (companyId && companyId == r.v.cid)         return true; break;
      case K_UUID:      if (haveSvc && memcmp(svc, r.v.uuid, 16) == 0) return true; break;
    }
  }
  return false;
}

bool add(const char* csvLine) {
  if (!csvLine || g_n >= MAX_RULES) return false;
  if (strlen(csvLine) > 95) return false;  // one add = one bounded record
  for (const char* c = csvLine; *c; c++) if ((unsigned char)*c < 0x20) return false;
  char tmp[LINE_BUF];
  strncpy(tmp, csvLine, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
  WlRule probe;
  if (!parseLine(tmp, probe)) return false;  // validate before touching the file
  strncpy(tmp, csvLine, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = 0;
  trim(tmp);
  bool needNl = false;  // hand-edited file without a trailing newline
  {
    File r = SD.open(kPath, "r");
    if (r) {
      size_t sz = r.size();
      if (sz > 0 && r.seek(sz - 1)) needNl = (r.read() != '\n');
      r.close();
    }
  }
  File f = SD.open(kPath, FILE_APPEND);
  if (!f) return false;
  if (needNl) f.print('\n');
  size_t w = f.print(tmp) + f.print('\n');
  f.close();
  if (w != strlen(tmp) + 1) return false;
  return reload();
}

bool removeAt(int idx) {
  if (idx < 0 || idx >= g_n) return false;
  File in = SD.open(kPath, "r");
  if (!in) return false;
  SD.remove(kTmpPath);
  File out = SD.open(kTmpPath, FILE_WRITE);
  if (!out) { in.close(); return false; }
  char line[LINE_BUF], orig[LINE_BUF];
  WlRule scratch;
  int  ri = 0;
  bool ok = true;
  while (readLine(in, line, LINE_BUF)) {
    strcpy(orig, line);
    trim(orig);
    bool isRule = parseLine(line, scratch);
    if (isRule && ri++ == idx) continue;  // drop this one
    ok &= out.print(orig) == strlen(orig) && out.print('\n') == 1;
  }
  in.close(); out.close();
  if (!ok) { SD.remove(kTmpPath); return false; }
  SD.remove(kPath);
  if (!SD.rename(kTmpPath, kPath)) return false;
  return reload();
}

bool lineAt(int idx, char* buf, size_t cap) {
  if (idx < 0 || idx >= g_n || !cap) return false;
  File f = SD.open(kPath, "r");
  if (!f) return false;
  char line[LINE_BUF], orig[LINE_BUF];
  WlRule scratch;
  int ri = 0;
  while (readLine(f, line, LINE_BUF)) {
    strcpy(orig, line);
    if (parseLine(line, scratch) && ri++ == idx) {
      f.close();
      trim(orig);
      snprintf(buf, cap, "%s", orig);
      return true;
    }
  }
  f.close();
  return false;
}

void listAll(String& out) {
  File f = SD.open(kPath, "r");
  if (!f) return;
  char line[LINE_BUF], orig[LINE_BUF];
  WlRule scratch;
  int n = 0;
  while (n < g_n && readLine(f, line, LINE_BUF)) {
    strcpy(orig, line);  // parseLine NUL-splits `line`; keep the stored text
    if (!parseLine(line, scratch)) continue;
    n++;
    trim(orig);
    out += orig;
    out += '\n';
  }
  f.close();
}

}  // namespace whitelist

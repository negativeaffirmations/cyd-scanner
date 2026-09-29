// sigdb.cpp — see sigdb.h.
#include "sigdb.h"
#include <SD.h>

using namespace link_protocol;

namespace {

constexpr int MAX_OUI   = 64;
constexpr int MAX_STR   = 48;
constexpr int MAX_IE    = 32;
constexpr int LABEL_LEN = 20;
constexpr int PAT_LEN   = 24;
constexpr int LINE_BUF  = 160;

const char* kDbPath  = "/signatures.csv";
const char* kTmpPath = "/signatures.tmp";  // used by phone push (atomic replace)

enum class StrKind : uint8_t { Exact, Prefix, Contains };

struct OuiRule { uint8_t prefix[3]; uint8_t weight; uint8_t srcMask; char label[LABEL_LEN]; };
struct StrRule { StrKind kind; uint8_t weight; uint8_t srcMask; char pat[PAT_LEN]; char label[LABEL_LEN]; };
struct IeRule  { uint32_t hash; uint8_t weight; uint8_t srcMask; char label[LABEL_LEN]; };

OuiRule  g_oui[MAX_OUI];
StrRule  g_str[MAX_STR];
IeRule   g_ie[MAX_IE];
int      g_ouiN = 0, g_strN = 0, g_ieN = 0;
uint8_t  g_suspect = 40, g_likely = 70, g_confirmed = 100;
bool     g_loaded = false;

// Built-in seed. Written to SD on first run; also the fallback if SD is unavailable.
// Weights are starting points — tune via the supervised-discovery workflow.
// srcmask: W=wifi, B=ble, 4=802.15.4, A=any. NOTE: shared vendor OUIs (Qualcomm,
// Espressif) carry LOW weight so they only escalate when combined with another layer.
const char* kSeedCsv =
    "# cyd-scanner signature DB. kind,pattern,weight,srcmask,label\n"
    "# Expand OUIs from community sources (e.g. flock-you). Edit freely.\n"
    "# 'ie,<8-hex>,...' matches an 802.11 IE fingerprint (see the 'ie' column in\n"
    "# scanlog.csv). No Flock IE hashes are seeded yet — capture them in the field.\n"
    "thresholds,40,70,100\n"
    "oui,B4:1E:52,70,A,Flock IEEE\n"
    "oui,70:C9:4E,40,W,Flock (community)\n"
    "oui,3C:91:80,40,W,Flock (community)\n"
    "oui,D8:F3:BC,40,W,Flock (community)\n"
    "oui,80:30:49,40,W,Flock (community)\n"
    "oui,B8:35:32,40,W,Flock (community)\n"
    "oui,82:6B:F2,40,W,Flock (field)\n"
    "oui,00:03:7F,15,A,Qualcomm (shared)\n"
    "oui,D4:AD:FC,15,A,Espressif (shared)\n"
    "oui,AC:67:B2,15,A,Espressif (shared)\n"
    "oui,84:F3:EB,15,A,Espressif (shared)\n"
    "oui,B4:E6:2D,15,A,Espressif (shared)\n"
    "prefix,Flock,50,W,Flock SoftAP\n"
    "prefix,Penguin-,50,B,Flock Penguin\n"
    "exact,FS Ext Battery,50,B,Flock Penguin batt\n";

void resetTables() { g_ouiN = 0; g_strN = 0; g_ieN = 0; g_suspect = 40; g_likely = 70; g_confirmed = 100; }

void trim(char* s) {
  int n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    s[--n] = 0;
  // (leading spaces are not expected in the schema; left as-is for simplicity)
}

uint8_t srcMaskFromChar(char c) {
  switch (c) {
    case 'W': case 'w': return MASK_WIFI;
    case 'B': case 'b': return MASK_BLE;
    case '4':           return MASK_154;
    default:            return MASK_ALL;  // 'A'/'a'/anything else
  }
}

// Split into up to 5 fields: first 4 commas delimit, the remainder is field 5 (label).
int split5(char* line, char* f[5]) {
  int nf = 0;
  char* s = line;
  for (int i = 0; i < 4; i++) {
    char* comma = strchr(s, ',');
    if (!comma) break;
    *comma = 0;
    f[nf++] = s;
    s = comma + 1;
  }
  f[nf++] = s;
  return nf;
}

void parseLine(char* line) {
  trim(line);
  if (line[0] == 0 || line[0] == '#') return;

  char* f[5];
  int nf = split5(line, f);
  if (nf < 1) return;

  if (strcmp(f[0], "thresholds") == 0 && nf >= 4) {
    g_suspect = (uint8_t)atoi(f[1]);
    g_likely = (uint8_t)atoi(f[2]);
    g_confirmed = (uint8_t)atoi(f[3]);
    return;
  }

  if (strcmp(f[0], "oui") == 0 && nf >= 5) {
    if (g_ouiN >= MAX_OUI) return;  // cap: extra rules skipped
    unsigned b0, b1, b2;
    if (sscanf(f[1], "%x:%x:%x", &b0, &b1, &b2) != 3) return;
    OuiRule& r = g_oui[g_ouiN];
    r.prefix[0] = (uint8_t)b0; r.prefix[1] = (uint8_t)b1; r.prefix[2] = (uint8_t)b2;
    r.weight = (uint8_t)atoi(f[2]);
    r.srcMask = srcMaskFromChar(f[3][0]);
    strncpy(r.label, f[4], LABEL_LEN - 1); r.label[LABEL_LEN - 1] = 0;
    g_ouiN++;
    return;
  }

  if (strcmp(f[0], "ie") == 0 && nf >= 5) {
    if (g_ieN >= MAX_IE) return;  // cap: extra rules skipped
    uint32_t hash = (uint32_t)strtoul(f[1], nullptr, 16);
    if (hash == 0) return;        // 0 is the "no fingerprint" sentinel
    IeRule& r = g_ie[g_ieN];
    r.hash = hash;
    r.weight = (uint8_t)atoi(f[2]);
    r.srcMask = srcMaskFromChar(f[3][0]);
    strncpy(r.label, f[4], LABEL_LEN - 1); r.label[LABEL_LEN - 1] = 0;
    g_ieN++;
    return;
  }

  StrKind kind;
  if      (strcmp(f[0], "exact") == 0)    kind = StrKind::Exact;
  else if (strcmp(f[0], "prefix") == 0)   kind = StrKind::Prefix;
  else if (strcmp(f[0], "contains") == 0) kind = StrKind::Contains;
  else return;
  if (nf < 5 || g_strN >= MAX_STR) return;
  StrRule& r = g_str[g_strN];
  r.kind = kind;
  strncpy(r.pat, f[1], PAT_LEN - 1); r.pat[PAT_LEN - 1] = 0;
  r.weight = (uint8_t)atoi(f[2]);
  r.srcMask = srcMaskFromChar(f[3][0]);
  strncpy(r.label, f[4], LABEL_LEN - 1); r.label[LABEL_LEN - 1] = 0;
  g_strN++;
}

bool readLine(File& f, char* buf, int cap) {
  int n = 0;
  while (f.available()) {
    char c = (char)f.read();
    if (c == '\n') { buf[n] = 0; return true; }
    if (n < cap - 1) buf[n++] = c;
  }
  buf[n] = 0;
  return n > 0;  // last line without trailing newline
}

bool loadFrom(const char* path) {
  resetTables();
  File f = SD.open(path, "r");
  if (!f) return false;
  char line[LINE_BUF];
  while (readLine(f, line, LINE_BUF)) parseLine(line);
  f.close();
  return (g_ouiN + g_strN + g_ieN) > 0;
}

void loadFallback() {
  // Parse the seed straight from RAM so detection works even without SD.
  resetTables();
  char line[LINE_BUF];
  const char* p = kSeedCsv;
  while (*p) {
    int n = 0;
    while (*p && *p != '\n') { if (n < LINE_BUF - 1) line[n++] = *p; p++; }
    line[n] = 0;
    if (*p == '\n') p++;
    parseLine(line);
  }
  g_loaded = false;
}

}  // namespace

namespace sigdb {

bool begin() {
  // Self-seed the card on first run so the full list ships without manual copying.
  if (!SD.exists(kDbPath)) {
    File f = SD.open(kDbPath, FILE_WRITE);
    if (f) { f.print(kSeedCsv); f.close(); }
  }
  return reload();
}

bool reload() {
  if (loadFrom(kDbPath)) {
    g_loaded = true;
    Serial.printf("[sigdb] loaded %d OUI + %d name + %d IE rules from %s (thr %d/%d/%d)\n",
                  g_ouiN, g_strN, g_ieN, kDbPath, g_suspect, g_likely, g_confirmed);
    return true;
  }
  loadFallback();
  Serial.printf("[sigdb] using built-in fallback (%d OUI + %d name + %d IE rules)\n",
                g_ouiN, g_strN, g_ieN);
  return false;
}

void score(const Detection& d, ScoreResult& out) {
  out = ScoreResult{};
  uint8_t srcBit = (uint8_t)(1u << d.source);
  // A promiscuous probe request is still a Wi-Fi frame: let W/A OUI, name and IE
  // rules match its client MAC and fingerprint (its own MASK_PROBE bit lets a rule
  // target probes specifically if it ever wants to).
  if (d.source == (uint8_t)Source::WifiProbe) srcBit |= MASK_WIFI;

  int ouiW = 0;
  for (int i = 0; i < g_ouiN; i++) {
    if (!(g_oui[i].srcMask & srcBit)) continue;
    if (memcmp(d.mac, g_oui[i].prefix, 3) == 0 && g_oui[i].weight > ouiW) {
      ouiW = g_oui[i].weight; out.bestOui = (int8_t)i;
    }
  }

  int strW = 0;
  if (d.name[0]) {
    for (int i = 0; i < g_strN; i++) {
      if (!(g_str[i].srcMask & srcBit)) continue;
      bool m = false;
      switch (g_str[i].kind) {
        case StrKind::Exact:    m = strcmp(d.name, g_str[i].pat) == 0; break;
        case StrKind::Prefix:   m = strncmp(d.name, g_str[i].pat, strlen(g_str[i].pat)) == 0; break;
        case StrKind::Contains: m = strstr(d.name, g_str[i].pat) != nullptr; break;
      }
      if (m && g_str[i].weight > strW) { strW = g_str[i].weight; out.bestStr = (int8_t)i; }
    }
  }

  int ieW = 0;
  if (d.ie_hash) {
    for (int i = 0; i < g_ieN; i++) {
      if (!(g_ie[i].srcMask & srcBit)) continue;
      if (g_ie[i].hash == d.ie_hash && g_ie[i].weight > ieW) {
        ieW = g_ie[i].weight; out.bestIe = (int8_t)i;
      }
    }
  }

  int total = ouiW + strW + ieW;
  out.score = (uint8_t)(total > 255 ? 255 : total);
  out.tier = out.score >= g_confirmed ? Tier::Confirmed
           : out.score >= g_likely    ? Tier::Likely
           : out.score >= g_suspect   ? Tier::Suspect
                                      : Tier::None;
}

const char* labelFor(const ScoreResult& r) {
  int ow = r.bestOui >= 0 ? g_oui[r.bestOui].weight : 0;
  int sw = r.bestStr >= 0 ? g_str[r.bestStr].weight : 0;
  int iw = r.bestIe  >= 0 ? g_ie[r.bestIe].weight   : 0;
  if (ow == 0 && sw == 0 && iw == 0) return "";
  if (iw >= ow && iw >= sw) return g_ie[r.bestIe].label;
  return (ow >= sw) ? g_oui[r.bestOui].label : g_str[r.bestStr].label;
}

const char* tierName(Tier t) {
  switch (t) {
    case Tier::Suspect:   return "suspect";
    case Tier::Likely:    return "likely";
    case Tier::Confirmed: return "confirmed";
    default:              return "none";
  }
}

bool     loaded()    { return g_loaded; }
uint16_t ruleCount() { return (uint16_t)(g_ouiN + g_strN + g_ieN); }

}  // namespace sigdb

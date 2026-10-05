// sigdb.cpp — see sigdb.h.
#include "sigdb.h"
#include <SD.h>

using namespace link_protocol;

namespace {

constexpr int MAX_OUI   = 96;
constexpr int MAX_STR   = 48;
constexpr int MAX_IE    = 32;
constexpr int MAX_UUID  = 24;
constexpr int MAX_CID   = 24;
constexpr int LABEL_LEN = 20;
constexpr int PAT_LEN   = 24;
constexpr int LINE_BUF  = 160;

const char* kDbPath  = "/signatures.csv";
const char* kTmpPath = "/signatures.tmp";  // used by phone push (atomic replace)

enum class StrKind : uint8_t { Exact, Prefix, Contains };

struct OuiRule  { uint8_t prefix[3]; uint8_t weight; uint8_t srcMask; char label[LABEL_LEN]; };
struct StrRule  { StrKind kind; uint8_t weight; uint8_t srcMask; char pat[PAT_LEN]; char label[LABEL_LEN]; };
struct IeRule   { uint32_t hash; uint8_t weight; uint8_t srcMask; char label[LABEL_LEN]; };
struct UuidRule { uint8_t uuid[16]; uint8_t weight; uint8_t srcMask; char label[LABEL_LEN]; };
struct CidRule  { uint16_t cid; uint8_t weight; uint8_t srcMask; char label[LABEL_LEN]; };

OuiRule  g_oui[MAX_OUI];
StrRule  g_str[MAX_STR];
IeRule   g_ie[MAX_IE];
UuidRule g_uuid[MAX_UUID];
CidRule  g_cid[MAX_CID];
int      g_ouiN = 0, g_strN = 0, g_ieN = 0, g_uuidN = 0, g_cidN = 0;
uint8_t  g_suspect = 40, g_likely = 70, g_confirmed = 100;
bool     g_loaded = false;

// Built-in seed. Written to SD on first run; also the fallback if SD is unavailable.
// Weights are starting points — tune via the supervised-discovery workflow.
// srcmask: W=wifi, B=ble, 4=802.15.4, A=any. NOTE: shared vendor OUIs (Qualcomm,
// Espressif) carry LOW weight so they only escalate when combined with another layer.
const char* kSeedCsv =
    "# cyd-scanner signature DB. kind,pattern,weight,srcmask,label\n"
    "# Expand OUIs from community sources (e.g. flock-you). Edit freely.\n"
    "# 'ie,<8-hex>,...' matches an 802.11 IE fingerprint (see the 'ie' field in\n"
    "# scanlog.jsonl). No Flock IE hashes are seeded yet — capture them in the field.\n"
    "# 'bleuuid,<uuid>,...' matches a BLE service UUID (16-bit or full 128-bit);\n"
    "# 'blecid,<hex>,...' matches a BLE manufacturer company ID (see the uuid/cid cols).\n"
    "# 802.15.4 devices: the EUI-64 OUI is matched like a MAC OUI; use srcmask 4 (or A):\n"
    "#   oui,<AA:BB:CC>,<weight>,4,<label>\n"
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
    "exact,FS Ext Battery,50,B,Flock Penguin batt\n"
    "bleuuid,e8ccbb38-9532-46a8-9fe5-1814df172e6f,60,B,Flock GATT\n"
    // Signature data below adapted from SquachWatch-CYD (https://github.com/skizzophrenic/SquachWatch-CYD),
    // GPL-3.0, commit f49ecbe, src/signatures.cpp. cyd-scanner is GPL-3.0 (see /LICENSE).
    // Upstream credits carried forward: Flock OUIs via colonelpanichacks/flock-you (MIT) + DeFlock community;
    // Ring/Verkada/Avigilon/Axis/Motorola OUIs from the public IEEE MA-L registry; Flipper via IEEE + BT SIG.
    "oui,00:25:DF,70,A,Axon\n"
    "oui,E4:05:40,15,A,Axon-Body\n"
    "oui,28:24:FF,15,A,Axon-Signal\n"
    "oui,00:04:7D,40,W,ALPR-Motorola\n"
    "oui,00:18:85,40,W,ALPR-Motorola\n"
    "oui,00:1F:92,40,W,ALPR-Motorola\n"
    "oui,4C:CC:34,40,W,ALPR-Motorola\n"
    "oui,B8:E2:8C,40,W,ALPR-Motorola\n"
    "oui,00:BF:15,40,W,ALPR-Genetec\n"
    "oui,0C:BF:15,40,W,ALPR-Genetec\n"
    "oui,2C:AA:8E,70,W,Wyze\n"
    "oui,D0:3F:27,70,W,Wyze\n"
    "oui,7C:78:B2,70,W,Wyze\n"
    "oui,B8:D7:AF,15,W,Wyze-Mod\n"
    "oui,34:D2:70,40,W,Amazon-Cam\n"
    "oui,F0:27:2D,40,W,Amazon-Cam\n"
    "oui,C0:56:E3,70,W,Hikvision\n"
    "oui,44:19:B6,70,W,Hikvision\n"
    "oui,28:57:BE,70,W,Hikvision\n"
    "oui,00:E0:4C,15,W,Realtek-Cam\n"
    "oui,BC:DD:C2,15,W,Arlo\n"
    "oui,4C:69:05,15,W,Blink\n"
    "oui,A4:C1:38,15,W,Tuya-Cam\n"
    "oui,E0:A7:00,70,W,Verkada\n"
    "oui,70:1A:D5,70,W,Avigilon\n"
    "oui,00:40:8C,70,W,Axis-Cam\n"
    "oui,B8:A4:4F,70,W,Axis-Cam\n"
    "oui,FC:65:DE,40,W,Ring\n"
    "oui,68:37:E9,40,W,Ring\n"
    "oui,AC:9F:C3,70,W,Ring\n"
    "oui,18:7F:88,70,W,Ring\n"
    "oui,34:3E:A4,70,W,Ring\n"
    "oui,54:E0:19,70,W,Ring\n"
    "oui,5C:47:5E,70,W,Ring\n"
    "oui,64:9A:63,70,W,Ring\n"
    "oui,90:48:6C,70,W,Ring\n"
    "oui,9C:76:13,70,W,Ring\n"
    "oui,CC:3B:FB,70,W,Ring\n"
    "oui,C4:DB:AD,70,W,Ring\n"
    "oui,24:2B:D6,70,W,Ring\n"
    "oui,00:B4:63,70,W,Ring\n"
    "oui,50:E4:67,70,W,Ring\n"
    "oui,0C:FA:22,70,A,Flipper\n"
    "oui,02:C0:CA,15,W,Hak5-LA\n"
    "oui,02:13:37,15,W,Hak5-LA\n"
    "oui,8C:AA:B5,15,W,Flock-ESP-S3\n"
    "oui,34:85:18,15,W,Flock-ESP-S3\n"
    "oui,A4:CF:12,15,W,Flock-ESP-S2\n"
    "oui,C0:49:EF,15,W,Flock-ESP-C6\n"
    "oui,08:3A:88,15,W,Flock-UGSI\n"
    "bleuuid,FD5F,70,B,RayBanMeta\n"
    "bleuuid,FEED,70,B,Tile\n"
    "bleuuid,FEEC,70,B,Tile\n"
    "bleuuid,FD5A,70,B,Samsung-SmartTag\n"
    "bleuuid,FEAA,40,B,GoogleFindMy\n"
    "bleuuid,FFFA,40,B,OpenDroneID\n"
    "bleuuid,3100,40,B,Raven\n"
    "bleuuid,3200,40,B,Raven\n"
    "bleuuid,3300,40,B,Raven\n"
    "bleuuid,3400,40,B,Raven\n"
    "bleuuid,3500,40,B,Raven\n"
    "bleuuid,3081,70,B,Flipper\n"
    "bleuuid,3082,70,B,Flipper\n"
    "bleuuid,3083,70,B,Flipper\n"
    "blecid,09C8,40,B,Flock-XUNTONG\n"
    "blecid,01AB,15,B,Meta\n"
    "blecid,058E,15,B,Meta-Tech\n"
    "blecid,0D53,40,B,Luxottica\n"
    "blecid,03C2,40,B,Snap-Spectacles\n"
    "blecid,004C,15,B,Apple\n"
    "blecid,0E29,70,B,Flipper\n"
    "contains,Pigvision,40,B,Flock-BLE\n"
    "exact,Flock_Setup,50,B,Flock-Setup\n"
    "prefix,flock-,50,W,Flock-Setup\n"
    "prefix,AB2-,70,W,Axon-Body2\n"
    "prefix,AB3-,70,W,Axon-Body3\n"
    "prefix,AB4-,70,W,Axon-Body4\n"
    "prefix,AXON-,70,W,Axon-Field\n"
    "contains,Axon,40,B,Axon-BLE\n"
    "prefix,Pineapple_,40,W,WiFi-Pineapple\n"
    "prefix,pwned,40,W,Deauther-AP\n"
    "contains,Flipper,40,B,Flipper-name\n";

void resetTables() {
  g_ouiN = g_strN = g_ieN = g_uuidN = g_cidN = 0;
  g_suspect = 40; g_likely = 70; g_confirmed = 100;
}

// Parse a service-UUID string into 16 canonical (big-endian) bytes. Accepts a full
// 128-bit UUID (dashes optional) or a 16-bit short UUID, which expands to the
// Bluetooth base UUID 0000xxxx-0000-1000-8000-00805f9b34fb.
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
  if (n == 4) {                       // 16-bit short UUID
    uint16_t v = (uint16_t)strtoul(hex, nullptr, 16);
    memcpy(out, base, 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)(v & 0xFF);
    return true;
  }
  if (n == 32) {                      // full 128-bit UUID
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
  // (leading spaces are not expected in the schema; left as-is for simplicity)
}

uint8_t srcMaskFromChar(char c) {
  switch (c) {
    case 'W': case 'w': return MASK_WIFI24;
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

  if (strcmp(f[0], "bleuuid") == 0 && nf >= 5) {
    if (g_uuidN >= MAX_UUID) return;  // cap: extra rules skipped
    UuidRule& r = g_uuid[g_uuidN];
    if (!parseUuid(f[1], r.uuid)) return;  // bad UUID -> skip
    r.weight = (uint8_t)atoi(f[2]);
    r.srcMask = srcMaskFromChar(f[3][0]);
    strncpy(r.label, f[4], LABEL_LEN - 1); r.label[LABEL_LEN - 1] = 0;
    g_uuidN++;
    return;
  }

  if (strcmp(f[0], "blecid") == 0 && nf >= 5) {
    if (g_cidN >= MAX_CID) return;  // cap: extra rules skipped
    uint16_t cid = (uint16_t)strtoul(f[1], nullptr, 16);
    if (cid == 0) return;  // 0 is the "no company ID" sentinel
    CidRule& r = g_cid[g_cidN];
    r.cid = cid;
    r.weight = (uint8_t)atoi(f[2]);
    r.srcMask = srcMaskFromChar(f[3][0]);
    strncpy(r.label, f[4], LABEL_LEN - 1); r.label[LABEL_LEN - 1] = 0;
    g_cidN++;
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
  return (g_ouiN + g_strN + g_ieN + g_uuidN + g_cidN) > 0;
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
    Serial.printf("[sigdb] loaded %d OUI + %d name + %d IE + %d bleuuid + %d blecid rules "
                  "from %s (thr %d/%d/%d)\n",
                  g_ouiN, g_strN, g_ieN, g_uuidN, g_cidN, kDbPath,
                  g_suspect, g_likely, g_confirmed);
    return true;
  }
  loadFallback();
  Serial.printf("[sigdb] using built-in fallback (%d OUI + %d name + %d IE + %d bleuuid + %d blecid)\n",
                g_ouiN, g_strN, g_ieN, g_uuidN, g_cidN);
  return false;
}

void score(const Detection& d, ScoreResult& out) {
  out = ScoreResult{};
  uint8_t srcBit = (uint8_t)(1u << d.source);
  // A promiscuous probe request is still a Wi-Fi frame: let W/A OUI, name and IE
  // rules match its client MAC and fingerprint (its own MASK_PROBE bit lets a rule
  // target probes specifically if it ever wants to).
  if (d.source == (uint8_t)Source::WifiProbe) srcBit |= MASK_WIFI24;

  int ouiW = 0;
  // A short 802.15.4 address is 2 bytes, not an OUI: skip the OUI layer for it.
  const bool skipOui = d.source == (uint8_t)Source::Ieee802154 && !(d.flags & FLAG_154_EXTENDED);
  for (int i = 0; i < g_ouiN && !skipOui; i++) {
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

  int uuidW = 0;
  bool haveSvc = false;
  for (int k = 0; k < 16; k++) if (d.svc[k]) { haveSvc = true; break; }
  if (haveSvc) {
    for (int i = 0; i < g_uuidN; i++) {
      if (!(g_uuid[i].srcMask & srcBit)) continue;
      if (memcmp(d.svc, g_uuid[i].uuid, 16) == 0 && g_uuid[i].weight > uuidW) {
        uuidW = g_uuid[i].weight; out.bestUuid = (int8_t)i;
      }
    }
  }

  int cidW = 0;
  if (d.companyId) {
    for (int i = 0; i < g_cidN; i++) {
      if (!(g_cid[i].srcMask & srcBit)) continue;
      if (g_cid[i].cid == d.companyId && g_cid[i].weight > cidW) {
        cidW = g_cid[i].weight; out.bestCid = (int8_t)i;
      }
    }
  }

  int total = ouiW + strW + ieW + uuidW + cidW;
  out.score = (uint8_t)(total > 255 ? 255 : total);
  out.tier = out.score >= g_confirmed ? Tier::Confirmed
           : out.score >= g_likely    ? Tier::Likely
           : out.score >= g_suspect   ? Tier::Suspect
                                      : Tier::None;
}

const char* labelFor(const ScoreResult& r) {
  // Return the label of the single highest-weight matched layer.
  int         best = 0;
  const char* lbl  = "";
  if (r.bestOui  >= 0 && g_oui[r.bestOui].weight   > best) { best = g_oui[r.bestOui].weight;   lbl = g_oui[r.bestOui].label; }
  if (r.bestStr  >= 0 && g_str[r.bestStr].weight   > best) { best = g_str[r.bestStr].weight;   lbl = g_str[r.bestStr].label; }
  if (r.bestIe   >= 0 && g_ie[r.bestIe].weight     > best) { best = g_ie[r.bestIe].weight;     lbl = g_ie[r.bestIe].label; }
  if (r.bestUuid >= 0 && g_uuid[r.bestUuid].weight > best) { best = g_uuid[r.bestUuid].weight; lbl = g_uuid[r.bestUuid].label; }
  if (r.bestCid  >= 0 && g_cid[r.bestCid].weight   > best) { best = g_cid[r.bestCid].weight;   lbl = g_cid[r.bestCid].label; }
  return lbl;
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
uint16_t ruleCount() { return (uint16_t)(g_ouiN + g_strN + g_ieN + g_uuidN + g_cidN); }

}  // namespace sigdb

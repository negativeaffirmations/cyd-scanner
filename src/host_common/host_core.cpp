// host_core.cpp — see host_core.h. Code moved verbatim from the CYD main.cpp (docs/mini-build.md §4).
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <algorithm>
#include "host_core.h"
#include "host_link.h"
#include "host_log.h"
#include "host_status.h"
#include "host_board.h"
#include "host_ui.h"
#include "phone.h"
#include "webshare.h"
#include "whitelist.h"

const char* srcTag(const Detection& d) {
  if (d.source == (uint8_t)Source::BleScan)   return "BLE";
  if (d.source == (uint8_t)Source::WifiProbe) return "PRB";  // Wi-Fi client probe
  if (d.source == (uint8_t)Source::Ieee802154) return "154";  // Zigbee / Thread
  return d.channel > 14 ? "5G" : "2.4";
}

// 0=2.4, 1=5G, 2=BLE, 3=PRB, 4=154 (stored in LogIdx).
uint8_t srcTypeOf(const char* tag) {
  if (!strcmp(tag, "BLE")) return 2;
  if (!strcmp(tag, "PRB")) return 3;
  if (!strcmp(tag, "154")) return 4;
  if (!strcmp(tag, "5G"))  return 1;
  return 0;
}

Detection          g_dets[MAX_DET];
sigdb::ScoreResult g_score[MAX_DET];  // aligned with g_dets
int                g_detCount = 0;
// Decoded Remote-ID broadcasts (Reply::RemoteId), keyed by emitter MAC. Same lifecycle as g_dets:
// reset at the top of each requestScan() so it mirrors the current burst.
RemoteId           g_rids[MAX_RID];
int                g_ridStoreCount = 0;
const RemoteId* ridByMac(const uint8_t* mac) {
  for (int i = 0; i < g_ridStoreCount; i++)
    if (memcmp(g_rids[i].mac, mac, 6) == 0) return &g_rids[i];
  return nullptr;
}
bool               g_linkOk   = false;

// --- Scan-source enable mask (ScanConfig.sources), persisted in NVS. The "2.4" toggle
// covers probe-request capture too (both are 2.4 GHz Wi-Fi). 802.15.4 has its own toggle. ---
uint8_t g_srcMask = (MASK_ALL & ~MASK_154);
void loadSrcMask() {
  Preferences p; p.begin("cydui", true);
  g_srcMask = p.getUChar("srcmask", (MASK_ALL & ~MASK_154));
  p.end();
}
void saveSrcMask() {
  Preferences p; p.begin("cydui", false);
  p.putUChar("srcmask", g_srcMask);
  p.end();
}

// --- Background scan: when ON the scan cycle keeps running on any (non-SD-heavy) screen, headless,
// so detection/logging continues while the user is on the menu / Phone Link. Persisted in NVS. ---
// Default ON. BGDEF_GEN (like DB_GEN) force-flips already-provisioned units to ON exactly once: bump it
// only to re-apply a new default (it overwrites a user's saved choice).
static constexpr uint32_t BGDEF_GEN = 1;
bool g_bgScan = true;
void loadBgScan() {
  Preferences p; p.begin("cydui", false);
  if (p.getULong("bgdef_gen", 0) < BGDEF_GEN) {
    p.putBool("bgscan", true);
    p.putULong("bgdef_gen", BGDEF_GEN);
  }
  g_bgScan = p.getBool("bgscan", true);
  p.end();
}
void saveBgScan() {
  Preferences p; p.begin("cydui", false);
  p.putBool("bgscan", g_bgScan);
  p.end();
}

void countBands(int& n24, int& n5, int& nble, int& nprb, int& n154) {
  n24 = n5 = nble = nprb = n154 = 0;
  for (int i = 0; i < g_detCount; i++) {
    switch (g_dets[i].source) {
      case (uint8_t)Source::BleScan:   nble++; break;
      case (uint8_t)Source::WifiProbe: nprb++; break;
      case (uint8_t)Source::Ieee802154: n154++; break;
      default: (g_dets[i].channel > 14 ? n5 : n24)++; break;
    }
  }
}

// --- C5 behavioral-detector flags (Detection.flags, protocol v6). Notable flags carry a minimum
// threat tier (applied after sigdb::score), a short on-screen label that overrides the sigdb
// signature label, and a lowercase token for the NDJSON "flags" key. iBeacon is informational
// only (no tier bump). Order = label priority when several are set. ---
struct FlagRule { uint16_t bit; sigdb::Tier floor; const char* label; const char* token; };
static const FlagRule kFlagRules[] = {
  { FLAG_BLE_FINDMY,      sigdb::Tier::Suspect, "Find My",    "findmy"     },
  { FLAG_WIFI_PWNAGOTCHI, sigdb::Tier::Likely,  "Pwnagotchi", "pwnagotchi" },
  { FLAG_WIFI_EVILTWIN,   sigdb::Tier::Likely,  "Evil Twin",  "eviltwin"   },
  { FLAG_BLE_ODID,        sigdb::Tier::Suspect, "Drone RID",  "droneid"    },
  { FLAG_BLE_IBEACON,     sigdb::Tier::None,    "iBeacon",    "ibeacon"    },
};
static char g_flagLabel[MAX_DET][16];  // aligned with g_dets; "" when no notable flag

const char* const kCatName[CAT_N] = {
  "", "Flock", "Axon", "ALPR", "Cam", "Ring", "Raven",
  "Glass", "Tracker", "Drone", "Deauth", "Flipper", "Skim", "Other"
};

uint8_t g_category[MAX_DET];  // aligned with g_dets; filled by computeScores()

// '+'-joined token list of the notable flags in `flags` ("" if none).
void flagTokens(uint16_t flags, char* out, size_t cap) {
  size_t o = 0;
  if (cap) out[0] = 0;
  for (const FlagRule& r : kFlagRules) {
    if (!(flags & r.bit)) continue;
    int w = snprintf(out + o, cap - o, o ? "+%s" : "%s", r.token);
    if (w < 0 || (size_t)w >= cap - o) { out[o] = 0; break; }
    o += w;
  }
}

// Label to show for detection i: the flag label when one exists, else the sigdb signature label.
const char* detLabel(int i) {
  return g_flagLabel[i][0] ? g_flagLabel[i] : sigdb::labelFor(g_score[i]);
}

// Map one detection to a category. Behavioral flags win over the sigdb label (they are the most
// specific signal); otherwise the roster label is matched by substring (labels are the
// /signatures.csv strings, e.g. "Flock IEEE", "Axon-Body", "ALPR-Motorola", "Wyze", "Ring").
Category categoryOf(int i) {
  uint16_t fl = g_dets[i].flags;
  if (fl & FLAG_BLE_ODID)                                return CAT_DRONE;
  if (fl & FLAG_BLE_FINDMY)                              return CAT_TRACKER;
  if (fl & (FLAG_WIFI_PWNAGOTCHI | FLAG_WIFI_EVILTWIN))  return CAT_DEAUTH;
  const char* l = detLabel(i);
  if (!l || !l[0]) return CAT_NONE;
  if (strstr(l, "Flock"))       return CAT_FLOCK;
  if (strstr(l, "Axon"))        return CAT_AXON;
  if (strstr(l, "ALPR"))        return CAT_ALPR;
  if (strstr(l, "Raven"))       return CAT_RAVEN;
  if (strstr(l, "Ring"))        return CAT_RING;
  if (strstr(l, "OpenDroneID")) return CAT_DRONE;
  if (strstr(l, "Tile") || strstr(l, "SmartTag") || strstr(l, "FindMy") || strstr(l, "Find My"))
    return CAT_TRACKER;
  if (strstr(l, "RayBan") || strstr(l, "Meta") || strstr(l, "Luxottica") ||
      strstr(l, "Spectacles") || strstr(l, "Snap"))
    return CAT_GLASS;
  if (strstr(l, "Flipper"))     return CAT_FLIPPER;
  if (strstr(l, "Pineapple") || strstr(l, "Deauth") || strstr(l, "pwned") ||
      strstr(l, "Pwnagotchi") || strstr(l, "Hak5"))
    return CAT_DEAUTH;
  if (strstr(l, "Wyze") || strstr(l, "Hikvision") || strstr(l, "Arlo") || strstr(l, "Blink") ||
      strstr(l, "Tuya") || strstr(l, "Realtek") || strstr(l, "Amazon-Cam") || strstr(l, "Verkada") ||
      strstr(l, "Avigilon") || strstr(l, "Axis") || strstr(l, "Nest") || strstr(l, "Reolink") ||
      strstr(l, "SimpliSafe") || strstr(l, "Cam"))
    return CAT_CAM;
  if (strstr(l, "Skim"))        return CAT_SKIM;
  return CAT_OTHER;
}

// Score every current detection against the signature DB (aligned into g_score[]), then raise
// the tier to at least each set flag's floor and record the flag label.
void computeScores() {
  for (int i = 0; i < g_detCount; i++) {
    sigdb::score(g_dets[i], g_score[i]);
    g_flagLabel[i][0] = 0;
    for (const FlagRule& r : kFlagRules) {
      if (!(g_dets[i].flags & r.bit)) continue;
      if (r.floor > g_score[i].tier) g_score[i].tier = r.floor;
      if (!g_flagLabel[i][0]) snprintf(g_flagLabel[i], sizeof(g_flagLabel[i]), "%s", r.label);
    }
    g_category[i] = (uint8_t)categoryOf(i);  // after score+flags so detLabel() is final
  }
}

// --- Aggregate C5 events (Status.deauth_recent / evil_count / rid_count, protocol v6). The values
// are refreshed by every requestScan(); an alert is held for EVENT_HOLD_MS so a one-cycle burst
// stays visible across the ~2 s poll cadence. Receive-side only: these are counts of frames the C5
// OBSERVED, nothing is transmitted. ---
uint16_t g_deauthRecent = 0;
uint8_t  g_evilCount = 0, g_ridCount = 0;

// Unified position: phone GPS is PREFERRED, the C5's onboard NEO-6M fix (Status, v9) is the
// FALLBACK. With no onboard GPS the Status carries gpsFix=0, so this is phone-only as before.
bool    g_onbValid = false;
float   g_onbLat = 0, g_onbLon = 0;
uint8_t g_onbSats = 0;   // satellites the onboard NEO-6M reports (0 = none; nonzero even pre-fix during acquisition)
bool  posHasFix() { return phone::hasGps() || g_onbValid; }
float posLat()    { return phone::hasGps() ? phone::lat() : g_onbLat; }
float posLon()    { return phone::hasGps() ? phone::lon() : g_onbLon; }
int   posSource() { return phone::hasGps() ? 1 : (g_onbValid ? 2 : 0); }  // 0 none / 1 phone / 2 onboard

// Unified wall-clock: the phone's synced time is PREFERRED; the onboard GPS's UTC (NEO-6M RMC, v10)
// is a FALLBACK so the device timestamps logs with no phone, as long as it has a fix. The GPS epoch
// is latched on first receipt and free-runs on the CYD's millis() (refreshed whenever a newer GPS
// epoch arrives, correcting drift). GPS gives UTC only, so we add the last phone tz offset (persisted
// in NVS) to show LOCAL time — matching phone::epochNow()'s local convention; 0 offset (never synced
// to a phone) means the fallback reads UTC until the first-ever phone sync teaches it the zone.
uint32_t g_gpsEpochUtc = 0;   // last UTC epoch from Status.gpsEpoch (0 = never)
uint32_t g_gpsEpochAtMs = 0;  // millis() when it was latched
// DS3231 RTC (docs/mini-build.md §6): lowest-priority persistent fallback, latched at boot by the board
// (hostboard::begin) only when rtcPresent(); 0 = none, so the CYD (no RTC) is unaffected.
uint32_t g_rtcEpochUtc = 0;   // UTC epoch read from the RTC (0 = none/invalid)
uint32_t g_rtcAtMs = 0;       // millis() when it was latched
// RTC write hook, installed by the board (S3) only. A hook, not a direct rtc:: call, so boards without an
// RTC (the CYD) never link the I2C driver (~32 KB) through this core.
void (*g_rtcWrite)(uint32_t utc) = nullptr;
bool     timeHasSync() { return phone::hasTime() || g_gpsEpochUtc != 0 || g_rtcEpochUtc != 0; }
int      timeSource()  { return phone::hasTime() ? 1 : (g_gpsEpochUtc ? 2 : (g_rtcEpochUtc ? 3 : 0)); }  // 0 none / 1 phone / 2 GPS / 3 RTC
uint32_t epochLocal() {
  if (phone::hasTime()) return phone::epochNow();
  if (g_gpsEpochUtc) {
    uint32_t utc = g_gpsEpochUtc + (millis() - g_gpsEpochAtMs) / 1000;
    return utc - (uint32_t)(phone::tzOffsetMin() * 60);  // UTC-local offset -> local
  }
  if (g_rtcEpochUtc) {
    uint32_t utc = g_rtcEpochUtc + (millis() - g_rtcAtMs) / 1000;
    return utc - (uint32_t)(phone::tzOffsetMin() * 60);  // RTC holds UTC: same offset convention as GPS
  }
  return 0;
}

// Write-back sink: when the phone OR the onboard GPS has the time, discipline the RTC with that UTC —
// first chance after boot, then at most hourly. Live-read priority stays phone > GPS > RTC (the RTC is
// never a source for itself). No-op unless the board has an RTC. Called every loop from
// core::servicePhoneCommands().
void disciplineRtc() {
  if (!g_rtcWrite || !hostboard::rtcPresent()) return;
  static bool     done = false;
  static uint32_t lastMs = 0;
  if (done && millis() - lastMs < 3600000UL) return;
  uint32_t utc = 0;
  if (phone::hasTime()) utc = phone::epochNow() + (uint32_t)(phone::tzOffsetMin() * 60);  // local -> UTC
  else if (g_gpsEpochUtc) utc = g_gpsEpochUtc + (millis() - g_gpsEpochAtMs) / 1000;
  if (utc < 1700000000UL) return;  // nothing trustworthy yet (before ~2023)
  g_rtcWrite(utc);
  g_rtcEpochUtc = utc; g_rtcAtMs = millis();
  done = true; lastMs = millis();
  Serial.printf("[CYD] RTC disciplined from %s (utc %lu)\n", phone::hasTime() ? "phone" : "GPS", (unsigned long)utc);
}
uint16_t g_deauthShown = 0;   // values captured when the alert was last raised (banner text)
uint8_t  g_evilShown = 0;
uint32_t g_deauthUntil = 0, g_evilUntil = 0;  // millis() deadlines (0 = never raised)
bool deauthAlertActive() { return g_deauthUntil && (int32_t)(g_deauthUntil - millis()) > 0; }
bool evilAlertActive()   { return g_evilUntil   && (int32_t)(g_evilUntil   - millis()) > 0; }
bool eventAlertActive()  { return deauthAlertActive() || evilAlertActive(); }

// Raise/hold the deauth + evil-twin alerts from the latest C5 Status and log an event line for each
// type at most once per EVENT_LOG_GAP_MS (never per cycle).
void updateEvents() {
  uint32_t now = millis();
  static uint32_t lastLog[2] = {0, 0};
  static bool     logged[2]  = {false, false};
  if (g_deauthRecent >= DEAUTH_ALERT) {
    g_deauthUntil = now + EVENT_HOLD_MS; if (!g_deauthUntil) g_deauthUntil = 1;
    g_deauthShown = g_deauthRecent;
    if (!logged[0] || now - lastLog[0] >= EVENT_LOG_GAP_MS) {
      logged[0] = true; lastLog[0] = now; logEvent("deauth", g_deauthRecent);
    }
  }
  if (g_evilCount > 0) {
    g_evilUntil = now + EVENT_HOLD_MS; if (!g_evilUntil) g_evilUntil = 1;
    g_evilShown = g_evilCount;
    if (!logged[1] || now - lastLog[1] >= EVENT_LOG_GAP_MS) {
      logged[1] = true; lastLog[1] = now; logEvent("eviltwin", g_evilCount);
    }
  }
}

// Fill idx[0..g_detCount) with detection indices sorted threat-tier-first then
// RSSI — the order shown on screen and streamed to the phone. Returns the count.
int buildSorted(int* idx) {
  for (int i = 0; i < g_detCount; i++) idx[i] = i;
  std::sort(idx, idx + g_detCount, [](int a, int b) {
    if (g_score[a].tier != g_score[b].tier) return g_score[a].tier > g_score[b].tier;
    return g_dets[a].rssi > g_dets[b].rssi;
  });
  return g_detCount;
}

int      g_sortIdx[MAX_DET];   // detections sorted tier-first then RSSI (see buildGroups)
DevGroup g_groups[MAX_DET];
int      g_groupCount = 0;

// Name to show for a device: its advertised name, or — when it has none — its MAC as text, so
// nameless devices stay individually identifiable instead of all collapsing to one "<hidden>".
// Returns either a pointer into g_dets[] (persistent) or a single static buffer; every caller
// consumes the result (copies/draws it) before the next call, so one buffer is safe.
const char* groupName(const DevGroup* g) {
  if (g && g->nameIdx >= 0 && g_dets[g->nameIdx].name[0]) return g_dets[g->nameIdx].name;
  static char buf[28];  // "<vendor> AA:BB" or "AA:BB:CC:DD:EE:FF" + NUL
  if (!g) { snprintf(buf, sizeof(buf), "?"); return buf; }
  // Nameless: if the OUI resolves to a known vendor (sigdb 'vendor,' rule), show "<vendor> <tail>" —
  // human-readable yet still distinguishable between same-vendor devices. Only for a real
  // (globally-administered) MAC that isn't a BLE rotating RPA, so randomized addresses can't be
  // mislabelled. Vendor lookup is display-only; it never affects scoring. Falls back to the raw MAC.
  bool realOui = (g->mac[0] & 0x02) == 0 &&                                     // IEEE globally-administered
                 !((g->srcMask & MASK_BLE) && (g->mac[0] & 0xC0) == 0x40);      // not a BLE RPA (top bits 01)
  const char* v = realOui ? sigdb::vendorFor(g->mac) : "";
  if (v && v[0])
    snprintf(buf, sizeof(buf), "%.14s %02X:%02X", v, g->mac[4], g->mac[5]);
  else
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             g->mac[0], g->mac[1], g->mac[2], g->mac[3], g->mac[4], g->mac[5]);
  return buf;
}

bool     g_scanActive = false;   // C5 polling on (START/STOP); independent of the screen
uint32_t g_lastCycleMs = 0;
uint32_t g_lastLinkOkMs = 0;                // millis() of the last successful scan exchange / ping

int buildGroups() {
  buildSorted(g_sortIdx);
  int n = 0;
  for (int r = 0; r < g_detCount; r++) {
    int i = g_sortIdx[r];
    const Detection& d = g_dets[i];
    int gi = -1;
    for (int k = 0; k < n; k++)
      if (memcmp(g_groups[k].mac, d.mac, 6) == 0) { gi = k; break; }
    if (gi < 0) {
      gi = n++;
      DevGroup& g = g_groups[gi];
      memcpy(g.mac, d.mac, 6);
      g.tier = (int)g_score[i].tier; g.score = g_score[i].score; g.bestRssi = d.rssi; g.rep = i;
      g.nameIdx = -1; g.ie = 0; g.companyId = 0; g.whitelisted = false; g.srcMask = 0; g.flags = 0;
    }
    DevGroup& g = g_groups[gi];
    if ((int)g_score[i].tier > g.tier) g.tier = (int)g_score[i].tier;
    if ((int)g_score[i].score > g.score) g.score = (int)g_score[i].score;
    if (d.rssi > g.bestRssi) { g.bestRssi = d.rssi; g.rep = i; }
    if (g.nameIdx < 0 && d.name[0]) g.nameIdx = i;
    if (!g.ie && d.ie_hash) g.ie = d.ie_hash;
    g.flags |= (uint16_t)(d.flags & FLAGS_NOTABLE);
    g.srcMask |= (uint8_t)(1u << srcTypeOf(srcTag(d)));
  }
  // Whitelist check per device (before the sort, which uses it): union of member sources,
  // first company ID / service UUID.
  static const uint8_t zeroSvc[16] = {0};
  for (int k = 0; k < n; k++) {
    DevGroup& g = g_groups[k];
    uint8_t bits = 0; uint16_t cid = 0; const uint8_t* svc = zeroSvc;
    for (int r = 0; r < g_detCount; r++) {
      const Detection& d = g_dets[g_sortIdx[r]];
      if (memcmp(d.mac, g.mac, 6) != 0) continue;
      bits |= (uint8_t)(1u << d.source);
      if (!cid) cid = d.companyId;
      if (svc == zeroSvc) for (int j = 0; j < 16; j++) if (d.svc[j]) { svc = d.svc; break; }
    }
    g.whitelisted = whitelist::match(g.mac, g.nameIdx >= 0 ? g_dets[g.nameIdx].name : nullptr,
                                     cid, svc, bits);
    g.companyId = cid;  // first company ID across members (for the detail screen + web maker label)
  }
  std::sort(g_groups, g_groups + n, [](const DevGroup& a, const DevGroup& b) {
    if (a.whitelisted != b.whitelisted) return !a.whitelisted;  // muted devices sink
    if (a.tier != b.tier) return a.tier > b.tier;
    return a.bestRssi > b.bestRssi;
  });
  // Per-group source strings, built once per cycle: distinct tags only (exact compare),
  // strongest first. tag = "2.4+PRB" for the screen, srcs = "2.4:-41,PRB:-55" for the phone.
  for (int k = 0; k < n; k++) {
    DevGroup& g = g_groups[k];
    g.tag[0] = 0; g.srcs[0] = 0;
    const char* seen[4]; int ns = 0;
    size_t tl = 0, sl = 0;
    for (int r = 0; r < g_detCount && ns < 4; r++) {
      const Detection& d = g_dets[g_sortIdx[r]];
      if (memcmp(d.mac, g.mac, 6) != 0) continue;
      const char* t = srcTag(d);
      bool dup = false;
      for (int s = 0; s < ns; s++) if (strcmp(seen[s], t) == 0) { dup = true; break; }
      if (dup) continue;
      seen[ns++] = t;
      int w = snprintf(g.tag + tl, sizeof(g.tag) - tl, tl ? "+%s" : "%s", t);
      if (w > 0 && (size_t)w < sizeof(g.tag) - tl) tl += w; else g.tag[tl] = 0;
      w = snprintf(g.srcs + sl, sizeof(g.srcs) - sl, sl ? ",%s:%d" : "%s:%d", t, d.rssi);
      if (w > 0 && (size_t)w < sizeof(g.srcs) - sl) sl += w; else g.srcs[sl] = 0;
    }
  }
  g_groupCount = n;
  return n;
}

// Category of a whole device (one MAC = one DevGroup). Behavioral flags (unioned across the
// device's members in g.flags) win; otherwise the category of its strongest-scoring member.
Category groupCategory(const DevGroup& g) {
  Category c;
  if      (g.flags & FLAG_BLE_ODID)                              c = CAT_DRONE;
  else if (g.flags & FLAG_BLE_FINDMY)                            c = CAT_TRACKER;
  else if (g.flags & (FLAG_WIFI_PWNAGOTCHI | FLAG_WIFI_EVILTWIN)) c = CAT_DEAUTH;
  else {
    int best = -1;
    for (int i = 0; i < g_detCount; i++)
      if (memcmp(g_dets[i].mac, g.mac, 6) == 0 && (best < 0 || g_score[i].score > g_score[best].score)) best = i;
    c = best >= 0 ? (Category)g_category[best] : CAT_OTHER;
  }
  // AirTag/Tile/SmartTag trackers are everywhere in daily life; counting every one as a threat
  // buries the real signal. Treat a tracker as the TRACKER threat category only when it is actually
  // FOLLOWING (ftier 2 = tracked across >=FOLLOW_SPAN_M of the GPS route); otherwise it drops out of
  // the HOME readout, the drill-down list, and the phone breakdown (all derive from this function).
  if (c == CAT_TRACKER && !isFollowing(g.mac)) return CAT_NONE;
  return c;
}

// Per-category live threat tally over the device groups. Counts only non-whitelisted devices at
// tier >= Suspect (same alarm basis as countTiers); records the worst tier seen per category so
// the readout can colour each chip. Returns the total threat devices.
int countCategories(int cnt[CAT_N], int worstTier[CAT_N], int scoreMax[CAT_N]) {
  for (int c = 0; c < CAT_N; c++) { cnt[c] = 0; worstTier[c] = 0; scoreMax[c] = 0; }
  int total = 0;
  for (int k = 0; k < g_groupCount; k++) {
    const DevGroup& g = g_groups[k];
    if (g.whitelisted || g.tier < (int)sigdb::Tier::Suspect) continue;
    Category c = groupCategory(g);
    cnt[c]++;
    if (g.tier > worstTier[c]) worstTier[c] = g.tier;
    if (g.score > scoreMax[c]) scoreMax[c] = g.score;
    total++;
  }
  return total;
}

int countMuted() {
  int m = 0;
  for (int k = 0; k < g_groupCount; k++) if (g_groups[k].whitelisted) m++;
  return m;
}

FollowState g_follow[MAX_DET];
int         g_followCount = 0;
int         g_followN     = 0;   // devices currently FOLLOWING

void resetFollow() {
  memset(g_follow, 0, sizeof(g_follow));
  g_followCount = 0; g_followN = 0;
}

const FollowState* findFollow(const uint8_t* mac) {
  for (int i = 0; i < g_followCount; i++)
    if (memcmp(g_follow[i].mac, mac, 6) == 0) return &g_follow[i];
  return nullptr;
}

// True only when this MAC is currently FOLLOWING (ftier 2). Used to gate the TRACKER category so
// stationary/passing AirTags don't alarm — only a tracker tailing the user's route does.
bool isFollowing(const uint8_t* mac) {
  const FollowState* f = findFollow(mac);
  return f && f->ftier == 2;
}

void updateFollowState() {
  uint32_t now = millis();  // millis() wrap (~49 d) ignored, as elsewhere for a handheld session
  // Expire stale entries (compact in place).
  int w = 0;
  for (int i = 0; i < g_followCount; i++)
    if (now - g_follow[i].lastMs <= FOLLOW_TTL_MS) { if (w != i) g_follow[w] = g_follow[i]; w++; }
  g_followCount = w;

  uint32_t nowWin = now / FOLLOW_WINDOW_MS;
  bool  gps = posHasFix();
  float lat = 0, lon = 0, cosLat = 1.0f;
  int32_t qLat = 0, qLon = 0;
  if (gps) {
    lat = posLat(); lon = posLon();
    cosLat = cosf(lat * (float)M_PI / 180.0f);  // once per cycle; lat barely changes
    qLat = (int32_t)lroundf(lat * FOLLOW_QUANT);
    qLon = (int32_t)lroundf(lon * FOLLOW_QUANT);
  }

  for (int k = 0; k < g_groupCount; k++) {
    const DevGroup& g = g_groups[k];
    FollowState* f = nullptr;
    for (int i = 0; i < g_followCount; i++)
      if (memcmp(g_follow[i].mac, g.mac, 6) == 0) { f = &g_follow[i]; break; }
    if (!f) {
      if (g_followCount >= MAX_DET) {  // full: evict oldest. Routine in busy RF (5 min TTL vs a ~2 s rebuild), so tracking caps at MAX_DET devices
        int o = 0;
        for (int i = 1; i < g_followCount; i++)
          if (now - g_follow[i].lastMs > now - g_follow[o].lastMs) o = i;
        f = &g_follow[o];
      } else f = &g_follow[g_followCount++];
      memset(f, 0, sizeof(*f));
      memcpy(f->mac, g.mac, 6);
      f->firstMs = now; f->baseWin = (uint8_t)nowWin;
      f->rssiMin = 127; f->rssiMax = -128;
      f->anchorLat = f->anchorLon = NAN;
      f->flags = FF_ACTIVE;
    }
    if (f->sightings < 0xFFFF) f->sightings++;
    f->lastMs = now;
    // Roll the presence mask forward so bit0 = the current window.
    uint8_t delta = (uint8_t)((uint8_t)nowWin - f->baseWin);
    if (delta) { f->windowMask = delta >= 32 ? 0 : (f->windowMask << delta); f->baseWin = (uint8_t)nowWin; }
    f->windowMask |= 1u;
    int r = constrain(g.bestRssi, -127, 127);
    if (r < f->rssiMin) f->rssiMin = (int8_t)r;
    if (r > f->rssiMax) f->rssiMax = (int8_t)r;
    if (gps) {
      if (isnan(f->anchorLat)) {
        f->anchorLat = lat; f->anchorLon = lon;
        f->lastQLat = qLat; f->lastQLon = qLon; f->distinctFixes = 1;
      } else {
        // Equirectangular: dx = dLon*cos(lat), dy = dLat, 1 deg ~ 111320 m.
        float dx = (lon - f->anchorLon) * cosLat * 111320.0f;
        float dy = (lat - f->anchorLat) * 111320.0f;
        float d  = sqrtf(dx * dx + dy * dy);
        if (d > f->maxSpanM) f->maxSpanM = d;
        if ((qLat != f->lastQLat || qLon != f->lastQLon) && f->distinctFixes < 255) {
          f->distinctFixes++; f->lastQLat = qLat; f->lastQLon = qLon;
        }
      }
      f->flags |= FF_HADGPS;
    }
  }
}


void computeFollowScores() {
  g_followN = 0;
  for (int i = 0; i < g_followCount; i++) {
    FollowState& f = g_follow[i];
    int pers = __builtin_popcount(f.windowMask);
    bool gpsOk = (f.flags & FF_HADGPS) && !isnan(f.anchorLat);
    float span = gpsOk ? f.maxSpanM : 0;
    int fixes  = gpsOk ? f.distinctFixes : 0;
    int sc = FOLLOW_W_PERSIST * ((pers >= FOLLOW_PERSIST_B1) + (pers >= FOLLOW_PERSIST_B2) + (pers >= FOLLOW_PERSIST_B3))
           + FOLLOW_W_SPAN    * ((span >= FOLLOW_SPAN_B1) + (span >= FOLLOW_SPAN_B2) + (span >= FOLLOW_SPAN_B3))
           + FOLLOW_W_FIXES   * ((fixes >= FOLLOW_FIXES_B1) + (fixes >= FOLLOW_FIXES_B2) + (fixes >= FOLLOW_FIXES_B3));
    if (span >= FOLLOW_SPAN_MIN && (f.rssiMax - f.rssiMin) <= FOLLOW_RSSI_SPREAD) sc += FOLLOW_W_STABLE;
    // Look up this MAC's group: threat boost (sigdb tier above None), whitelist state, and the
    // data the RPA false-positive guard needs (source + flags + tier).
    bool whitelisted = false;
    uint8_t  grpSrcMask = 0, grpTier = (uint8_t)sigdb::Tier::None;
    uint16_t grpFlags = 0;
    for (int k = 0; k < g_groupCount; k++)
      if (memcmp(g_groups[k].mac, f.mac, 6) == 0) {
        if (g_groups[k].tier != (int)sigdb::Tier::None) sc += FOLLOW_W_THREAT;
        whitelisted = g_groups[k].whitelisted;
        grpSrcMask = g_groups[k].srcMask; grpFlags = g_groups[k].flags; grpTier = (uint8_t)g_groups[k].tier;
        break;
      }
    // Whitelisted (known) devices never raise the follow alert or the magenta LED.
    if (whitelisted) { f.score = 0; f.ftier = 0; continue; }
    f.score = (uint8_t)min(sc, 100);
    // Follow false-positive guard: a benign BLE RPA (privacy-rotating phone/wearable with no threat
    // signature and no notable flag) never escalates to FOLLOWING — it can only reach PERSISTENT.
    bool benignRpa = (grpSrcMask & MASK_BLE) && macIsBleRpa(f.mac) &&
                     grpTier == (uint8_t)sigdb::Tier::None && (grpFlags & FLAGS_NOTABLE) == 0;
    // PERSISTENT is time-only and capped: only GPS movement can reach FOLLOWING.
    if (gpsOk && span >= FOLLOW_SPAN_M && fixes >= FOLLOW_FIXES && pers >= FOLLOW_PERSIST_FOLLOW && !benignRpa) f.ftier = 2;
    else if (pers >= FOLLOW_PERSIST_ONLY) f.ftier = 1;
    else f.ftier = 0;
    if (f.ftier == 2) g_followN++;
  }
}

void countTiers(int& susp, int& lk, int& conf) {
  susp = lk = conf = 0;
  for (int i = 0; i < g_detCount; i++) {
    bool muted = false;  // known (whitelisted) devices don't count toward the alarm totals
    for (int k = 0; k < g_groupCount; k++)
      if (memcmp(g_groups[k].mac, g_dets[i].mac, 6) == 0) { muted = g_groups[k].whitelisted; break; }
    if (muted) continue;
    switch (g_score[i].tier) {
      case sigdb::Tier::Suspect:   susp++; break;
      case sigdb::Tier::Likely:    lk++;   break;
      case sigdb::Tier::Confirmed: conf++; break;
      default: break;
    }
  }
}

// This device's own BLE MAC, cached at boot so we can drop our own advertisement
// from the scan results (the CYD's phone-link peripheral is visible to the C5's BLE
// scan). Captured at runtime -> device-agnostic, works on any CYD unit.
uint8_t g_ownMac[6]   = {0};
bool    g_haveOwnMac  = false;

bool isSelfDet(const Detection& d) {
  return g_haveOwnMac && d.source == (uint8_t)Source::BleScan &&
         memcmp(d.mac, g_ownMac, 6) == 0;
}

// Bag-safe touch lock (also settable from the web app via CMD "K1"/"K0"); declared here so pushStatus
// can report it. The padlock glyph + 2-tap gesture live further down (drawLockGlyph/serviceTouchLock).
bool g_touchLocked = false;

// Scan-cycle LED (works headless). Event alert (deauth / evil twin) blinks red each cycle, alternating
// with magenta when a follow alert is also up, else with the link colour; follow alone = solid
// magenta; otherwise link green/red.
void applyScanLed() {
  static bool phase = false;
  phase = !phase;
  if (eventAlertActive()) {
    if (phase) hostboard::setScanLed(true, false, false);
    else if (followAlertN() > 0) hostboard::setScanLed(true, false, true);
    else hostboard::setScanLed(!g_linkOk, g_linkOk, false);
  } else if (followAlertN() > 0) hostboard::setScanLed(true, false, true);
  else hostboard::setScanLed(!g_linkOk, g_linkOk, false);
}

// One scan → score → log → render cycle, also mirrored to the phone app. The board's UI work is done
// through the hostboard:: / hostui:: hooks (CYD repaints; the headless host does nothing).
ScanCycleResult core::runScanCycle() {
  ScanCycleResult res{};
  hostboard::sample();  // refresh the battery reading every cadence (independent of the C5 link)
  if (!requestScan()) {
    // BUSY MISS: the C5 didn't answer (it's in its Wi-Fi-scan busy window). This is not a link
    // failure. Keep the last table/view (requestScan zeroed g_detCount but we skip the pipeline, so
    // g_groups/g_scanView and the on-screen list stay as they were), don't push the empty table to
    // the phone, and keep the link dot/LED green until LINK_STICKY_MS of total silence. Re-poll soon
    // so we catch the C5 the moment it frees up, instead of waiting the full cadence.
    g_linkOk = (millis() - g_lastLinkOkMs < LINK_STICKY_MS);
    if (!g_linkOk) { g_deauthRecent = 0; g_evilCount = 0; g_ridCount = 0; }  // no stale C5 events
    applyScanLed();
    hostui::onBusyMiss();  // link dot + clock only (never touches the list region); headless in background
    g_lastCycleMs = millis() - (SCAN_CYCLE_MS - SCAN_RETRY_MS);  // fire the next poll in ~SCAN_RETRY_MS
    res.busyMiss = true;
    return res;
  }
  g_linkOk = true; g_lastLinkOkMs = millis();
  computeScores();
  buildGroups();  // once per cycle; shared by the board's view and pushDetections()
  updateFollowState();
  computeFollowScores();
  updateEvents();   // deauth / evil-twin alert hold + rate-limited event log lines
  applyScanLed();   // alert / follow / link LED (works headless)
  if (bgCtx() && g_bg.seenCount >= MAX_SEEN - 32) {
    // The bg stream never ends on its own, and once its dedup set is full new devices would be
    // re-logged every cycle: roll to a fresh bg session (also a cap-rotation trigger).
    beginSessionNamed(g_bg, true);
    enforceLogCap();
  }
  int newCount = logNewDetections();
  logRidEvents();   // rate-limited per-MAC Remote-ID event lines
  logFollowEvents(); // rate-limited per-MAC FOLLOWING event lines
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  Serial.printf("[CYD] table=%d (2.4:%d 5G:%d BLE:%d PRB:%d 154:%d) new=%d uniq=%d threats S%d/L%d/C%d db=%d\n",
                g_detCount, n24, n5, nble, nprb, n154, newCount, activeStream().seenCount,
                susp, lk, conf, sigdb::loaded());
  res.newCount = newCount;
  res.n24 = n24; res.n5 = n5; res.nble = nble; res.nprb = nprb; res.n154 = n154;
  res.susp = susp; res.lk = lk; res.conf = conf;
  pushStatus();
  pushDetections();
  hostui::onScanCycle(res);  // CYD: rebuild the scan view + repaint if on the scanner screen
  return res;
}

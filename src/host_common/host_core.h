// host_core.h — board-agnostic host core: detection table, scoring, categories, device grouping,
// follow-me engine, position/time sources, C5 events, and the scan-poll pipeline. Carved out of the
// CYD main.cpp (docs/mini-build.md §4.1) so the CYD and the headless XIAO-S3 share one copy.
// Everything here is UI-free: boards supply LED/battery/policy via host_board.h and draw via host_ui.h.
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "link_protocol.h"
#include "sigdb.h"

using namespace link_protocol;

constexpr int MAX_DET = 96;

// --- "Following me" detector (Phase 5, device-only). Tunables; these become NVS-settable in a
// later increment. A device is FOLLOWING when it keeps showing up (persistence) while the
// phone's GPS says we moved far from where it was first seen (span) across several distinct
// places (fixes). Without GPS movement a device can only reach PERSISTENT. ---
#define FOLLOW_WINDOW_MS     15000UL   // presence-history bucket (32-bit mask -> ~8 min)
#define FOLLOW_TTL_MS        300000UL  // drop a device unseen this long (> the C5's 30 s TTL)
#define FOLLOW_SPAN_M        400.0f    // min GPS span (m) over which a device must track to FOLLOW
#define FOLLOW_FIXES         3         // ...across at least this many distinct GPS fixes
#define FOLLOW_PERSIST_FOLLOW 4        // ...and >= this many present windows (popcount)
#define FOLLOW_PERSIST_ONLY  6         // PERSISTENT (time only, no GPS needed): present windows
#define FOLLOW_PERSIST_B1    3         // persistence buckets (windows present)
#define FOLLOW_PERSIST_B2    6
#define FOLLOW_PERSIST_B3    12
#define FOLLOW_SPAN_B1       150.0f    // span buckets (metres)
#define FOLLOW_SPAN_B2       400.0f
#define FOLLOW_SPAN_B3       1000.0f
#define FOLLOW_FIXES_B1      2         // distinct-fix buckets
#define FOLLOW_FIXES_B2      4
#define FOLLOW_FIXES_B3      8
#define FOLLOW_RSSI_SPREAD   12        // max-min RSSI (dB) counted as "stable" ...
#define FOLLOW_SPAN_MIN      150.0f    // min span (m) before the RSSI-stability bonus applies
#define FOLLOW_W_PERSIST     10        // score weight per persistence bucket (max 30)
#define FOLLOW_W_SPAN        12        // per span bucket (max 36)
#define FOLLOW_W_FIXES       6         // per fixes bucket (max 18)
#define FOLLOW_W_STABLE      16        // RSSI-stability bonus
#define FOLLOW_W_THREAT      10        // bonus when the sigdb tier is above None
#define FOLLOW_QUANT         1e4f      // fix quantum: 1e-4 deg (~11 m)

// --- detection table (rebuilt from each C5 scan burst by requestScan()) ---
extern Detection          g_dets[MAX_DET];
extern sigdb::ScoreResult g_score[MAX_DET];  // aligned with g_dets
extern int                g_detCount;
constexpr int MAX_RID = 8;
extern RemoteId           g_rids[MAX_RID];
extern int                g_ridStoreCount;
// "Unknown" sentinels in RemoteId fields (match the C5 decoder's values; tested by VALUE, not `have`).
constexpr int32_t  RID_LL_UNK  = 0;      // lat/lon 0 = unknown
constexpr int16_t  RID_ALT_UNK = -1000;  // height / geodetic altitude -1000 = unknown
constexpr uint16_t RID_HDG_UNK = 361;    // heading 361 = unknown
constexpr size_t   LOG_ID_ESC  = 128;    // escaped uas/operator ID buffer (20 chars x up to 6x escape + NUL)
const RemoteId* ridByMac(const uint8_t* mac);
extern bool               g_linkOk;

const char* srcTag(const Detection& d);
uint8_t     srcTypeOf(const char* tag);   // 0=2.4, 1=5G, 2=BLE, 3=PRB, 4=154

// --- scan-source enable mask + background-scan flag (NVS-persisted) ---
extern uint8_t g_srcMask;
constexpr uint8_t SRC_24_BITS = MASK_WIFI24 | MASK_PROBE;
void loadSrcMask();
void saveSrcMask();
extern bool g_bgScan;
void loadBgScan();
void saveBgScan();

void countBands(int& n24, int& n5, int& nble, int& nprb, int& n154);

// --- C5 behavioral-detector flags: the notable set (rules/labels live in host_core.cpp) ---
constexpr uint16_t FLAGS_NOTABLE = FLAG_BLE_IBEACON | FLAG_BLE_FINDMY | FLAG_WIFI_PWNAGOTCHI |
                                   FLAG_WIFI_EVILTWIN | FLAG_BLE_ODID;
// Data-frame client bits: carried in DevGroup.flags (DETS / live-POST "data"/"dl") but NOT notable
// (no tier floor, no follow-guard exemption).
constexpr uint16_t FLAGS_DATA = FLAG_WIFI_DATA_CLIENT | FLAG_WIFI_DATA_UPLINK;

// --- Threat categories. A SquachWatch-style grouping of each device's sigdb label + behavioral
// flags into a small fixed taxonomy, used for the HOME-menu category readout + drill-down and
// streamed to the phone (DETS v5) so the web app shows the same breakdown. Derived on the CYD so
// the mapping lives in one place. ---
enum Category : uint8_t {
  CAT_NONE = 0, CAT_FLOCK, CAT_AXON, CAT_ALPR, CAT_CAM, CAT_RING, CAT_RAVEN,
  CAT_GLASS, CAT_TRACKER, CAT_DRONE, CAT_DEAUTH, CAT_FLIPPER, CAT_SKIM, CAT_OTHER, CAT_N
};
extern const char* const kCatName[CAT_N];
extern uint8_t g_category[MAX_DET];  // aligned with g_dets; filled by computeScores()
Category categoryOf(int i);
void flagTokens(uint16_t flags, char* out, size_t cap);
const char* detLabel(int i);
void computeScores();

// --- Aggregate C5 events ---
#define DEAUTH_ALERT      20        // deauth/disassoc frames in the C5's last ~1 s that raise the alert
#define EVENT_HOLD_MS     6000UL    // keep an alert up this long after the last trigger
#define EVENT_LOG_GAP_MS  30000UL   // log at most one event line per type per this interval
extern uint16_t g_deauthRecent;
extern uint8_t  g_evilCount, g_ridCount;
extern uint16_t g_deauthShown;   // values captured when the alert was last raised (banner text)
extern uint8_t  g_evilShown;
extern uint32_t g_deauthUntil, g_evilUntil;
bool deauthAlertActive();
bool evilAlertActive();
bool eventAlertActive();
void updateEvents();

// --- Unified position + wall-clock (phone preferred, onboard C5 GPS fallback) ---
extern bool     g_onbValid;
extern float    g_onbLat, g_onbLon;
extern uint32_t g_onbAtMs;       // millis() of the last Status carrying an onboard fix (0 = never)
extern uint8_t  g_onbSats;
extern uint32_t g_gpsEpochUtc;   // last UTC epoch from Status.gpsEpoch (0 = never)
extern uint32_t g_gpsEpochAtMs;  // millis() when it was latched
bool     posLive();              // a live fix right now (fresh phone, else live onboard)
bool     posHasFix();            // any position: live, or the held last-known fix
float    posLat();
float    posLon();
int      posSource();            // 0 none / 1 phone / 2 onboard / 3 held (last known, not live)
uint32_t posFixAgeS();           // seconds since the held fix was current; 0 while live (or no fix)
bool     timeHasSync();
int      timeSource();           // 0 none / 1 phone / 2 GPS / 3 RTC
uint32_t epochLocal();
extern uint32_t g_rtcEpochUtc;   // DS3231 UTC latch (0 = none); set by the board at boot
extern uint32_t g_rtcAtMs;
extern void (*g_rtcWrite)(uint32_t utc);  // set by a board that has an RTC (null = none)
void disciplineRtc();           // write phone/GPS UTC back to the RTC (rate-limited; no-op without an RTC)

// --- Device grouping ---
int buildSorted(int* idx);
// One device = one MAC, merged across sources (2.4 / 5G / BLE / PRB). Built from the
// sorted detections; ordered tier-first then strongest RSSI. Shared by the on-screen
// list and the phone DETS stream.
struct DevGroup {
  uint8_t  mac[6];
  int      tier;      // max tier across members
  int      score;     // max member sigdb score (0-100); drives the HOME tile threat-score badge
  int      bestRssi;  // strongest member RSSI
  int      rep;       // g_dets index of the strongest member (colour/source hint)
  int      nameIdx;   // g_dets index of the first member with a name, else -1
  uint32_t ie;        // first non-zero IE fingerprint, else 0
  uint16_t companyId; // first non-zero BLE manufacturer company ID across members, else 0 (display/web)
  bool     whitelisted;  // matches a /whitelist.csv rule (muted; excluded from threat counts)
  char     tag[12];   // combined distinct source tag, e.g. "2.4+PRB"
  char     srcs[48];  // "tag:rssi,tag:rssi" list for the phone stream
  uint16_t flags;     // union of the notable C5 detector flags (FLAGS_NOTABLE) across members
  uint8_t  srcMask;   // union of srcType bits (2.4/5G/BLE/PRB/154) across members; computed at
                      // build time so the live-view/snapshot filter never depends on the transient
                      // g_dets table (which requestScan() zeroes, and an aborted scan leaves empty)
};
extern int      g_sortIdx[MAX_DET];
extern DevGroup g_groups[MAX_DET];
extern int      g_groupCount;
const char* groupName(const DevGroup* g);
int  buildGroups();
Category groupCategory(const DevGroup& g);
int  countCategories(int cnt[CAT_N], int worstTier[CAT_N], int scoreMax[CAT_N]);
int  countMuted();
void countTiers(int& susp, int& lk, int& conf);

// --- Scan state + poll cadence ---
extern bool     g_scanActive;   // C5 polling on (START/STOP); independent of any screen
extern uint32_t g_lastCycleMs;
// Scan-poll cadence + link stickiness. The C5 is a continuous scanner that cannot service the link
// while its Wi-Fi scan phase runs (a multi-second busy window), so a poll landing in that window
// gets no reply. That is a BUSY MISS, not a link failure: we keep the last table/view, don't flash
// the link dot, and retry soon instead of after the full cadence. The link only counts as DOWN once
// no successful exchange has happened for LINK_STICKY_MS (covers the longest observed busy window).
static constexpr uint32_t SCAN_CYCLE_MS  = 2000;   // normal poll spacing
static constexpr uint32_t SCAN_RETRY_MS  = 400;    // quick re-poll after a busy miss (catch the C5 free)
static constexpr uint32_t SCAN_CYCLE_DOWN_MS = 5000; // headless bg-scan poll spacing when the C5 link is down
                                                     // (each poll costs ~800ms of no-reply wait; back off so menus stay snappy)
static constexpr uint32_t LINK_STICKY_MS = 8000;   // treat the link as up this long after the last reply
extern uint32_t g_lastLinkOkMs;      // millis() of the last successful scan exchange / ping

// --- Following-me engine ---
struct FollowState {
  uint8_t  mac[6];
  uint16_t sightings;
  uint32_t firstMs, lastMs;
  uint32_t windowMask;      // 1 bit per FOLLOW_WINDOW_MS, bit0 = window baseWin (newest)
  uint8_t  baseWin;         // window index (mod 256) of bit0
  int8_t   rssiMin, rssiMax;
  float    anchorLat, anchorLon;  // first GPS fix seen at (NAN until a fix)
  float    maxSpanM;        // farthest anchor->current distance seen
  int32_t  lastQLat, lastQLon;    // last counted quantized fix
  uint8_t  distinctFixes;
  uint8_t  score;           // 0..100
  uint8_t  ftier;           // 0 none / 1 PERSISTENT / 2 FOLLOWING
  uint8_t  flags;           // FF_ACTIVE | FF_HADGPS
};
static constexpr uint8_t FF_ACTIVE = 1, FF_HADGPS = 2;
extern FollowState g_follow[MAX_DET];
extern int         g_followCount;
extern int         g_followN;   // devices currently FOLLOWING
// Follower-ALERT feature flag (temporarily off). The follow DETECTION keeps running in full —
// updateFollowState()/computeFollowScores() still set each device's ftier/score, so isFollowing()
// and the follow-gated TRACKER category behave exactly as before. This flag only suppresses the
// user-facing follower SURFACES: the count, the magenta banners (scan + HOME), the magenta LED,
// STATUS fol, the evt:follow log line, the DETS follow fields, and the drill-down. Flip to true to
// restore the full follower UX unchanged. Disabled for now because the follow metric (anchor-radial
// GPS span in computeFollowScores) over-fires on fixed infrastructure passed while driving — a
// co-travel rework is planned before re-enabling. [[phase5-followme-direction]]
static constexpr bool FOLLOW_ALERTS_ENABLED = false;
// Follower count as surfaced to UI / STATUS / logging — 0 while the alerts feature is disabled.
static inline int followAlertN() { return FOLLOW_ALERTS_ENABLED ? g_followN : 0; }
void resetFollow();
const FollowState* findFollow(const uint8_t* mac);
bool isFollowing(const uint8_t* mac);
void updateFollowState();
// BLE Resolvable Private Address: the most-significant address byte's top two bits are 0b01. RPAs
// are privacy-rotating addresses used by phones, watches and other personal gear. A device we're
// "tracking" by a fixed RPA is almost always our own/companion phone within a single rotation
// window (the classic follow false-positive, and the one the own-phone peerMac filter misses
// because a phone's GATT address differs from its advertised RPA) — and it can't be tracked past
// the next rotation anyway. A genuinely planted AirTag/tracker surfaces through its Find-My/tracker
// signature (a notable flag or a sigdb tier), which exempts it from this guard below.
static inline bool macIsBleRpa(const uint8_t* mac) { return (mac[0] & 0xC0) == 0x40; }
void computeFollowScores();

// --- This device's own BLE MAC (dropped from scan results) + bag-safe lock flag ---
extern uint8_t g_ownMac[6];
extern bool    g_haveOwnMac;
bool isSelfDet(const Detection& d);
extern bool g_touchLocked;

// --- Scan-cycle LED + the scan-cycle pipeline (docs/mini-build.md §4.4) ---
void applyScanLed();

struct ScanCycleResult {
  bool busyMiss;            // the C5 did not answer (its Wi-Fi-scan busy window) -- table kept, no pipeline run
  int  newCount;            // first-seen detections logged this cycle
  int  n24, n5, nble, nprb, n154;
  int  susp, lk, conf;
};
namespace core {
// One scan -> score -> log -> push cycle. Calls the hostboard:: / hostui:: hooks for LED + UI.
ScanCycleResult runScanCycle();
}

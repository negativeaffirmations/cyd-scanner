// host_log.h — SD session logging (NDJSON), the fg/bg log streams, session lifecycle, phone log
// transfers and the /logs/ cap. Carved from the CYD main.cpp (docs/mini-build.md §4.1).
#pragma once
#include <Arduino.h>
#include "host_core.h"

constexpr int MAX_SEEN = 400;
static constexpr size_t LOG_LINE = 512;  // max log line (NDJSON lines are ~100-330 B; worst case < 512)

extern bool g_sdOk;

// One entry of a stream's recent-device table. lastSeen drives LRU eviction (the table never fills up
// and stops logging); lastLogged drives the RELOG_MS re-sighting rows. firstSeen / rssiMax / present
// track the CURRENT presence period for the {"evt":"lastseen"} departure line (see DEPART_MS).
// Packed: 24 B (x MAX_SEEN x 2 streams on the no-PSRAM CYD).
struct __attribute__((packed)) SeenKey {
  uint32_t lastSeen, lastLogged, firstSeen;  // millis()
  uint16_t pan;
  uint8_t  source, mac[6];
  int8_t   rssiMax;   // strongest RSSI this presence period
  uint8_t  srcType;   // srcTypeOf(srcTag()) at first sighting: 0=2.4 1=5G 2=BLE 3=PRB 4=154
  uint8_t  present;   // 1 = in a presence period (no lastseen line written for it yet)
};

// Departure line: once a present device has not been heard for DEPART_MS (or is evicted from the LRU,
// or the session ends via New Session) one line is written for that presence period:
//   {"epoch":..,"ms":..,["lat":..,"lon":..,]"evt":"lastseen","src":"..","mac":"..",["pan":".."],
//    "first_ms":..,"last_ms":..,"rssi_max":N}
// first_ms/last_ms are on the same boot clock as ms (reader: epoch - (ms - x)/1000). last_ms can be up
// to the C5's 30 s table TTL later than the device's last packet. A device that comes back starts a
// new period. File log only (never in the live DETS / live-POST path). Schema owner-approved
// 2026-10-22 with scan-analyzer (rotating-address linking + dwell time).
static constexpr uint32_t DEPART_MS = 60UL * 1000;

// A device already logged this session is logged AGAIN once RELOG_MS has passed since its last row
// while it is still (or again) in range, so a drive / long session gets periodic location points
// for it instead of one row at first sight. Downstream (web app, scan-analyzer) handles many rows
// per MAC.
static constexpr uint32_t RELOG_MS = 5UL * 60 * 1000;

// One log stream = one live NDJSON session file + its own first-seen dedup set. Two streams:
//   g_fg = foreground/manual sessions (the on-screen scan):  /logs/sess-NNNNN.jsonl -> /logs/YYYYMMDD-HHMMSS.jsonl
//   g_bg = headless background scan (not the foreground scan):  /logs/bg-NNNNN.jsonl   -> /logs/bg-YYYYMMDD-HHMMSS.jsonl
// The file is created lazily on the first row; the counter name is renamed to a date-time name once
// the phone syncs the clock (renameSessionOnSync). Each stream dedups independently (MAX_SEEN each).
// One scan = one file: the seen table is an LRU of the MAX_SEEN most recently seen devices, so a
// dense drive never fills it -- the least-recently-seen device is evicted (and simply re-logged if
// it comes back). The file only changes on a new boot or an explicit "New Session".
struct LogStream {
  char     path[48];   // current session file (NDJSON)
  bool     named;      // true once renamed to the date-time form
  bool     started;    // true once the file is actually created (first row)
  SeenKey* seen;       // MAX_SEEN entries, heap-allocated on the first beginSessionNamed() (~9.6 KB per
                       // stream: too big for the CYD's static DRAM); nullptr only if that malloc failed
  int      seenCount;  // occupied table slots (<= MAX_SEEN)
  uint32_t uniqCount;  // distinct devices logged this session (STATUS uniq / U:); a device evicted
                       // from the LRU and seen again later counts again, so it's a close upper bound
  uint32_t rowCount;   // detection rows written this session (first sightings + re-sightings)
};

extern LogStream g_fg;  // foreground/manual (on-screen) scan session
extern LogStream g_bg;  // headless background scan session
extern char g_streamingPath[100];  // file currently being streamed to a phone (BLE); never auto-deleted

// Headless background cycle = scan polling while NOT in the foreground scan (only runs when bg is on).
// "Foreground" is the board's call: hostboard::foregroundScan().
bool bgCtx();
// The stream the current context logs to / "current session" refers to.
LogStream& activeStream();
// Keep the Wi-Fi download server's "current session" pointed at the active stream.
void syncSharePath();

void sanitizeField(char* dst, size_t cap, const char* src);
void jsonEscape(char* dst, size_t cap, const char* src);

// Bump DB_GEN to force a one-time delete of /signatures.csv so sigdb re-seeds it from
// the firmware (e.g. after adding seed rules). This DISCARDS any on-card DB edits, so
// only bump it when that's intended.
inline constexpr uint32_t DB_GEN = 5;  // 1: Phase-3 Flock GATT bleuuid rule; 2: SquachWatch roster expansion;
                                       // 3: Nest/Reolink/SimpliSafe OUIs + Arlo/Blink reweight (2026-10-10);
                                       // 4: 'vendor,' OUI->vendor display rules (2026-10-15);
                                       // 5: expanded 'cidname,' BLE company makers + Garmin ID fix (2026-10-15)
// Human-facing firmware version, distinct from the CYD<->C5 PROTOCOL_VERSION and the sig-DB DB_GEN.
// Emitted in the per-session {"evt":"meta"} provenance anchor and the STATUS characteristic.
inline constexpr char FW_VERSION[] = "0.18.0";  // 0.10.0: LRU never-stop logging + 5-min relog, unit names; 0.11.0: lastseen departure lines; 0.12.0: link v11 bssid + data/dl flags; 0.13.0: phone-GPS staleness + held fix "fix_age"; 0.14.0: phone fix preferred only while < 10 s old, fix_age on any fix > 10 s, STATUS gav/tav; 0.15.0: time/GPS source prefs (P: cmd), device dot = GPS-time-fresh or RTC; 0.16.0: STATUS pos/pfa (live-POST rows use the scanner position); 0.16.1: review fixes (one position snapshot per use, race-safe fix ages, fix_age > 10 s in ms, one device-clock order); 0.16.2: CYD GPS modal refreshes live while open; 0.17.0: status-LED brightness (I: cmd, STATUS led=); 0.18.0: web SD file explorer (Z:/U cmds, F: any path)

const char* scannerIdHex();
// Human-friendly unit name, e.g. "CFLO-MINI-KOBALI": board tag + a pronounceable 6-letter word
// derived from the factory eFuse MAC, so it is unique per unit and stable across reflashes / NVS
// wipes. Used as the BLE advertised name (phone picker), STATUS name=, the CYD screens, and the
// session meta line's scanner_name.
const char* scannerName();
int sessionMetaLine(char* out, size_t cap);
// Every session file written THIS boot: the live fg/bg files plus earlier ones this boot (New Session,
// bg toggles). Their ms-since-boot is still valid, so a time-window export can date them. Returns n.
int thisBootFiles(const char** out, int cap);  // one {"evt":"meta",...}\n line; returns its length (0 = no room)

void beginSessionNamed(LogStream& st, bool bg);
void openSession();
void openBgSession();
void startNewSession(bool forceFg = false);
bool ensureLogFile(LogStream& st);
void renameSessionOnSync();
void initSD();
int  logNewDetections();
void logEvent(const char* evt, int count);
// {"evt":"phonecfg"} audit line: the phone changed card files / whitelist / DB, or the key was rotated.
void logPhoneCfg(const char* op, const char* path = nullptr, const char* to = nullptr);
void logRidEvents();
void logFollowEvents();
void transferLog();
void transferFiltered(int mode, int minutes, const char* pathIn);
void transferFile(const char* path);
void deleteSession(const char* path);
bool logPathProtected(const char* full);  // live fg/bg file, Explore-held, or being streamed: never delete/overwrite
void enforceLogCap();
void sendSessionList();
void sendWhitelist();
void sessBase(const LogStream& st, char* out, size_t cap);

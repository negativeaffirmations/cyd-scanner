// host_log.h — SD session logging (NDJSON), the fg/bg log streams, session lifecycle, phone log
// transfers and the /logs/ cap. Carved from the CYD main.cpp (docs/mini-build.md §4.1).
#pragma once
#include <Arduino.h>
#include "host_core.h"

constexpr int MAX_SEEN = 400;
static constexpr size_t LOG_LINE = 512;  // max log line (NDJSON lines are ~100-330 B; worst case < 512)

extern bool g_sdOk;

// One entry of a stream's recent-device table. lastSeen drives LRU eviction (the table never fills up
// and stops logging); lastLogged drives the RELOG_MS re-sighting rows.
// Packed: 17 B instead of 20 (x MAX_SEEN x 2 streams on the no-PSRAM CYD).
struct __attribute__((packed)) SeenKey { uint32_t lastSeen; uint32_t lastLogged; uint16_t pan; uint8_t source; uint8_t mac[6]; };

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
  SeenKey  seen[MAX_SEEN];
  int      seenCount;  // occupied table slots (<= MAX_SEEN)
  uint32_t uniqCount;  // distinct devices logged this session (STATUS uniq / U:); a device evicted
                       // from the LRU and seen again later counts again, so it's a close upper bound
  uint32_t rowCount;   // detection rows written this session (first sightings + re-sightings)
};

extern LogStream g_fg;  // foreground/manual (on-screen) scan session
extern LogStream g_bg;  // headless background scan session
extern char g_streamingPath[48];  // file currently being streamed to a phone (BLE); never auto-deleted

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
inline constexpr char FW_VERSION[] = "0.9.0";

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
void logRidEvents();
void logFollowEvents();
void transferLog();
void transferFiltered(int mode, int minutes, const char* pathIn);
void transferFile(const char* path);
void deleteSession(const char* path);
void enforceLogCap();
void sendSessionList();
void sendWhitelist();
void sessBase(const LogStream& st, char* out, size_t cap);

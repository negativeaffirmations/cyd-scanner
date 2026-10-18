// host_log.h — SD session logging (NDJSON), the fg/bg log streams, session lifecycle, phone log
// transfers and the /logs/ cap. Carved from the CYD main.cpp (docs/mini-build.md §4.1).
#pragma once
#include <Arduino.h>
#include "host_core.h"

constexpr int MAX_SEEN = 400;
static constexpr size_t LOG_LINE = 512;  // max log line (NDJSON lines are ~100-330 B; worst case < 512)

extern bool g_sdOk;

struct SeenKey { uint8_t source; uint8_t mac[6]; uint16_t pan; };

// One log stream = one live NDJSON session file + its own first-seen dedup set. Two streams:
//   g_fg = foreground/manual sessions (the on-screen scan):  /logs/sess-NNNNN.jsonl -> /logs/YYYYMMDD-HHMMSS.jsonl
//   g_bg = headless background scan (not the foreground scan):  /logs/bg-NNNNN.jsonl   -> /logs/bg-YYYYMMDD-HHMMSS.jsonl
// The file is created lazily on the first row; the counter name is renamed to a date-time name once
// the phone syncs the clock (renameSessionOnSync). Each stream dedups independently (MAX_SEEN each).
struct LogStream {
  char     path[48];   // current session file (NDJSON)
  bool     named;      // true once renamed to the date-time form
  bool     started;    // true once the file is actually created (first row)
  SeenKey  seen[MAX_SEEN];
  int      seenCount;
  uint32_t lastRotateMs;  // millis() of the last session start (dense-area auto-rotate guard)
};
// When a stream's per-session dedup set (MAX_SEEN) fills, unknown devices can no longer be deduped,
// so logNewDetections() skips them (never re-logs the same MAC every cycle) and rolls the session to
// a fresh file so capture resumes. This caps that auto-rotation to at most once per interval so a
// dense RF drive can't thrash out many tiny files.
static constexpr uint32_t SESS_ROTATE_MIN_MS = 5UL * 60 * 1000;  // >= 5 min between dense-area rotations

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

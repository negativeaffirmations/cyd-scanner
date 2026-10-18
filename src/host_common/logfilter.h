// logfilter.h — time-window filtering of session logs, done ON the CYD so a filtered export never
// streams the whole file. Header-only; shared by the BLE "T:" export (main.cpp) and the Wi-Fi
// /dlf handler (webshare.cpp).
//
// Lines are NDJSON ({"epoch":..,"ms":..,...}) or legacy CSV (epoch,ms,...). The epoch/ms prefix is
// found by a cheap string scan -- no JSON parse. epoch is LOCAL time (0 before the phone synced the
// clock); ms is ms-since-boot, so it is only comparable within the boot that wrote the line.
#pragma once

#include <Arduino.h>
#include <SD.h>

namespace logfilter {

static constexpr size_t LINE_CAP = 600;  // longest usable line (log lines are <= 512 B); longer = skipped

struct Cutoff {
  uint8_t  mode;      // 0 = keep every line; 1/2 = time window
  bool     haveTime;  // the phone has synced wall-clock time (epoch cutoff is valid)
  uint32_t epoch;     // keep lines with epoch >= this (local epoch)
  uint32_t ms;        // keep lines with ms >= this (pre-sync / epoch==0 fallback)
};

// mode 0 = whole file, 1 = past 24 h, 2 = past `minutes`. nowEpoch = phone::epochNow() (0 = unsynced).
inline Cutoff make(int mode, uint32_t minutes, uint32_t nowEpoch, uint32_t nowMs) {
  Cutoff c{};
  c.mode = (mode == 1 || mode == 2) ? (uint8_t)mode : 0;
  if (c.mode == 1) minutes = 1440;
  if (minutes < 1) minutes = 1;
  if (minutes > 525600UL) minutes = 525600UL;  // 1 year
  uint32_t spanS  = minutes * 60UL;
  uint64_t spanMs = (uint64_t)spanS * 1000ULL;
  c.haveTime = nowEpoch != 0;
  c.epoch    = nowEpoch > spanS ? nowEpoch - spanS : 0;
  c.ms       = nowMs > spanMs ? (uint32_t)(nowMs - spanMs) : 0;
  return c;
}

inline uint32_t fieldU32(const char* line, const char* key) {
  const char* p = strstr(line, key);
  return p ? (uint32_t)strtoul(p + strlen(key), nullptr, 10) : 0;
}

// True if `line` (NDJSON or CSV data line) falls inside the window. Lines with no epoch (pre-sync)
// fall back to the ms-since-boot cutoff; non-data lines (a legacy CSV header) are always kept.
inline bool keep(const char* line, const Cutoff& c) {
  if (c.mode == 0) return true;
  uint32_t epoch, ms;
  if (line[0] == '{') {
    epoch = fieldU32(line, "\"epoch\":");
    ms    = fieldU32(line, "\"ms\":");
  } else if (line[0] >= '0' && line[0] <= '9') {
    char* e = nullptr;
    epoch = (uint32_t)strtoul(line, &e, 10);
    ms    = (e && *e == ',') ? (uint32_t)strtoul(e + 1, nullptr, 10) : 0;
  } else {
    return true;
  }
  if (c.haveTime && epoch > 0) return epoch >= c.epoch;
  return ms >= c.ms;
}

// Block-buffered line reader (SD byte reads are slow). next() yields each COMPLETE line that fits
// LINE_CAP, newline included and NUL-terminated; a torn trailing line (power loss) or an overlong
// line is skipped. Both export passes use it, so their byte counts always agree.
class LineReader {
 public:
  explicit LineReader(File& f) : f_(f) {}
  bool next(char* line, size_t& len) {
    size_t o = 0;
    bool over = false;
    for (;;) {
      if (pos_ >= n_) {
        int r = f_.read(blk_, sizeof(blk_));
        if (r <= 0) { n_ = pos_ = 0; return false; }
        n_ = (size_t)r; pos_ = 0;
        if ((++blocks_ & 0x7F) == 0) delay(1);  // yield on very large files
      }
      char ch = (char)blk_[pos_++];
      if (o < LINE_CAP - 1) line[o++] = ch; else over = true;
      if (ch == '\n') {
        if (!over) { line[o] = 0; len = o; return true; }
        o = 0; over = false;
      }
    }
  }
 private:
  File&    f_;
  uint8_t  blk_[512];
  size_t   pos_ = 0, n_ = 0;
  uint32_t blocks_ = 0;
};

}  // namespace logfilter

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

// ---------------------------------------------------------------------------------------------
// Multi-file time-window export: one NDJSON stream holding every row from EVERY /logs/*.jsonl file
// that falls inside the window (a scan spans several files: fg + bg streams, New Session, reboots).
//
// Rows are dated per file. A pre-sync row (epoch:0) is backfilled from its own file's
// {"evt":"timesync"} anchor (epoch = anchor.epoch + (row.ms - anchor.ms)/1000, the same formula the
// web app uses) and REWRITTEN with that epoch. Anchors from different boots can't be mixed in one
// joined file, so dating happens here. A live file (this boot) with no anchor yet uses the current
// clock as its anchor. Rows that still can't be dated (an old file that never saw a clock) are
// dropped, except in a live file when no clock is known at all (then the ms-since-boot cutoff applies).
// Per-file meta + timesync lines are dropped; the caller writes one fresh meta line first.
//
// Output is file by file (oldest file first), not interleaved by time: readers sort if they need to.
// Usage: MultiExport mx(mode, minutes, nowEpoch, nowMs, bootPaths, nBoot); mx.begin();
//        mx.run([](const char* ln, size_t len){ ...; return keepGoing; });   // run() may repeat
class MultiExport {
 public:
  static constexpr int MAX_FILES = 64;  // newest files kept if more match

  // mode 0 = since boot, 1 = past 24 h, 2 = past `minutes`. nowEpoch = local epoch (0 = no clock).
  // boot/nBoot = every session file written THIS boot (live fg/bg + earlier ones): their ms is valid.
  MultiExport(int mode, uint32_t minutes, uint32_t nowEpoch, uint32_t nowMs, const char* const* boot, int nBoot)
      : nowEpoch_(nowEpoch), nowMs_(nowMs), boot_(boot), nBoot_(nBoot) {
    if (mode == 1 || mode == 2) {
      Cutoff c = make(mode, minutes, nowEpoch, nowMs);
      cutEpoch_ = c.epoch; cutMs_ = c.ms;
    } else {  // since boot
      uint32_t up = nowMs / 1000;
      cutEpoch_ = nowEpoch > up ? nowEpoch - up : 0;
      cutMs_ = 0;
    }
  }
  ~MultiExport() { free(files_); }
  MultiExport(const MultiExport&) = delete;             // owns files_
  MultiExport& operator=(const MultiExport&) = delete;

  // Pick the candidate files (cheap: reads each file's head/tail block) and sort them oldest first.
  // Returns the number of files that will be read.
  int begin() {
    free(files_); n_ = 0;
    files_ = (Ent*)malloc(sizeof(Ent) * MAX_FILES);
    if (!files_) return 0;
    File dir = SD.open("/logs");
    if (!dir) return 0;
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      String nm = e.name();
      bool ok = !e.isDirectory() && nm.endsWith(".jsonl");
      Ent en{};
      if (ok) {
        int sl = nm.lastIndexOf('/');
        snprintf(en.path, sizeof(en.path), "/logs/%s", (sl >= 0 ? nm.substring(sl + 1) : nm).c_str());
        en.live = isLive(en.path);
        if (!nowEpoch_ && !en.live) ok = false;  // no clock: only this boot's files can be windowed
      }
      if (ok) {
        uint32_t head = 0;
        bool headZero = false;
        ends(e, head, headZero, en.tail);
        if (!en.live && (en.tail == 0 || en.tail < cutEpoch_)) ok = false;  // undatable, or wholly older
        else if (headZero) en.hasAnc = findAnchor(e, en.ancEpoch, en.ancMs);
        if (ok && en.live && !en.hasAnc && nowEpoch_) { en.hasAnc = true; en.ancEpoch = nowEpoch_; en.ancMs = nowMs_; }
        if (ok && en.tail == 0) en.tail = 0xFFFFFFFFUL;  // undated (this boot, no clock): sorts last
      }
      e.close();
      if (!ok) continue;
      if (n_ < MAX_FILES) { files_[n_++] = en; continue; }
      int lo = 0;  // full: replace the oldest kept file if this one is newer
      for (int i = 1; i < n_; i++) if (files_[i].tail < files_[lo].tail) lo = i;
      if (en.tail > files_[lo].tail) files_[lo] = en;
      dropped_++;
    }
    dir.close();
    for (int i = 1; i < n_; i++)  // insertion sort by tail epoch (oldest first)
      for (int j = i; j > 0 && files_[j].tail < files_[j - 1].tail; j--) { Ent t = files_[j]; files_[j] = files_[j - 1]; files_[j - 1] = t; }
    return n_;
  }
  int dropped() const { return dropped_; }

  // First and last non-zero epoch found in a file's head / tail block (0 = none there). Cheap (two
  // 1 KB reads); used by the session list so the phone can group a scan's files by time.
  static void fileSpan(File& f, uint32_t& first, uint32_t& last) {
    bool hz;
    ends(f, first, hz, last);
    if (hz) {  // head row is pre-sync: take the first dated line in the head block instead
      first = 0;
      char* b = (char*)malloc(1025);
      if (b) {
        f.seek(0);
        int r = f.read((uint8_t*)b, 1024);
        if (r > 0) {
          b[r] = 0;
          for (const char* p = strstr(b, "\"epoch\":"); p && !first; p = strstr(p + 8, "\"epoch\":"))
            if (!epochAfter(p, first)) first = 0;
        }
        free(b);
        f.seek(0);
      }
    }
  }

  // Stream every kept line (newline included) to emit(line, len) -> bool (false = stop). Returns bytes
  // emitted. Deterministic, so a count pass and a send pass produce the same bytes.
  template <class F> uint32_t run(F emit) {
    uint32_t total = 0;
    static char line[LINE_CAP];      // static: off the loop-task stack (callers are single-threaded)
    static char out[LINE_CAP + 16];
    for (int i = 0; i < n_; i++) {
      const Ent& en = files_[i];
      File f = SD.open(en.path, "r");
      if (!f) continue;
      LineReader rd(f);
      size_t len;
      bool go = true;
      while (go && rd.next(line, len)) {
        if (line[0] != '{') continue;
        if (strstr(line, "\"evt\":\"meta\"") || strstr(line, "\"evt\":\"timesync\"")) continue;
        uint32_t ep = fieldU32(line, "\"epoch\":");
        uint32_t ms = fieldU32(line, "\"ms\":");
        const char* src = line;
        size_t slen = len;
        if (ep == 0 && en.hasAnc) {
          int64_t dms = (int64_t)ms - (int64_t)en.ancMs;  // rounded like the web app's backfillEpoch
          int64_t e = (int64_t)en.ancEpoch + (dms + (dms >= 0 ? 500 : -500)) / 1000;
          static const char kZero[] = "{\"epoch\":0,";
          if (e > 0 && strncmp(line, kZero, sizeof(kZero) - 1) == 0) {
            int w = snprintf(out, sizeof(out), "{\"epoch\":%lu,", (unsigned long)e);
            if (w > 0 && (size_t)w + len - (sizeof(kZero) - 1) < sizeof(out)) {
              memcpy(out + w, line + sizeof(kZero) - 1, len - (sizeof(kZero) - 1) + 1);  // + NUL
              src = out; slen = (size_t)w + len - (sizeof(kZero) - 1);
              ep = (uint32_t)e;
            }
          }
        }
        bool keepIt;
        if (ep > 0) keepIt = nowEpoch_ ? ep >= cutEpoch_ : (en.live && ms >= cutMs_);
        else        keepIt = en.live && !nowEpoch_ && ms >= cutMs_;  // undated: only this boot, no clock
        if (!keepIt) continue;
        total += slen;
        go = emit(src, slen);
      }
      f.close();
      if (!go) break;
    }
    return total;
  }

 private:
  struct Ent { char path[48]; uint32_t tail; uint32_t ancEpoch, ancMs; bool hasAnc, live; };

  bool isLive(const char* p) const {
    for (int i = 0; i < nBoot_; i++) if (boot_[i] && boot_[i][0] && !strcmp(p, boot_[i])) return true;
    return false;
  }
  // The epoch value after an "epoch": key, only if the number is complete (a torn last line written
  // during power loss, e.g. {"epoch":17, must not read as epoch 17). Rows always continue with ",".
  static bool epochAfter(const char* p, uint32_t& v) {
    char* e = nullptr;
    v = (uint32_t)strtoul(p + 8, &e, 10);
    return e && e != p + 8 && *e == ',';
  }
  // headZero = the first dated-or-not data line has epoch 0 (pre-sync rows -> needs the anchor);
  // tail = the last non-zero epoch in the file's final block (any line, incl. the timesync anchor).
  static void ends(File& f, uint32_t& head, bool& headZero, uint32_t& tail) {
    static constexpr size_t BLK = 1024;
    char* b = (char*)malloc(BLK + 1);
    head = tail = 0; headZero = false;
    if (!b) return;
    size_t sz = f.size();
    f.seek(0);
    int r = f.read((uint8_t*)b, BLK);
    if (r > 0) {
      b[r] = 0;
      const char* p = strstr(b, "\"epoch\":");
      if (p && epochAfter(p, head)) headZero = head == 0;
      else head = 0;
    }
    f.seek(sz > BLK ? sz - BLK : 0);
    r = f.read((uint8_t*)b, BLK);
    if (r > 0) {
      b[r] = 0;
      for (const char* p = strstr(b, "\"epoch\":"); p; p = strstr(p + 8, "\"epoch\":")) {
        uint32_t v;
        if (epochAfter(p, v) && v) tail = v;
      }
    }
    free(b);
    f.seek(0);
  }
  static bool findAnchor(File& f, uint32_t& ep, uint32_t& ms) {
    f.seek(0);
    char line[LINE_CAP];
    size_t len;
    LineReader rd(f);
    while (rd.next(line, len)) {
      if (!strstr(line, "\"evt\":\"timesync\"")) continue;
      ep = fieldU32(line, "\"epoch\":");
      ms = fieldU32(line, "\"ms\":");
      if (ep) { f.seek(0); return true; }
    }
    f.seek(0);
    return false;
  }

  uint32_t nowEpoch_, nowMs_, cutEpoch_ = 0, cutMs_ = 0;
  const char* const* boot_;
  int nBoot_;
  Ent* files_ = nullptr;
  int n_ = 0, dropped_ = 0;
};

}  // namespace logfilter

// link_protocol.h — shared UART message protocol for the CYD <-> ESP32-C5 link.
//
// Compiled into BOTH firmwares (it lives in lib/, so PlatformIO's LDF pulls it
// into every env that #includes it). Keep the two sides in sync by editing only
// this file — never duplicate these definitions in src/cyd or src/c5.
//
// Roles:
//   CYD  = master. Sends Command frames, consumes Reply frames, drives the UI.
//   C5   = scanner. Consumes Command frames, emits Reply frames (detections).
//
// Wire format (little-endian, byte stream over UART):
//   [FRAME_START][version][type][len_lo][len_hi][payload... len bytes][checksum]
//   checksum = XOR of every byte from `version` through the last payload byte.
//
// This is a starter definition — extend as the firmware grows.

#pragma once
#include <stdint.h>

namespace link_protocol {

// --- Link parameters --------------------------------------------------------
static constexpr uint32_t LINK_BAUD        = 115200;  // UART baud, both sides
static constexpr uint8_t  FRAME_START      = 0xAA;    // frame delimiter
static constexpr uint8_t  PROTOCOL_VERSION = 2;       // v2: Detection gains flags + ie_hash
static constexpr uint16_t MAX_PAYLOAD      = 256;     // sanity cap for RX buffers

// --- CYD -> C5 : commands ---------------------------------------------------
enum class Command : uint8_t {
  Ping      = 0x01,  // health check; C5 replies Pong (no payload)
  StartScan = 0x10,  // begin scanning; payload = ScanConfig
  StopScan  = 0x11,  // stop scanning (no payload)
  GetStatus = 0x20,  // request Status reply (no payload)
};

// --- C5 -> CYD : replies ----------------------------------------------------
enum class Reply : uint8_t {
  Pong      = 0x81,  // no payload
  Heartbeat = 0x82,  // payload = Heartbeat (link connection monitor)
  Detection = 0x90,  // payload = Detection (one per detected device)
  Status    = 0xA0,  // payload = Status
  Error     = 0xEF,  // payload = 1 byte error code
};

// Which radio/band a detection came from.
enum class Source : uint8_t {
  WifiScan   = 0,   // AP/beacon from WiFi.scanNetworks() OR a promiscuous beacon
  BleScan    = 1,
  Ieee802154 = 2,   // Zigbee / Thread
  WifiProbe  = 3,   // Wi-Fi CLIENT probe request captured in promiscuous mode
};

// Bitmask values for ScanConfig.sources (1 << Source).
enum SourceMask : uint8_t {
  MASK_WIFI     = 1 << 0,
  MASK_BLE      = 1 << 1,
  MASK_154      = 1 << 2,
  MASK_PROBE    = 1 << 3,  // promiscuous Wi-Fi probe-request capture
  MASK_ALL      = MASK_WIFI | MASK_BLE | MASK_154,
};

// Per-detection behavioral flags (Detection.flags bitfield).
enum DetFlags : uint8_t {
  FLAG_WILDCARD_PROBE = 1 << 0,  // probe request with a zero-length (broadcast) SSID
};

#pragma pack(push, 1)

// Config for a StartScan command.
struct ScanConfig {
  uint8_t  sources;   // SourceMask bits: which radios to scan
  uint16_t dwell_ms;  // per-channel dwell time (0 = firmware default)
};

// One detected device reported by the C5 (Reply::Detection payload).
struct Detection {
  uint8_t  source;    // Source
  uint8_t  channel;   // Wi-Fi / 802.15.4 channel (0 if not applicable)
  int8_t   rssi;      // signal strength, dBm
  uint8_t  flags;     // DetFlags bitfield (0 if none)
  uint8_t  mac[6];    // device MAC / BSSID (probe: client source address)
  uint32_t ie_hash;   // 802.11 IE-order fingerprint (0 = none / not applicable)
  char     name[32];  // SSID or BLE name, NUL-terminated (may be empty)
};

// Sequenced heartbeat for the link connection monitor (Reply::Heartbeat payload).
// The receiver tracks gaps in `seq` to measure packet loss on the wire.
struct Heartbeat {
  uint32_t seq;
};

// C5 health/status snapshot (Reply::Status payload).
struct Status {
  uint8_t  scanning;      // 0 = idle, 1 = scanning
  uint8_t  active_sources;// SourceMask currently active
  uint16_t seen_total;    // devices seen since boot (wraps)
  uint32_t uptime_ms;
};

// Fixed header prepended to every frame's payload on the wire.
struct FrameHeader {
  uint8_t  start;    // == FRAME_START
  uint8_t  version;  // == PROTOCOL_VERSION
  uint8_t  type;     // Command or Reply value
  uint16_t length;   // payload byte count that follows
};

#pragma pack(pop)

// XOR checksum over a byte range.
inline uint8_t checksum(const uint8_t* data, uint16_t len) {
  uint8_t c = 0;
  for (uint16_t i = 0; i < len; ++i) c ^= data[i];
  return c;
}

// Bytes of framing overhead around a payload: start+version+type+len(2)+checksum.
static constexpr uint16_t FRAME_OVERHEAD = 6;
static constexpr uint16_t MAX_FRAME      = FRAME_OVERHEAD + MAX_PAYLOAD;

// Encode a complete frame into `out` (must hold at least FRAME_OVERHEAD + len
// bytes). Returns the total number of bytes written. The checksum covers every
// byte from `version` through the last payload byte (i.e. not the start marker).
inline uint16_t encodeFrame(uint8_t type, const uint8_t* payload, uint16_t len,
                            uint8_t* out) {
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  out[0] = FRAME_START;
  out[1] = PROTOCOL_VERSION;
  out[2] = type;
  out[3] = (uint8_t)(len & 0xFF);
  out[4] = (uint8_t)(len >> 8);
  for (uint16_t i = 0; i < len; ++i) out[FRAME_OVERHEAD - 1 + i] = payload[i];
  out[FRAME_OVERHEAD - 1 + len] = checksum(out + 1, 4 + len);
  return FRAME_OVERHEAD + len;
}

// Streaming byte-at-a-time frame decoder. Feed received bytes with feed(); it
// returns true once a complete, checksum-valid frame is available, then type(),
// length() and payload() describe it. Self-resynchronizes on the start marker.
class FrameParser {
 public:
  bool feed(uint8_t b) {
    switch (state_) {
      case S_START: if (b == FRAME_START) { sum_ = 0; state_ = S_VER; } break;
      case S_VER:   ver_ = b;  sum_ ^= b; state_ = S_TYPE; break;
      case S_TYPE:  type_ = b; sum_ ^= b; state_ = S_LEN_LO; break;
      case S_LEN_LO: lenLo_ = b; sum_ ^= b; state_ = S_LEN_HI; break;
      case S_LEN_HI:
        len_ = (uint16_t)lenLo_ | ((uint16_t)b << 8);
        sum_ ^= b;
        idx_ = 0;
        if (len_ > MAX_PAYLOAD) { state_ = S_START; break; }  // bad length, drop
        state_ = (len_ == 0) ? S_CKSUM : S_PAYLOAD;
        break;
      case S_PAYLOAD:
        payload_[idx_++] = b; sum_ ^= b;
        if (idx_ >= len_) state_ = S_CKSUM;
        break;
      case S_CKSUM: {
        bool ok = (b == sum_) && (ver_ == PROTOCOL_VERSION);
        state_ = S_START;
        return ok;
      }
    }
    return false;
  }

  void reset() { state_ = S_START; }
  uint8_t         type()    const { return type_; }
  uint16_t        length()  const { return len_; }
  const uint8_t*  payload() const { return payload_; }

 private:
  enum State { S_START, S_VER, S_TYPE, S_LEN_LO, S_LEN_HI, S_PAYLOAD, S_CKSUM };
  State    state_  = S_START;
  uint8_t  ver_    = 0;
  uint8_t  type_   = 0;
  uint8_t  lenLo_  = 0;
  uint16_t len_    = 0;
  uint16_t idx_    = 0;
  uint8_t  sum_    = 0;
  uint8_t  payload_[MAX_PAYLOAD];
};

}  // namespace link_protocol

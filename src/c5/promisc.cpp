// promisc.cpp — see promisc.h.
#include "promisc.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

using namespace link_protocol;

namespace {

promisc::DetCb g_cb     = nullptr;
volatile bool  g_active = false;

inline uint32_t fnv1a(uint32_t h, uint8_t b) { return (h ^ b) * 16777619u; }

// Build the IE-order fingerprint of a management-frame body and pull the SSID
// out along the way. We hash the ordered list of element IDs (the classic
// fingerprint that survives MAC randomization, since it's driver/firmware
// determined), folding the OUI of any vendor-specific element (id 221) in for
// extra discrimination. Element *contents* (SSID text, volatile fields) are
// deliberately excluded so the hash identifies the device, not the network.
// Returns 0 only when there are no elements at all (0 is the "none" sentinel).
uint32_t fingerprint(const uint8_t* f, int len, int off, char* ssid, int ssidCap) {
  uint32_t h = 2166136261u;  // FNV-1a 32-bit offset basis
  bool any = false;
  if (ssidCap > 0) ssid[0] = 0;
  while (off + 2 <= len) {
    uint8_t id = f[off], ln = f[off + 1];
    if (off + 2 + (int)ln > len) break;  // truncated element — stop
    const uint8_t* data = f + off + 2;
    h = fnv1a(h, id);
    if (id == 0 && ssidCap > 0 && ssid[0] == 0) {   // SSID element
      int c = ln < ssidCap - 1 ? ln : ssidCap - 1;
      memcpy(ssid, data, c);
      ssid[c] = 0;
    } else if (id == 221 && ln >= 3) {              // vendor-specific: fold OUI
      h = fnv1a(h, data[0]);
      h = fnv1a(h, data[1]);
      h = fnv1a(h, data[2]);
    }
    any = true;
    off += 2 + ln;
  }
  if (!any) return 0;
  return h ? h : 1;  // never collide with the "none" sentinel
}

// Promiscuous RX callback (runs in the Wi-Fi task). Management frames only.
void rxCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT || !g_cb) return;
  auto* pkt = reinterpret_cast<wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* f = pkt->payload;
  int len = pkt->rx_ctrl.sig_len;
  if (len > 4) len -= 4;   // drop the trailing FCS
  if (len < 24) return;    // need a full management header (through addr3)

  uint8_t fc = f[0];
  if ((fc & 0x0C) != 0) return;  // frame type != management
  uint8_t sub = fc >> 4;         // management subtype

  int     body;
  uint8_t source;
  if (sub == 4) {                       // probe request  -> Wi-Fi client
    body   = 24;
    source = (uint8_t)Source::WifiProbe;
  } else if (sub == 8 || sub == 5) {    // beacon / probe response -> AP
    body   = 36;                        // 24 hdr + 12 fixed (ts/interval/caps)
    source = (uint8_t)Source::WifiScan;
  } else {
    return;                             // ignore auth/assoc/etc.
  }
  if (len < body) return;

  Detection d{};
  d.source  = source;
  d.channel = pkt->rx_ctrl.channel;
  d.rssi    = (int8_t)pkt->rx_ctrl.rssi;
  memcpy(d.mac, f + 10, 6);  // addr2: client SA (probe req) or BSSID (beacon)

  char ssid[32];
  d.ie_hash = fingerprint(f, len, body, ssid, sizeof(ssid));
  strncpy(d.name, ssid, sizeof(d.name) - 1);
  if (source == (uint8_t)Source::WifiProbe && d.name[0] == 0)
    d.flags |= FLAG_WILDCARD_PROBE;  // zero-length SSID = wildcard probe

  g_cb(d);
}

}  // namespace

namespace promisc {

void begin(DetCb cb) { g_cb = cb; }

void enable() {
  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;  // management frames only
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&rxCb);
  esp_wifi_set_promiscuous(true);
  g_active = true;
}

void disable() {
  esp_wifi_set_promiscuous(false);
  g_active = false;
}

void setChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

bool active() { return g_active; }

}  // namespace promisc

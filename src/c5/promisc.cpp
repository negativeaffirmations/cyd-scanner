// promisc.cpp — see promisc.h.
#include "promisc.h"
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

using namespace link_protocol;

namespace {

promisc::DetCb g_cb     = nullptr;
promisc::ApCb  g_apCb   = nullptr;
volatile bool  g_active = false;

// Deauth/disassoc counting: 10 x 100 ms buckets = ~1 s sliding window.
constexpr int      DEAUTH_BUCKETS   = 10;
constexpr uint32_t DEAUTH_BUCKET_MS = 100;
constexpr uint32_t DEAUTH_WIN_MS    = 1000;
volatile uint32_t  g_dStamp[DEAUTH_BUCKETS] = {0};  // bucket start time (ms)
volatile uint16_t  g_dCnt[DEAUTH_BUCKETS]   = {0};
// Sticky peak: rxCb only runs during the ~3 s promiscuous window and the 1 s buckets decay fast,
// so hold the peak 1 s-rate for ~10 s. A CYD poll landing between promiscuous windows then still
// reports a recent flood instead of 0. (Still receive-only — this only remembers what was observed.)
constexpr uint32_t DEAUTH_STICKY_MS = 10000;
volatile uint16_t  g_dSticky  = 0;  // peak 1 s deauth+disassoc count seen recently
volatile uint32_t  g_dStickyT = 0;  // millis() when g_dSticky was captured

// Sum the deauth/disassoc buckets covering the last DEAUTH_WIN_MS (the live ~1 s rate).
static uint16_t deauth1s(uint32_t now) {
  uint32_t sum = 0;
  for (int i = 0; i < DEAUTH_BUCKETS; i++)
    if (now - g_dStamp[i] <= DEAUTH_WIN_MS && g_dCnt[i]) sum += g_dCnt[i];
  return sum > 65535 ? 65535 : (uint16_t)sum;
}

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

  if (sub == 10 || sub == 12) {         // deauth / disassoc: count only (observed, never sent)
    uint32_t now = millis();
    int b = (now / DEAUTH_BUCKET_MS) % DEAUTH_BUCKETS;
    uint32_t stamp = now - (now % DEAUTH_BUCKET_MS);
    if (g_dStamp[b] != stamp) { g_dStamp[b] = stamp; g_dCnt[b] = 0; }  // bucket rolled over
    if (g_dCnt[b] < 65535) g_dCnt[b] = g_dCnt[b] + 1;
    uint16_t cur = deauth1s(now);  // latch the peak so a poll outside this window still sees it
    if (cur >= g_dSticky || now - g_dStickyT > DEAUTH_STICKY_MS) { g_dSticky = cur; g_dStickyT = now; }
    return;
  }

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

  if (source == (uint8_t)Source::WifiScan) {  // beacon / probe response: passive IE decode
    bool rsn = false;
    char jname[sizeof(d.name)] = {0};
    for (int off = body; off + 2 <= len;) {
      uint8_t id = f[off], ln = f[off + 1];
      if (off + 2 + (int)ln > len) break;
      const uint8_t* e = f + off + 2;
      if (id == 48) rsn = true;                                   // RSN
      else if (id == 221 && ln >= 4) {
        if (e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xF2 && e[3] == 0x01) rsn = true;  // WPA1
        else if (e[0] == 0xFA && e[1] == 0x0B && e[2] == 0xBC)    // OpenDroneID Wi-Fi beacon
          d.flags |= FLAG_BLE_ODID;
        else if (e[0] == 0xDE && e[1] == 0xAD && e[2] == 0xBE && e[3] == 0xEF) {  // Pwnagotchi
          d.flags |= FLAG_WIFI_PWNAGOTCHI;
          // chunked JSON: salvage "name":"..." if it sits inside this chunk
          for (int k = 4; k + 8 <= ln && !jname[0]; k++) {
            if (memcmp(e + k, "\"name\":\"", 8) != 0) continue;
            int c = 0;
            for (int m = k + 8; m < ln && e[m] != '"' && c < (int)sizeof(jname) - 1; m++) jname[c++] = (char)e[m];
            jname[c] = 0;
          }
        }
      }
      // Fallback marker: "pwnd" text in any vendor chunk or a JSON-looking SSID.
      if (id == 221 || id == 0) {
        for (int k = 0; k + 4 <= ln; k++)
          if (memcmp(e + k, "pwnd", 4) == 0) { d.flags |= FLAG_WIFI_PWNAGOTCHI; break; }
      }
      off += 2 + ln;
    }
    if ((d.flags & FLAG_WIFI_PWNAGOTCHI) && jname[0] && !d.name[0]) strncpy(d.name, jname, sizeof(d.name) - 1);
    g_cb(d);
    if (g_apCb && ssid[0]) {
      bool priv = f[34] & 0x10;  // capability: Privacy
      g_apCb(ssid, d.mac, rsn ? 2 : (priv ? 1 : 0), d.channel);
    }
    return;
  }

  g_cb(d);
}

}  // namespace

namespace promisc {

void begin(DetCb cb, ApCb apCb) { g_cb = cb; g_apCb = apCb; }

uint16_t deauthRecent() {
  uint32_t now = millis();
  uint16_t cur    = deauth1s(now);
  uint16_t sticky = (now - g_dStickyT <= DEAUTH_STICKY_MS) ? g_dSticky : 0;
  return cur > sticky ? cur : sticky;
}

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

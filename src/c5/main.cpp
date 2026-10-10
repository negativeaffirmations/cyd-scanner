// main.cpp — ESP32-C5 scanner co-processor firmware (env:c5)
//
// Two build modes (LINK_MONITOR):
//   0 = NORMAL async scanner. Scans dual-band Wi-Fi (async, non-blocking) and BLE
//       (continuous, callback-driven) in the background into a live detection
//       table. When the CYD polls (StartScan), it instantly dumps the current
//       table — so scanning never blocks the link or the CYD's UI.
//   1 = LINK MONITOR (heartbeat) — connection tester for hunting flaky wiring.
//
// Debug -> native USB console. Link UART -> GPIO4/GPIO5 (XTAL clock).

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include "pins.h"
#include "link_protocol.h"
#include "promisc.h"
#include "ieee154.h"

using namespace link_protocol;

// >>> Set to 1 to run the link connection monitor, 0 for the normal scanner. <<<
#define LINK_MONITOR 0

HardwareSerial LinkSerial(1);  // UART1 on GPIO4/GPIO5
FrameParser    parser;

static void sendFrame(uint8_t type, const void* payload, uint16_t len) {
  uint8_t out[MAX_FRAME];
  uint16_t n = encodeFrame(type, (const uint8_t*)payload, len, out);
  LinkSerial.write(out, n);
  LinkSerial.flush();
}

#if LINK_MONITOR
// ---------------------------------------------------------------- monitor mode
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[C5] boot: LINK MONITOR (heartbeat every 500ms)");
  LinkSerial.setClockSource(UART_CLK_SRC_XTAL);
  LinkSerial.setRxBufferSize(1024);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  Serial.printf("[C5] link UART RX=%d TX=%d @ %lu baud\n",
                LINK_RX_PIN, LINK_TX_PIN, (unsigned long)LINK_BAUD);
}

void loop() {
  while (LinkSerial.available()) {
    if (parser.feed(LinkSerial.read()) && parser.type() == (uint8_t)Command::Ping)
      sendFrame((uint8_t)Reply::Pong, nullptr, 0);
  }
  static uint32_t seq = 0, last = 0;
  if (millis() - last >= 500) {
    last = millis();
    Heartbeat hb{};
    hb.seq = seq++;
    sendFrame((uint8_t)Reply::Heartbeat, &hb, sizeof(hb));
  }
}

#else
// ---------------------------------------------------------------- async scanner

// Live detection table, shared between the WiFi poll (loop task), the BLE scan
// callback (NimBLE task), and the stream-on-request path. Guarded by g_mux.
static constexpr int      MAX_ENTRIES  = 96;
static constexpr uint32_t ENTRY_TTL_MS = 30000;  // drop devices unseen for 30 s
static constexpr uint32_t WIFI_GAP_MS  = 3000;   // pause between WiFi scans

struct Entry {
  Detection d;
  uint32_t  lastSeen;
  bool      used;
};
static Entry              g_table[MAX_ENTRIES];
static SemaphoreHandle_t  g_mux;
static Detection          g_streamBuf[MAX_ENTRIES];  // copy target for streaming

// Wi-Fi radio phase (declared early: the BLE callback/watchdog read it).
enum WifiPhase { PH_SCAN, PH_PROMISC, PH_154 };
static volatile WifiPhase g_phase = PH_SCAN;

// BLE must be paused while the radio does a PASSIVE capture (promiscuous Wi-Fi OR 802.15.4): a
// continuous BLE scan runs at ~99% duty and starves the passive RX of airtime (measured: only ~2
// mgmt frames / 0 data per promiscuous window with BLE live). Active AP scan (PH_SCAN) gets coex
// priority so BLE can run alongside it. Every BLE (re)start path checks this.
static inline bool blePaused() { return g_phase == PH_PROMISC || g_phase == PH_154; }

// Active scan-source mask (SourceMask bits), set by the CYD's StartScan payload.
// Disabled sources are only skipped/dropped here - never transmitted around.
static volatile uint8_t   g_srcMask = MASK_ALL & ~MASK_154;  // 802.15.4 is opt-in

// Is this table entry's source currently enabled? Wi-Fi AP-scan detections are
// filtered per band by channel.
static bool sourceEnabled(const Detection& d) {
  uint8_t m = g_srcMask;
  switch ((Source)d.source) {
    case Source::BleScan:    return m & MASK_BLE;
    case Source::Ieee802154: return m & MASK_154;
    case Source::WifiProbe:  return m & MASK_PROBE;
    default: return d.channel > 14 ? (m & MASK_WIFI5) : (m & MASK_WIFI24);
  }
}

static bool sameDev(const Detection& a, const Detection& b) {
  if (a.source != b.source || memcmp(a.mac, b.mac, 6) != 0) return false;
  // Short 15.4 addresses are unique only within a PAN (e.g. every coordinator is 0x0000).
  if (a.source == (uint8_t)Source::Ieee802154 && !(a.flags & FLAG_154_EXTENDED))
    return a.panId == b.panId;
  return true;
}

// Insert or refresh a detection (dedup by source+MAC). Thread-safe.
static void mergeDetection(const Detection& d) {
  if (!g_mux) return;
  xSemaphoreTake(g_mux, portMAX_DELAY);
  int freeIdx = -1, oldestIdx = 0;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (!g_table[i].used) { if (freeIdx < 0) freeIdx = i; continue; }
    if (sameDev(g_table[i].d, d)) {
      // Don't lose identifying info a later, sparser advertisement might omit.
      uint32_t keepIe  = d.ie_hash ? d.ie_hash : g_table[i].d.ie_hash;
      uint16_t keepFl  = d.flags | g_table[i].d.flags;                  // flags are sticky
      uint16_t keepCid = d.companyId ? d.companyId : g_table[i].d.companyId;
      uint16_t keepPan = d.panId ? d.panId : g_table[i].d.panId;
      bool     haveSvc = false;
      for (int k = 0; k < 16; k++) if (d.svc[k]) { haveSvc = true; break; }
      Detection prev = g_table[i].d;
      g_table[i].d = d;
      g_table[i].d.ie_hash   = keepIe;
      g_table[i].d.flags     = keepFl;
      g_table[i].d.companyId = keepCid;
      g_table[i].d.panId     = keepPan;
      if (!haveSvc) memcpy(g_table[i].d.svc, prev.svc, 16);  // keep prior UUID
      // Union the extra 16-bit UUID sets (adv + scan-response reports each carry a subset).
      for (int p = 0; p < SVC16_MAX && prev.svc16[p]; p++) {
        bool have = false;
        for (int k = 0; k < SVC16_MAX; k++) if (g_table[i].d.svc16[k] == prev.svc16[p]) { have = true; break; }
        if (have) continue;
        for (int k = 0; k < SVC16_MAX; k++) if (!g_table[i].d.svc16[k]) { g_table[i].d.svc16[k] = prev.svc16[p]; break; }
      }
      // Keep a previously-captured name when this (often nameless ADV_IND) packet omits it:
      // the complete local name usually rides only in the scan response, so otherwise an
      // interleaved nameless advert clobbers it and the device streams out as <hidden>.
      if (!d.name[0] && prev.name[0]) strncpy(g_table[i].d.name, prev.name, sizeof(g_table[i].d.name) - 1);
      g_table[i].lastSeen = millis();
      xSemaphoreGive(g_mux);
      return;
    }
    if (g_table[i].lastSeen < oldest) { oldest = g_table[i].lastSeen; oldestIdx = i; }
  }
  int idx = (freeIdx >= 0) ? freeIdx : oldestIdx;  // reuse oldest if full
  g_table[idx].d = d;
  g_table[idx].lastSeen = millis();
  g_table[idx].used = true;
  xSemaphoreGive(g_mux);
}

// --- Evil-twin: SSID observation table (fed by AP scan + promiscuous beacons) ---
// Verdict: the SAME SSID seen from >=2 distinct BSSIDs whose encryption CLASS differs
// (open / WEP / secured). BSSID count alone never flags (band-roaming, mesh, extenders).
// Guarded by g_mux (promiscuous callback + loop task + stream path).
static constexpr int SSID_SLOTS = 24, SSID_APS = 4;
struct SsidObs {
  char     ssid[33];
  uint8_t  n;          // valid aps
  bool     used, evil;
  uint32_t last;
  struct { uint8_t bssid[6]; uint8_t enc; uint8_t ch; uint32_t t; } ap[SSID_APS];
};
static SsidObs g_ssidTab[SSID_SLOTS];

// Drop stale APs, recompute the verdict. Caller holds g_mux.
static void ssidRecompute(SsidObs& e, uint32_t now) {
  int w = 0;
  for (int i = 0; i < e.n; i++)
    if (now - e.ap[i].t <= ENTRY_TTL_MS) e.ap[w++] = e.ap[i];
  e.n = (uint8_t)w;
  e.evil = false;
  for (int i = 1; i < e.n && !e.evil; i++)
    for (int j = 0; j < i; j++)
      if (e.ap[i].enc != e.ap[j].enc) { e.evil = true; break; }
  if (e.n == 0) e.used = false;
}

// OR FLAG_WIFI_EVILTWIN onto the AP detections of an evil SSID. Caller holds g_mux.
static void flagEvilEntries(const SsidObs& e) {
  for (int a = 0; a < e.n; a++)
    for (int i = 0; i < MAX_ENTRIES; i++)
      if (g_table[i].used && g_table[i].d.source == (uint8_t)Source::WifiScan &&
          memcmp(g_table[i].d.mac, e.ap[a].bssid, 6) == 0)
        g_table[i].d.flags |= FLAG_WIFI_EVILTWIN;
}

// enc class: 0 open, 1 WEP, 2 secured (WPA/RSN/WPA3).
static void ssidObserve(const char* ssid, const uint8_t* bssid, uint8_t enc, uint8_t ch) {
  if (!g_mux || !ssid[0]) return;  // hidden SSIDs can't be compared
  uint32_t now = millis();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  int freeIdx = -1, oldIdx = 0, hit = -1;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < SSID_SLOTS; i++) {
    if (!g_ssidTab[i].used) { if (freeIdx < 0) freeIdx = i; continue; }
    if (strncmp(g_ssidTab[i].ssid, ssid, 32) == 0) { hit = i; break; }
    if (g_ssidTab[i].last < oldest) { oldest = g_ssidTab[i].last; oldIdx = i; }
  }
  if (hit < 0) {
    hit = (freeIdx >= 0) ? freeIdx : oldIdx;
    memset(&g_ssidTab[hit], 0, sizeof(SsidObs));
    strncpy(g_ssidTab[hit].ssid, ssid, 32);
    g_ssidTab[hit].used = true;
  }
  SsidObs& e = g_ssidTab[hit];
  e.last = now;
  int slot = -1;
  for (int i = 0; i < e.n; i++) if (memcmp(e.ap[i].bssid, bssid, 6) == 0) { slot = i; break; }
  if (slot < 0) {
    if (e.n < SSID_APS) slot = e.n++;
    else {  // full: replace the stalest AP
      slot = 0;
      for (int i = 1; i < SSID_APS; i++) if (e.ap[i].t < e.ap[slot].t) slot = i;
    }
    memcpy(e.ap[slot].bssid, bssid, 6);
  }
  e.ap[slot].enc = enc;
  e.ap[slot].ch  = ch;
  e.ap[slot].t   = now;
  ssidRecompute(e, now);
  if (e.used && e.evil) flagEvilEntries(e);
  xSemaphoreGive(g_mux);
}

// Count currently-evil SSIDs and distinct ODID emitters (presence) for Status.
static void countAggregates(uint8_t& evil, uint8_t& rid) {
  uint32_t now = millis();
  int ev = 0, rd = 0;
  xSemaphoreTake(g_mux, portMAX_DELAY);
  for (int i = 0; i < SSID_SLOTS; i++)
    if (g_ssidTab[i].used) { ssidRecompute(g_ssidTab[i], now); if (g_ssidTab[i].used && g_ssidTab[i].evil) ev++; }
  for (int i = 0; i < MAX_ENTRIES; i++)
    if (g_table[i].used && now - g_table[i].lastSeen <= ENTRY_TTL_MS &&
        (g_table[i].d.flags & FLAG_BLE_ODID) && sourceEnabled(g_table[i].d)) rd++;
  xSemaphoreGive(g_mux);
  evil = ev > 255 ? 255 : (uint8_t)ev;
  rid  = rd > 255 ? 255 : (uint8_t)rd;
}

// --- BLE: continuous scan, merging each advertisement into the table ---
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    Detection d{};
    d.source  = (uint8_t)Source::BleScan;
    d.channel = 0;
    d.rssi    = (int8_t)dev->getRSSI();
    unsigned v[6] = {0};
    if (sscanf(dev->getAddress().toString().c_str(), "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
      for (int i = 0; i < 6; i++) d.mac[i] = (uint8_t)v[i];
    std::string nm = dev->getName();
    if (!nm.empty()) strncpy(d.name, nm.c_str(), sizeof(d.name) - 1);

    // BLE manufacturer company ID (first 2 bytes of manufacturer data, LE).
    std::string md = dev->getManufacturerData();
    if (md.size() >= 2)
      d.companyId = (uint16_t)((uint8_t)md[0] | ((uint8_t)md[1] << 8));

    // Passive advert decode (same scan, no extra scanning): Apple continuity subtype.
    // md = [cid_lo cid_hi type len ...]; guard length before indexing.
    if (md.size() >= 4 && d.companyId == 0x004C) {
      uint8_t t = (uint8_t)md[2], l = (uint8_t)md[3];
      if (t == 0x02 && l == 0x15) d.flags |= FLAG_BLE_IBEACON;
      else if (t == 0x12 || t == 0x07) d.flags |= FLAG_BLE_FINDMY;
    }
    // OpenDroneID rides service data under 16-bit UUID 0xFFFA (presence only).
    for (uint8_t i = 0, n = dev->getServiceDataCount(); i < n; i++) {
      if (dev->getServiceDataUUID(i) == NimBLEUUID((uint16_t)0xFFFA)) { d.flags |= FLAG_BLE_ODID; break; }
    }

    // Advertised service UUIDs. The primary (first) is stored full 128-bit big-endian in svc[]
    // (NimBLE stores it little-endian; a 16-bit UUID expands to the Bluetooth base). Active
    // scanning often returns MULTIPLE UUIDs (adv + scan response) and the signature one isn't
    // always first, so additionally collect up to SVC16_MAX distinct 16-bit UUIDs into svc16[]
    // for the CYD to match (covers Tile/Samsung/Raven/ODID/Flipper/Eddystone-class signatures;
    // a secondary 128-bit UUID beyond the primary is not carried).
    uint8_t nsvc16 = 0;
    for (uint8_t i = 0, n = dev->getServiceUUIDCount(); i < n; i++) {
      NimBLEUUID u = dev->getServiceUUID(i);
      if (i == 0) {
        const uint8_t* le = u.to128().getValue();  // 16 bytes, little-endian
        for (int k = 0; k < 16; k++) d.svc[k] = le[15 - k];
      }
      if (u.bitSize() != 16 || nsvc16 >= SVC16_MAX) continue;
      const uint8_t* le = u.to128().getValue();          // 16-bit value lives at LE bytes 12/13
      uint16_t v = (uint16_t)(le[12] | ((uint16_t)le[13] << 8));
      if (!v) continue;
      bool dup = false;
      for (uint8_t k = 0; k < nsvc16; k++) if (d.svc16[k] == v) { dup = true; break; }
      if (!dup) d.svc16[nsvc16++] = v;
    }
    mergeDetection(d);
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    if ((g_srcMask & MASK_BLE) && !blePaused()) { NimBLEScan* s = NimBLEDevice::getScan(); if (s) s->start(0, false); }  // keep scanning continuously
  }
};
static ScanCB g_scanCB;

// --- WiFi: two-phase capture, polled from loop() ---
//
// The radio can't do an all-channel WiFi.scanNetworks() and a fixed-channel
// promiscuous capture at once, so we time-slice:
//   PH_SCAN    — async dual-band AP scan (2.4 + 5 GHz beacons via scanNetworks)
//   PH_PROMISC — passive promiscuous capture, hopping the 2.4 GHz probe hotspots
//                (1/6/11) to catch client probe requests + IE fingerprints.
// BLE runs continuously throughout (separate controller, coexistence-managed).
// PH_154 — passive 802.15.4 capture, entered with the Wi-Fi PHY idle.
static bool      g_wifiScanning = false;
static uint32_t  g_lastWifiDone = 0;

// All US 2.4 GHz channels (1-11): associated cameras sit on whatever channel their home AP chose,
// which is often NOT 1/6/11 (routers auto-select 3/4/8/9/...), so sweep the whole band. BLE is paused
// for this window (see blePaused()), so passive RX gets full airtime even across more channels.
static const uint8_t   kHopChans[]      = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static constexpr int   NUM_HOPS         = sizeof(kHopChans) / sizeof(kHopChans[0]);
static constexpr uint32_t PROMISC_MS    = 4500;  // length of a capture window (covers ~2.5 full sweeps)
static constexpr uint32_t HOP_DWELL_MS  = 160;   // per-channel dwell while capturing
static uint32_t g_promStart = 0, g_lastHop = 0;
static int      g_hopIdx    = 0;

// Promiscuous sink (Wi-Fi task context): just fold each frame into the table.
static void onPromisc(const Detection& d) { mergeDetection(d); }
static void onPromiscAp(const char* ssid, const uint8_t* bssid, uint8_t enc, uint8_t ch) { ssidObserve(ssid, bssid, enc, ch); }

static void on154(const Detection& d) { mergeDetection(d); }

static const uint8_t      k154Chans[]     = {11, 15, 20, 25, 26};
static constexpr int      NUM_154_CHANS    = sizeof(k154Chans) / sizeof(k154Chans[0]);
static constexpr uint32_t IEEE154_MS       = 3000;  // length of a capture window
static constexpr uint32_t IEEE154_DWELL_MS = 150;   // per-channel dwell while capturing
static uint32_t g_154Start = 0, g_154LastHop = 0;
static int      g_154Idx    = 0;
static bool     g_154Logged = false;

static void enter154() {
  esp_err_t e = ieee154::enable();
  if (!g_154Logged) {
    g_154Logged = true;
    Serial.printf("[C5] 802.15.4 enable() -> 0x%x (%s)\n", (unsigned)e, esp_err_to_name(e));
  }
  if (e != ESP_OK) {            // PHY unavailable: skip this window
    ieee154::disable();
    g_lastWifiDone = millis();
    return;
  }
  g_phase      = PH_154;  // publish first: once PH_154 is visible no restart path can re-arm BLE
  if (g_srcMask & MASK_BLE) { NimBLEScan* s = NimBLEDevice::getScan(); if (s) s->stop(); }  // then free the radio for 15.4 RX
  g_154Idx     = 0;
  g_154Start   = millis();
  g_154LastHop = g_154Start;
  ieee154::setChannel(k154Chans[0]);
}
// Radio hand-off after the Wi-Fi phases: 802.15.4 window if enabled, else the gap.
static void after154Slot() {
  if (g_srcMask & MASK_154) enter154();
  else g_lastWifiDone = millis();
}

static void enterPromisc() {
  g_phase     = PH_PROMISC;   // publish first: blePaused() is now true, so no path re-arms BLE
  if (g_srcMask & MASK_BLE) { NimBLEScan* s = NimBLEDevice::getScan(); if (s) s->stop(); }  // free the radio for passive RX
  g_hopIdx    = 0;
  g_promStart = millis();
  g_lastHop   = g_promStart;
  promisc::enable();
  promisc::setChannel(kHopChans[0]);
}

// After the AP-scan phase: capture probes if enabled, else just wait out the gap.
static void afterScanPhase() {
  if (g_srcMask & MASK_PROBE) enterPromisc();
  else after154Slot();
}

static void wifiTick() {
  if (g_phase == PH_SCAN) {
    if (!g_wifiScanning) {
      if (millis() - g_lastWifiDone < WIFI_GAP_MS) return;
      if (!(g_srcMask & (MASK_WIFI24 | MASK_WIFI5))) {  // AP scan fully disabled
        afterScanPhase();
        return;
      }
      WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);  // 2.4 + 5 GHz
      g_wifiScanning = true;
      return;
    }
    int n = WiFi.scanComplete();
    if (n >= 0) {
      for (int i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)WiFi.channel(i);
        if (ch > 14 && !(g_srcMask & MASK_WIFI5)) continue;    // 5 GHz disabled
        if (ch <= 14 && !(g_srcMask & MASK_WIFI24)) continue;  // 2.4 GHz disabled
        Detection d{};
        d.source  = (uint8_t)Source::WifiScan;
        d.channel = (uint8_t)WiFi.channel(i);
        d.rssi    = (int8_t)WiFi.RSSI(i);
        memcpy(d.mac, WiFi.BSSID(i), 6);
        strncpy(d.name, WiFi.SSID(i).c_str(), sizeof(d.name) - 1);
        mergeDetection(d);
        wifi_auth_mode_t am = WiFi.encryptionType(i);
        if (am != WIFI_AUTH_OWE)  // enhanced-open legitimately pairs with open (transition mode)
          ssidObserve(d.name, d.mac, am == WIFI_AUTH_OPEN ? 0 : am == WIFI_AUTH_WEP ? 1 : 2, d.channel);
      }
      WiFi.scanDelete();
      g_wifiScanning = false;
      afterScanPhase();               // hand the radio to promiscuous capture
    } else if (n == WIFI_SCAN_FAILED) {
      g_wifiScanning = false;
      afterScanPhase();
    }
    return;
  }

  if (g_phase == PH_154) {
    uint32_t t = millis();
    if (t - g_154Start >= IEEE154_MS) {
      ieee154::disable();             // hand the PHY back before the next AP scan
      g_phase        = PH_SCAN;
      g_lastWifiDone = t;
      if (g_srcMask & MASK_BLE) {         // resume BLE promptly (watchdog is the backstop)
        NimBLEScan* bs = NimBLEDevice::getScan();
        if (bs && !bs->isScanning()) bs->start(0, false);
      }
      return;
    }
    if (t - g_154LastHop >= IEEE154_DWELL_MS) {
      g_154LastHop = t;
      g_154Idx     = (g_154Idx + 1) % NUM_154_CHANS;
      ieee154::setChannel(k154Chans[g_154Idx]);
    }
    return;
  }

  // PH_PROMISC: hop channels for the capture window, then return to scanning.
  uint32_t now = millis();
  if (now - g_promStart >= PROMISC_MS) {
    promisc::disable();
    uint32_t pm, pd, pdc; promisc::frameStats(pm, pd, pdc);  // field diagnostic: capture health
    Serial.printf("[C5] promisc window end: mgmt=%u data=%u dataClient=%u\n", (unsigned)pm, (unsigned)pd, (unsigned)pdc);
    g_phase        = PH_SCAN;
    g_lastWifiDone = now;             // honor WIFI_GAP_MS before the next scan
    after154Slot();                   // may hand the radio to 802.15.4 (which keeps BLE paused)
    if (g_phase == PH_SCAN && (g_srcMask & MASK_BLE)) {  // back to scan phase -> resume BLE promptly
      NimBLEScan* bs = NimBLEDevice::getScan();
      if (bs && !bs->isScanning()) bs->start(0, false);
    }
    return;
  }
  if (now - g_lastHop >= HOP_DWELL_MS) {
    g_lastHop = now;
    g_hopIdx  = (g_hopIdx + 1) % NUM_HOPS;
    promisc::setChannel(kHopChans[g_hopIdx]);
  }
}

static void sendStatus(uint8_t scanning, uint16_t total) {
  Status st{};
  st.scanning       = scanning;
  st.active_sources = g_srcMask;
  st.seen_total     = total;
  st.uptime_ms      = millis();
  st.deauth_recent  = promisc::deauthRecent();
  countAggregates(st.evil_count, st.rid_count);
  sendFrame((uint8_t)Reply::Status, &st, sizeof(st));
}

// Snapshot the live table (dropping stale entries) and stream it to the CYD.
static void streamTable() {
  int cnt = 0;
  uint32_t now = millis();
  xSemaphoreTake(g_mux, portMAX_DELAY);
  for (int i = 0; i < MAX_ENTRIES; i++) {
    if (!g_table[i].used) continue;
    if (now - g_table[i].lastSeen > ENTRY_TTL_MS) { g_table[i].used = false; continue; }
    if (!sourceEnabled(g_table[i].d)) continue;  // just-disabled source in a stale table
    g_streamBuf[cnt++] = g_table[i].d;
  }
  xSemaphoreGive(g_mux);

  sendStatus(1, cnt);
  for (int i = 0; i < cnt; i++) {
    sendFrame((uint8_t)Reply::Detection, &g_streamBuf[i], sizeof(Detection));
    delay(2);
  }
  sendStatus(0, cnt);
  Serial.printf("[C5] streamed %d detections\n", cnt);
  while (LinkSerial.available()) LinkSerial.read();  // drop stale commands
  parser.reset();
}

// Start/stop the continuous BLE scan to match MASK_BLE (watchdog also honors it).
static void applyBleMask() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan) return;
  bool want = (g_srcMask & MASK_BLE) && !blePaused();
  if (want && !scan->isScanning()) scan->start(0, false);
  else if (!want && scan->isScanning()) scan->stop();
}

static void handleFrame(uint8_t type, const uint8_t* payload, uint16_t len) {
  switch ((Command)type) {
    case Command::Ping:      sendFrame((uint8_t)Reply::Pong, nullptr, 0); break;
    case Command::GetStatus: sendStatus(0, 0); break;
    case Command::StartScan:
      if (len >= sizeof(ScanConfig)) {
        g_srcMask = ((const ScanConfig*)payload)->sources;
        applyBleMask();
      }
      streamTable();
      break;
    case Command::StopScan:  break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[C5] boot: async scanner (Wi-Fi dual-band scan + promiscuous probe/beacon + BLE)");

  g_mux = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  promisc::begin(&onPromisc, &onPromiscAp);  // promiscuous capture feeds the same table
  ieee154::begin(&on154);      // 802.15.4 presence feeds the same table

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&g_scanCB, /*wantDuplicates=*/true);  // refresh lastSeen
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  // Callback-only: don't buffer results in NimBLE's cache. We already store everything
  // in g_table, and an unbounded cache (with duplicates, while driving past thousands of
  // BLE devices) fills the heap and silently stalls the scan. This was the bug that made
  // BLE detections stop ~10 min into a drive while Wi-Fi kept going.
  scan->setMaxResults(0);
  scan->start(0, false);  // continuous

  LinkSerial.setClockSource(UART_CLK_SRC_XTAL);
  LinkSerial.setRxBufferSize(1024);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);
  Serial.println("[C5] scanning in background; waiting for commands");
}

void loop() {
  wifiTick();
  ieee154::tick();

  // BLE watchdog: if the scan ever stops (stall, error, or an onScanEnd we missed),
  // restart it. Belt-and-suspenders alongside setMaxResults(0).
  static uint32_t lastBleChk = 0;
  if (millis() - lastBleChk > 5000) {
    lastBleChk = millis();
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && (g_srcMask & MASK_BLE) && !blePaused() && !scan->isScanning()) {
      scan->start(0, false);
      Serial.println("[C5] BLE scan restarted by watchdog");
    }
  }

  while (LinkSerial.available())
    if (parser.feed(LinkSerial.read()))
      handleFrame(parser.type(), parser.payload(), parser.length());
  delay(5);
}
#endif

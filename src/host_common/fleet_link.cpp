// fleet_link.cpp — see fleet.h and docs/fleet-mode.md §1, §3. Roles + boot-loop guard (NVS), Wi-Fi STA + ESP-NOW
// bring-up with a random locally-administered MAC, the RX pool + index queues, and (FLEET_DEBUG only) the P1
// ping/pong bench instrument. Receive-only in production P1: nothing is transmitted without FLEET_DEBUG.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/md.h>
#include <new>
#include <ctype.h>
#include "fleet.h"
#include "fleet_proto.h"
#include "fleet_internal.h"
#include "devkey.h"
#include "host_board.h"
#include "webshare.h"
#include "host_core.h"   // g_srcMask (planString)
#include "host_log.h"    // logXferActive

using namespace fleet_proto;

// Transport state shared with fleet_leader.cpp / fleet_client.cpp (fleet_internal.h).
namespace fleet { namespace detail {
uint8_t  g_ch = FLEET_DEFAULT_CH;
bool     g_active = false, g_leader = false;
uint8_t  g_mac[6] = {0};
uint16_t g_maxPayload = 0;
}}
using namespace fleet::detail;

namespace {

// Fixed project PMK (secrecy rests on the per-session LMK from P2 on; the PMK only keys the LMK wrapping).
const uint8_t kPmk[16] = { 'C','F','L','O','-','F','L','T','-','P','M','K','-','0','0','1' };

constexpr uint32_t STABLE_MS     = 60000;   // uptime after which the crash counter is cleared
constexpr uint8_t  CRASH_BOOTS   = 3;
constexpr int      DRAIN_MAX     = 8;       // RX frames handled per service() pass
constexpr size_t   CLIENT_SLOT   = 256;     // client RX slot (header + data); leader->client frames are <= 243 B (Sync + tag)
constexpr int      CLIENT_SLOTS  = 8;
constexpr int      LEADER_SLOTS  = 48;      // PSRAM: several members' DetBatch bursts land while loop() runs a cycle
constexpr int      FALLBACK_SLOTS = 8;      // leader pool when PSRAM is unavailable

// --- static footprint (normal mode must stay < 300 B: everything big is allocated by begin*) ---
uint8_t  g_role = 0, g_fellBackRole = 0;
bool     g_fellBack = false, g_bootCounted = false;
bool     g_rate12 = true, g_rateWarned = false;
void   (*g_eventSink)(const char*, const fleet::EventInfo&) = nullptr;
uint32_t g_events = 0;
volatile uint8_t g_badVer = 0;   // a frame with another fleet proto version (set in onRecv, reported once by service())
bool     g_badVerShown = false;
uint8_t* g_pool = nullptr;
uint16_t g_slotSize = 0;
uint8_t  g_slots = 0;
QueueHandle_t g_freeQ = nullptr, g_fullQ = nullptr;
uint32_t g_txOk = 0, g_txFail = 0, g_rxFrames = 0, g_rxDrop = 0;   // written by the Wi-Fi task, read by loop()
#define BUMP(c) __atomic_fetch_add(&(c), 1u, __ATOMIC_RELAXED)

// One RX pool slot: copied out of the Wi-Fi task, handled in service(). `len` is the TRUE frame length;
// only min(len, cap) bytes are stored (truncation is allowed only for the oversized debug frames).
struct Slot { uint8_t mac[6]; int8_t rssi; uint8_t flags; uint16_t len; uint8_t data[]; };
constexpr uint8_t SF_TRUNC = 1;

inline Slot* slotAt(uint8_t i) { return (Slot*)(g_pool + (size_t)i * g_slotSize); }
inline uint16_t slotCap() { return (uint16_t)(g_slotSize - sizeof(Slot)); }

void printHeap(const char* tag) {
  Serial.printf("[FLT] %s heap free=%u min=%u largest=%u", tag, (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM))
    Serial.printf(" | internal free=%u psram free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println();
}

// ------------------------------------------------------------------ peers / rate
void applyRate(const uint8_t* mac) {
  esp_now_rate_config_t rc = {};
  if (g_rate12) { rc.phymode = WIFI_PHY_MODE_11G; rc.rate = WIFI_PHY_RATE_12M; }
  else          { rc.phymode = WIFI_PHY_MODE_11B; rc.rate = WIFI_PHY_RATE_1M_L; }
  esp_err_t e = esp_now_set_peer_rate_config(mac, &rc);
  if (e != ESP_OK && !g_rateWarned) {   // keep the 1 Mbps default; delivery is judged by the P1 bench
    g_rateWarned = true;
    Serial.printf("[FLT] peer rate config failed (%d): staying on the default rate\n", (int)e);
  }
}

bool ensurePeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t pi = {};
  memcpy(pi.peer_addr, mac, 6);
  pi.channel = 0;                  // current channel
  pi.ifidx = WIFI_IF_STA;
  pi.encrypt = false;
  esp_err_t e = esp_now_add_peer(&pi);
  if (e != ESP_OK) { Serial.printf("[FLT] add_peer failed (%d)\n", (int)e); return false; }
  applyRate(mac);
  return true;
}

// ------------------------------------------------------------------ FLEET_DEBUG bench instrument
#ifdef FLEET_DEBUG
struct DbgPeer {                    // leader: one per client that ponged
  uint8_t  mac[6];
  bool     used;
  int8_t   rssi;
  uint32_t pong[2], since[2];       // [0] = 250 B class, [1] = 1300 B class; since = pings sent since first heard
  uint32_t lastMs;
};
struct Dbg {
  uint32_t seq, lastPingMs, lastPrintMs;
  uint32_t pingSent[2];
  int      padOverride;             // 0 = alternate 250 / 1300
  DbgPeer  peers[FLEET_MAX_MEMBERS];
  // client side
  uint8_t  leader[6];
  bool     haveLeader;
  int8_t   lrssi;
  uint32_t lastRxMs, lastSeq, gaps, rxPing[2], pongSent[2];
  uint8_t  tx[FLEET_V2_MAX];        // padded frame buffer
};
Dbg* g_dbg = nullptr;
bool g_trace = false;               // `fleet trace on`: one console line per ping sent / received (stall hunting)

inline int sizeClass(uint16_t len) { return len > 400 ? 1 : 0; }

uint8_t rfChannel() {               // the channel the radio is actually on (vs g_ch, the one we asked for)
  uint8_t p = 0; wifi_second_chan_t s;
  return esp_wifi_get_channel(&p, &s) == ESP_OK ? p : 0;
}

// Wi-Fi STA events that could move the radio off the fleet channel (a scan / connect attempt / restart).
void dbgWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_START:     Serial.printf("[FLT] %lu wifi STA_START\n", millis()); break;
    case ARDUINO_EVENT_WIFI_STA_STOP:      Serial.printf("[FLT] %lu wifi STA_STOP\n", millis()); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: Serial.printf("[FLT] %lu wifi STA_CONNECTED\n", millis()); break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[FLT] %lu wifi STA_DISCONNECTED reason %u\n", millis(), (unsigned)info.wifi_sta_disconnected.reason); break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:     Serial.printf("[FLT] %lu wifi SCAN_DONE\n", millis()); break;
    default: break;
  }
}

bool dbgSend(const uint8_t* mac, Msg type, uint32_t seq, uint16_t frameLen) {
  if (frameLen < sizeof(DbgPing)) frameLen = sizeof(DbgPing);
  if (frameLen > g_maxPayload) frameLen = g_maxPayload;
  if (!ensurePeer(mac)) return false;
  DbgPing* p = (DbgPing*)g_dbg->tx;
  p->h.ver = FLEET_PROTO_VERSION; p->h.type = (uint8_t)type; p->h.sess = 0; p->h.seq = seq;
  p->padLen = frameLen - sizeof(DbgPing);
  memset(g_dbg->tx + sizeof(DbgPing), 0xA5, p->padLen);
  return esp_now_send(mac, g_dbg->tx, frameLen) == ESP_OK;
}

void dbgHandle(const Slot* s) {
  if (s->len < sizeof(DbgPing)) return;
  const DbgPing* p = (const DbgPing*)s->data;
  int c = sizeClass(s->len);
  if (p->h.type == (uint8_t)Msg::DbgPing && !g_leader) {            // client: note the leader, pong back
    memcpy(g_dbg->leader, s->mac, 6);
    g_dbg->haveLeader = true; g_dbg->lrssi = s->rssi; g_dbg->lastRxMs = millis();
    if (g_dbg->lastSeq && p->h.seq > g_dbg->lastSeq + 1) g_dbg->gaps += p->h.seq - g_dbg->lastSeq - 1;
    g_dbg->lastSeq = p->h.seq;
    g_dbg->rxPing[c]++;
    if (g_trace) Serial.printf("[FLT] %lu rx ping %lu len %u rssi %d rf %u\n", millis(), (unsigned long)p->h.seq,
                               (unsigned)s->len, (int)s->rssi, rfChannel());
    if (dbgSend(s->mac, Msg::DbgPong, p->h.seq, s->len)) g_dbg->pongSent[c]++;
  } else if (p->h.type == (uint8_t)Msg::DbgPong && g_leader) {      // leader: per-peer delivery
    DbgPeer* d = nullptr;
    for (auto& q : g_dbg->peers) if (q.used && !memcmp(q.mac, s->mac, 6)) { d = &q; break; }
    if (!d) for (auto& q : g_dbg->peers) if (!q.used) { d = &q; memset(&q, 0, sizeof(q)); memcpy(q.mac, s->mac, 6); q.used = true; break; }
    if (!d) return;                                                // table full: ignore (bench only)
    d->pong[c]++; d->rssi = s->rssi; d->lastMs = millis();
  }
}

void dbgTick() {
  uint32_t now = millis();
  if (g_leader && now - g_dbg->lastPingMs >= 2000) {
    g_dbg->lastPingMs = now;
    uint32_t n = ++g_dbg->seq;
    uint16_t L = g_dbg->padOverride ? (uint16_t)g_dbg->padOverride : ((n & 1) ? 1300 : 250);
    if (L > g_maxPayload) L = 250;
    uint8_t bc[6]; memset(bc, 0xFF, 6);
    bool sent = dbgSend(bc, Msg::DbgPing, n, L);
    if (g_trace) Serial.printf("[FLT] %lu tx ping %lu len %u %s rf %u\n", now, (unsigned long)n, (unsigned)L,
                               sent ? "queued" : "SEND-ERR", rfChannel());
    if (sent) {
      int c = sizeClass(L);
      g_dbg->pingSent[c]++;
      for (auto& q : g_dbg->peers) if (q.used) q.since[c]++;
    }
  }
  if (now - g_dbg->lastPrintMs >= 5000) {
    g_dbg->lastPrintMs = now;
    if (g_leader) {
      Serial.printf("[FLT] L ch=%u rf=%u ping sent 250:%lu 1300:%lu  tx ok=%lu fail=%lu  rx drop=%lu\n", g_ch, rfChannel(),
                    (unsigned long)g_dbg->pingSent[0], (unsigned long)g_dbg->pingSent[1],
                    (unsigned long)g_txOk, (unsigned long)g_txFail, (unsigned long)g_rxDrop);
      for (auto& q : g_dbg->peers) {
        if (!q.used) continue;
        char ms[18]; macStr(q.mac, ms);
        Serial.printf("[FLT]   peer %s pong 250:%lu/%lu 1300:%lu/%lu rssi %d age %lus\n", ms,
                      (unsigned long)q.pong[0], (unsigned long)(q.since[0] + 1),   // +1: the ping that was answered first
                      (unsigned long)q.pong[1], (unsigned long)(q.since[1] + 1), (int)q.rssi,
                      (unsigned long)((now - q.lastMs) / 1000));
      }
    } else {
      char ms[18]; macStr(g_dbg->leader, ms);
      Serial.printf("[FLT] C ch=%u rf=%u leader %s rssi %d age %lus  ping rx 250:%lu 1300:%lu gaps=%lu  pong ok 250:%lu 1300:%lu"
                    "  tx ok=%lu fail=%lu  rx drop=%lu rxf=%lu\n", g_ch, rfChannel(), g_dbg->haveLeader ? ms : "-", (int)g_dbg->lrssi,
                    (unsigned long)(g_dbg->haveLeader ? (now - g_dbg->lastRxMs) / 1000 : 0),
                    (unsigned long)g_dbg->rxPing[0], (unsigned long)g_dbg->rxPing[1], (unsigned long)g_dbg->gaps,
                    (unsigned long)g_dbg->pongSent[0], (unsigned long)g_dbg->pongSent[1],
                    (unsigned long)g_txOk, (unsigned long)g_txFail, (unsigned long)g_rxDrop, (unsigned long)g_rxFrames);
    }
    printHeap(g_leader ? "L" : "C");
  }
}
#endif  // FLEET_DEBUG

// ------------------------------------------------------------------ ESP-NOW callbacks (Wi-Fi task context)
// Copy into the pool and queue the index; nothing else happens here.
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (data && len >= (int)sizeof(Hdr) && data[0] != FLEET_PROTO_VERSION) g_badVer = data[0];
  if (!g_freeQ || !info || !data || len < (int)sizeof(Hdr) || data[0] != FLEET_PROTO_VERSION) { BUMP(g_rxDrop); return; }
  uint8_t i;
  if (xQueueReceive(g_freeQ, &i, 0) != pdTRUE) { BUMP(g_rxDrop); return; }
  Slot* s = slotAt(i);
  const uint16_t cap = slotCap();
  bool trunc = len > cap;
  if (trunc) {
#ifdef FLEET_DEBUG
    bool dbgFrame = data[1] == (uint8_t)Msg::DbgPing || data[1] == (uint8_t)Msg::DbgPong;   // header-only is enough
#else
    bool dbgFrame = false;
#endif
    if (!dbgFrame) { xQueueSend(g_freeQ, &i, 0); BUMP(g_rxDrop); return; }   // oversized: not ours to hold
  }
  memcpy(s->mac, info->src_addr, 6);
  s->rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  s->flags = trunc ? SF_TRUNC : 0;
  s->len = (uint16_t)len;
  memcpy(s->data, data, trunc ? cap : len);
  if (xQueueSend(g_fullQ, &i, 0) != pdTRUE) { xQueueSend(g_freeQ, &i, 0); BUMP(g_rxDrop); return; }
  BUMP(g_rxFrames);
}

// Last few unicast destinations that left the radio (Wi-Fi task writes, loop() reads + consumes; detail::sendDone).
portMUX_TYPE g_sentMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t      g_sentMac[4][6];
bool         g_sentUsed[4] = { false, false, false, false };
uint8_t      g_sentNext = 0;

void onSent(const esp_now_send_info_t* info, esp_now_send_status_t st) {
  if (st == ESP_NOW_SEND_SUCCESS) BUMP(g_txOk); else BUMP(g_txFail);
  if (info && info->des_addr && !(info->des_addr[0] & 1)) {   // unicast only (Probe broadcasts don't matter)
    portENTER_CRITICAL(&g_sentMux);
    memcpy(g_sentMac[g_sentNext], info->des_addr, 6);
    g_sentUsed[g_sentNext] = true;
    g_sentNext = (g_sentNext + 1) % 4;
    portEXIT_CRITICAL(&g_sentMux);
  }
}

// ------------------------------------------------------------------ radio bring-up
void randomMac(uint8_t* m) {
  esp_fill_random(m, 6);
  m[0] = (uint8_t)((m[0] & 0xFC) | 0x02);   // locally administered, unicast
}

// Set the STA MAC. Tries it with the interface up first; if the driver refuses or the read-back differs,
// does the stop -> set -> start sequence. Reports which path worked (docs §3.2: verify in P1).
bool setStaMac(const uint8_t* mac, bool* viaStopStart) {
  uint8_t rb[6];
  if (viaStopStart) *viaStopStart = false;
  esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
  if (e == ESP_OK && esp_wifi_get_mac(WIFI_IF_STA, rb) == ESP_OK && !memcmp(rb, mac, 6)) return true;
  if (viaStopStart) *viaStopStart = true;
  esp_wifi_stop();
  e = esp_wifi_set_mac(WIFI_IF_STA, mac);
  esp_err_t s = esp_wifi_start();
  bool ok = e == ESP_OK && s == ESP_OK && esp_wifi_get_mac(WIFI_IF_STA, rb) == ESP_OK && !memcmp(rb, mac, 6);
  if (!ok) Serial.printf("[FLT] set_mac failed (set=%d start=%d)\n", (int)e, (int)s);
  return ok;
}

void radioTune() {   // after every esp_wifi_start()
  esp_wifi_set_channel(g_ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(FLEET_TXPWR_QDBM);
  if (!hostboard::fleetLeaderCapable()) esp_wifi_set_ps(WIFI_PS_NONE);   // CYD; the S3 keeps the default (BT coex)
}

bool nowInit() {
  esp_err_t e = esp_now_init();
  if (e != ESP_OK) { Serial.printf("[FLT] esp_now_init failed (%d)\n", (int)e); return false; }
  uint32_t v = 1;
  esp_now_get_version(&v);
  g_maxPayload = v >= 2 ? FLEET_V2_MAX : FLEET_V1_MAX;
  esp_now_set_pmk(kPmk);
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  if (g_leader) {   // broadcast peer for Probe (and the debug ping)
    uint8_t bc[6]; memset(bc, 0xFF, 6);
    ensurePeer(bc);
  }
  return true;
}

void freeBuffers() {   // also the partial-failure cleanup: every pointer is checked before use elsewhere
  if (g_freeQ) { vQueueDelete(g_freeQ); g_freeQ = nullptr; }
  if (g_fullQ) { vQueueDelete(g_fullQ); g_fullQ = nullptr; }
  if (g_pool)  { heap_caps_free(g_pool); g_pool = nullptr; }
#ifdef FLEET_DEBUG
  if (g_dbg)   { free(g_dbg); g_dbg = nullptr; }
#endif
}

bool buffersOk() {
  bool ok = g_pool && g_freeQ && g_fullQ;
#ifdef FLEET_DEBUG
  ok = ok && g_dbg;
#endif
  return ok;
}

bool allocBuffers(bool leader) {
  if (buffersOk()) return true;   // kept across a MAC re-randomize
  freeBuffers();                  // drop any partial set from an earlier failed attempt
  if (leader) {
    g_slotSize = (uint16_t)(sizeof(Slot) + FLEET_V2_MAX);
    g_slots = LEADER_SLOTS;
    g_pool = (uint8_t*)heap_caps_malloc((size_t)g_slots * g_slotSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_pool) {   // no PSRAM: a smaller pool in internal RAM
      g_slots = FALLBACK_SLOTS;
      g_pool = (uint8_t*)heap_caps_malloc((size_t)g_slots * g_slotSize, MALLOC_CAP_8BIT);
    }
  } else {
    g_slotSize = CLIENT_SLOT;
    g_slots = CLIENT_SLOTS;
    g_pool = (uint8_t*)heap_caps_malloc((size_t)g_slots * g_slotSize, MALLOC_CAP_8BIT);
  }
  g_freeQ = xQueueCreate(g_slots, sizeof(uint8_t));
  g_fullQ = xQueueCreate(g_slots, sizeof(uint8_t));
#ifdef FLEET_DEBUG
  g_dbg = (Dbg*)calloc(1, sizeof(Dbg));
#endif
  if (!buffersOk()) {
    Serial.println("[FLT] buffer allocation failed");
    freeBuffers();
    return false;
  }
  for (uint8_t i = 0; i < g_slots; i++) xQueueSend(g_freeQ, &i, 0);
  return true;
}

// begin() failed after the radio was started: undo it so a CYD doesn't hold ~51 KB of Wi-Fi for nothing.
bool beginFailed(const char* why) {
  Serial.printf("[FLT] start failed: %s\n", why);
  esp_now_deinit();
  if (!webshare::active()) WiFi.mode(WIFI_OFF);
  freeBuffers();
  g_active = false;
  return false;
}

bool begin(bool leader) {
  if (g_active) return true;
  if (webshare::active()) {   // WiFi.mode(WIFI_STA) below would kill the live SoftAP download
    Serial.println("[FLT] start refused: the SoftAP download is up");
    return false;
  }
  g_leader = leader;
  if (!g_ch || g_ch > 13) g_ch = FLEET_DEFAULT_CH;
  uint32_t heap0 = ESP.getFreeHeap();
  if (!allocBuffers(leader)) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  if (!hostboard::fleetLeaderCapable()) WiFi.setSleep(false);   // CYD: keeps Arduino's STA_START handler from resetting PS to MIN_MODEM
  uint8_t mac[6];
  randomMac(mac);
  bool ss = false;
  if (!setStaMac(mac, &ss)) return beginFailed("set_mac");
  memcpy(g_mac, mac, 6);
  radioTune();
  if (!nowInit()) return beginFailed("esp_now_init");
  g_active = true;
#ifdef FLEET_DEBUG
  static bool evReg = false;
  if (!evReg) { WiFi.onEvent(dbgWifiEvent); evReg = true; }
#endif
  char ms[18]; macStr(g_mac, ms);
  int8_t pw = 0; esp_wifi_get_max_tx_power(&pw);
  Serial.printf("[FLT] %s up: ch %u mac %s (set %s) maxPayload %u txpwr %d.%02d dBm rate %s pool %ux%u (%s)\n",
                leader ? "leader" : "client", g_ch, ms, ss ? "via stop/start" : "direct", g_maxPayload,
                pw / 4, (pw % 4) * 25, g_rate12 ? "12M" : "1M", g_slots, g_slotSize,
                esp_ptr_external_ram(g_pool) ? "psram" : "heap");
  Serial.printf("[FLT] heap before %u, after %u (fleet cost %d B)\n", (unsigned)heap0, (unsigned)ESP.getFreeHeap(),
                (int)heap0 - (int)ESP.getFreeHeap());
  printHeap(leader ? "L" : "C");
  if (leader) fleet::detail::leader::begin(); else fleet::detail::client::begin();
  return true;
}

const char* roleName(uint8_t r) { return r == fleet::ROLE_LEADER ? "leader" : r == fleet::ROLE_CLIENT ? "client" : "off"; }

}  // namespace

// ------------------------------------------------------------------ shared internals (fleet_internal.h)
namespace fleet { namespace detail {

Hmac::Hmac(const uint8_t* key, size_t klen) : ok_(false) {
  mbedtls_md_init(&ctx_);
  ok_ = mbedtls_md_setup(&ctx_, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0 &&
        mbedtls_md_hmac_starts(&ctx_, key, klen) == 0;
}
Hmac::~Hmac() { mbedtls_md_free(&ctx_); }
Hmac& Hmac::add(const void* p, size_t n) {
  if (ok_ && n) ok_ = mbedtls_md_hmac_update(&ctx_, (const uint8_t*)p, n) == 0;
  return *this;
}
Hmac& Hmac::add(const char* s) { return add(s, strlen(s)); }
bool Hmac::finish(uint8_t out[32]) {
  if (ok_) ok_ = mbedtls_md_hmac_finish(&ctx_, out) == 0;
  if (!ok_) esp_fill_random(out, 32);   // never all-zeros: a caller that forgot the check still can't be matched
  return ok_;
}

// FK = HMAC-SHA256(devkey, "CFLO-FLT-KEY-1|" + sid)[0..16) (docs §5.1). The web app derives the same value from the
// key it stores for this sid; the leader only ever holds FK, never the devkey.
bool ownFleetKey(uint8_t out[KEY_LEN]) {
  char msg[32];
  int n = snprintf(msg, sizeof(msg), "CFLO-FLT-KEY-1|%s", devkey::sid());
  uint8_t h[32];
  bool ok = devkey::hmac((const uint8_t*)msg, (size_t)n, h);
  memcpy(out, h, KEY_LEN);
  memset(h, 0, sizeof(h));
  return ok;
}

void randomBytes(void* p, size_t n) { esp_fill_random(p, n); }
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) { return devkey::ctEqual(a, b, n); }

void macStr(const uint8_t* m, char* out) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

bool sidFromHex(const char* hex, uint8_t out[6]) {
  if (!hex || strlen(hex) != 12) return false;
  for (int i = 0; i < 6; i++) {
    unsigned v;
    char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
    if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1]) || sscanf(b, "%x", &v) != 1) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}
void sidToHex(const uint8_t sid[6], char out[13]) {
  snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", sid[0], sid[1], sid[2], sid[3], sid[4], sid[5]);
}

bool peerSet(const uint8_t* mac, const uint8_t* lmk) {
  esp_now_peer_info_t pi = {};
  memcpy(pi.peer_addr, mac, 6);
  pi.channel = 0;                  // current channel
  pi.ifidx = WIFI_IF_STA;
  pi.encrypt = lmk != nullptr;
  if (lmk) memcpy(pi.lmk, lmk, KEY_LEN);
  esp_err_t e = esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&pi) : esp_now_add_peer(&pi);
  memset(pi.lmk, 0, sizeof(pi.lmk));
  if (e != ESP_OK) { Serial.printf("[FLT] peer %s failed (%d)\n", lmk ? "encrypt" : "add", (int)e); return false; }
  applyRate(mac);
  return true;
}
void peerDel(const uint8_t* mac) { if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac); }

bool sendDone(const uint8_t* mac) {
  bool hit = false;
  portENTER_CRITICAL(&g_sentMux);
  for (int i = 0; i < 4; i++)
    if (g_sentUsed[i] && !memcmp(g_sentMac[i], mac, 6)) { g_sentUsed[i] = false; hit = true; }
  portEXIT_CRITICAL(&g_sentMux);
  return hit;
}

bool sendClear(const uint8_t* mac, const void* frame, size_t len) {
  if (!g_active || len > g_maxPayload) return false;
  return esp_now_send(mac, (const uint8_t*)frame, len) == ESP_OK;
}

bool sendSealed(const uint8_t* mac, uint8_t* frame, size_t len, Msg type, uint16_t sess, uint32_t& txSeq,
                const uint8_t sk[KEY_LEN]) {
  if (len < sizeof(Hdr) || len + TAG_LEN > g_maxPayload) return false;
  Hdr* h = (Hdr*)frame;
  h->ver = FLEET_PROTO_VERSION; h->type = (uint8_t)type; h->sess = sess; h->seq = ++txSeq;
  uint8_t t[32];
  if (!Hmac(sk, KEY_LEN).add(frame, len).finish(t)) return false;
  memcpy(frame + len, t, TAG_LEN);
  return sendClear(mac, frame, len + TAG_LEN);
}

bool tagOk(const uint8_t* frame, size_t len, const uint8_t sk[KEY_LEN]) {
  if (len < sizeof(Hdr) + TAG_LEN) return false;
  uint8_t t[32];
  if (!Hmac(sk, KEY_LEN).add(frame, len - TAG_LEN).finish(t)) return false;
  return ctEqual(t, frame + len - TAG_LEN, TAG_LEN);
}

bool SeqWin::accept(uint32_t s) {
  if (s == 0) return false;
  if (s > top) {
    uint32_t d = s - top;
    bits = d >= 32 ? 0 : bits << d;
    bits |= 1;
    top = s;
    return true;
  }
  uint32_t d = top - s;
  if (d >= 32 || (bits & (1u << d))) return false;   // too old, or a duplicate
  bits |= 1u << d;
  return true;
}

void setRadioChannel(uint8_t ch) { if (g_active && ch >= 1 && ch <= 13) esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }
void event(const char* op, const EventInfo& x) { g_events++; if (g_eventSink) g_eventSink(op, x); }
void event(const char* op) { EventInfo x = {}; x.slot = -1; x.n = -1; event(op, x); }
uint32_t eventCount() { return g_events; }

}}  // namespace fleet::detail

namespace fleet {

namespace {
uint8_t  g_known[FLEET_RADIOS][6];
bool     g_knownDirty = false;
uint32_t g_knownDirtyMs = 0;
void knownChanged() { g_knownDirty = true; g_knownDirtyMs = millis(); }
void knownFlush() {
  if (!g_knownDirty || millis() - g_knownDirtyMs < 5000 || logXferActive()) return;   // a rejoin changes it a few times at once
  g_knownDirtyMs = millis();             // a failed write retries in 5 s
  Preferences p;
  if (!p.begin("cydscan", false)) return;
  if (p.putBytes("fltself", g_known, sizeof(g_known)) == sizeof(g_known)) g_knownDirty = false;
  p.end();
}
bool macIsSet(const uint8_t* m) { for (int k = 0; k < 6; k++) if (m[k]) return true; return false; }
void slotPut(int i, const uint8_t* m) {
  if (!m || !macIsSet(m) || !memcmp(g_known[i], m, 6)) return;
  memcpy(g_known[i], m, 6);
  knownChanged();
}
}  // namespace

// Slots are overwritten, never appended: a unit can only ever occupy its own three.
void knownSlotSet(uint8_t unit, const uint8_t* ble, const uint8_t* c5, const uint8_t* ap) {
  if (unit > FLEET_MAX_MEMBERS) return;
  slotPut(unit * 3, ble); slotPut(unit * 3 + 1, c5); slotPut(unit * 3 + 2, ap);
}
void knownSlotRemove(uint8_t unit) {
  if (unit > FLEET_MAX_MEMBERS) return;
  const int at = unit * 3;
  memmove(g_known[at], g_known[at + 3 < FLEET_RADIOS ? at + 3 : at], (size_t)(FLEET_RADIOS - at - 3) * 6);
  memset(g_known[FLEET_RADIOS - 3], 0, 3 * 6);
  knownChanged();
}
void knownRadiosSet(const uint8_t* macs, uint8_t n) {
  if (n != FLEET_RADIOS || !memcmp(g_known, macs, sizeof(g_known))) return;
  memcpy(g_known, macs, sizeof(g_known));
  knownChanged();
}
// Leader start: anything above its enrolled members is left over, e.g. a removal that a reboot beat to NVS.
void knownTrim(uint8_t members) {
  for (int i = 3 * (members + 1); i < FLEET_RADIOS; i++)
    if (macIsSet(g_known[i])) { memset(g_known[i], 0, 6); knownChanged(); }
}
const uint8_t* knownRadios() { return &g_known[0][0]; }
uint8_t knownRadioCount() {
  uint8_t c = 0;
  for (int i = 0; i < FLEET_RADIOS; i++) if (macIsSet(g_known[i])) c++;
  return c;
}

void loadRole() {
  Preferences p;
  if (!p.begin("cydscan", false)) { g_role = ROLE_OFF; return; }
  g_role = p.getUChar("fltrole", ROLE_OFF);
  g_ch = p.getUChar("fltch", FLEET_DEFAULT_CH);
  if (g_ch < 1 || g_ch > 13) g_ch = FLEET_DEFAULT_CH;
  if (g_role > ROLE_LEADER || (g_role == ROLE_LEADER && !hostboard::fleetLeaderCapable())) {   // the CYD refuses leader
    g_role = ROLE_OFF;
    p.putUChar("fltrole", ROLE_OFF);
    p.remove("fltcarry");
  }
  if (g_role != ROLE_OFF) {   // boot-loop guard (docs §1.4): counted only while a fleet role is set
    // fltboot = CONSECUTIVE crash boots: a clean boot resets it, and service() clears it after 60 s of
    // uptime, so a crash later than that never counts (Leave stays reachable).
    uint8_t n = p.getUChar("fltboot", 0);
    esp_reset_reason_t rr = esp_reset_reason();
    bool crash = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                 rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
    uint8_t was = n;
    n = crash ? (n < 255 ? n + 1 : n) : 0;
    if (n >= CRASH_BOOTS) {
      Serial.printf("[FLT] boot-loop guard: %u boots, last reset reason %d -> leaving the fleet\n", n, (int)rr);
      g_fellBackRole = g_role;
      g_role = ROLE_OFF;
      g_fellBack = true;
      p.putUChar("fltrole", ROLE_OFF);
      p.putUChar("fltboot", 0);
      p.remove("fltcarry");     // a leave: the carried log tail must not reach some later, unrelated fleet
    } else {
      if (n != was) p.putUChar("fltboot", n);
      g_bootCounted = n != 0;
    }
  }
  if (g_role == ROLE_OFF) p.remove("fltself");
  else if (p.getBytesLength("fltself") == sizeof(g_known)) p.getBytes("fltself", g_known, sizeof(g_known));
  p.end();
}

uint8_t role()     { return g_role; }
bool    isClient() { return g_role == ROLE_CLIENT; }
bool    fellBack() { bool r = g_fellBack; g_fellBack = false; return r; }
uint8_t fellBackRole() { return g_fellBackRole; }
uint8_t channel()  { return g_ch; }

bool setRole(uint8_t r) {
  if (r > ROLE_LEADER || (r == ROLE_LEADER && !hostboard::fleetLeaderCapable())) return false;
  Preferences p;
  if (!p.begin("cydscan", false)) return false;
  if (r != g_role) p.remove("fltcarry");   // leave / release / role change: drop the carried log tail (fleet_client.cpp)
  if (r != g_role) { p.remove("fltself"); memset(g_known, 0, sizeof(g_known)); g_knownDirty = false; }   // another fleet, or none: the old one's radios are ordinary devices
  bool ok = p.putUChar("fltrole", r) == sizeof(uint8_t);
  p.putUChar("fltboot", 0);
  p.end();
  if (ok) g_role = r;   // keep role() in step with NVS (a `leader on` with no restart runs on this value)
  return ok;
}

bool setChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return false;
  Preferences p;
  if (!p.begin("cydscan", false)) return false;
  bool ok = p.putUChar("fltch", ch) == sizeof(uint8_t);
  p.end();
  if (ok) { g_ch = ch; if (g_active) retune(); }   // applied immediately when the fleet is up
  return ok;
}

void retune() { if (g_active) esp_wifi_set_channel(g_ch, WIFI_SECOND_CHAN_NONE); }

bool beginClient() { return begin(false); }
bool beginLeader() { return begin(true); }
bool active()      { return g_active; }

void service() {
  knownFlush();
  if (g_bootCounted && millis() > STABLE_MS) {   // stable for 60 s: the last boots were not a crash loop
    g_bootCounted = false;
    Preferences p;
    if (p.begin("cydscan", false)) { p.putUChar("fltboot", 0); p.end(); }
  }
  if (!g_active) return;
  if (g_badVer && !g_badVerShown) {           // a partly reflashed fleet: say so once instead of only bumping rxDrop
    g_badVerShown = true;
    Serial.printf("[FLT] ignoring fleet proto v%u frames (this unit speaks v%u): reflash every unit together\n",
                  (unsigned)g_badVer, (unsigned)FLEET_PROTO_VERSION);
  }
  for (int n = 0; n < DRAIN_MAX; n++) {
    uint8_t i;
    if (xQueueReceive(g_fullQ, &i, 0) != pdTRUE) break;
    const Slot* s = slotAt(i);
    const uint8_t type = s->len >= 2 ? s->data[1] : 0;
#ifdef FLEET_DEBUG
    if (type == (uint8_t)Msg::DbgPing || type == (uint8_t)Msg::DbgPong) { dbgHandle(s); xQueueSend(g_freeQ, &i, 0); continue; }
#endif
    if (g_active && !(s->flags & SF_TRUNC) && s->len >= sizeof(Hdr)) {   // (a handler may stop the radio mid-drain)
      Rx f{ s->mac, s->rssi, s->data, s->len };
      if (g_leader) leader::onFrame(f); else client::onFrame(f);
    }
    xQueueSend(g_freeQ, &i, 0);
  }
  if (!g_active) return;   // a frame handler can stop the radio (rerandomizeMac failure)
  if (g_leader) leader::tick(); else client::tick();
#ifdef FLEET_DEBUG
  dbgTick();
#endif
}

bool rerandomizeMac() {
  if (!g_active) return false;
  if (webshare::active()) { Serial.println("[FLT] remac refused: the SoftAP is up"); return false; }
  uint8_t mac[6];
  randomMac(mac);   // before the radio stops: it is the entropy source
  uint32_t t0 = micros();
  esp_now_deinit();
  esp_wifi_stop();
  esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
  esp_err_t s = esp_wifi_start();
  uint8_t rb[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, rb);
  bool ok = e == ESP_OK && s == ESP_OK && !memcmp(rb, mac, 6);
  radioTune();
  bool now = nowInit();
  uint32_t us = micros() - t0;
  if (ok) memcpy(g_mac, mac, 6);
  g_active = now;
  char ms[18]; macStr(g_mac, ms);
  Serial.printf("[FLT] MAC re-randomized in %lu.%03lu ms (%s): %s, ESP-NOW %s\n", (unsigned long)(us / 1000),
                (unsigned long)(us % 1000), ok ? "ok" : "FAILED", ms, now ? "up" : "DOWN");
  return ok && now;
}

const uint8_t* staMac()   { return g_mac; }
uint16_t       maxPayload() { return g_maxPayload; }
// The device key changed, so this unit's fleet key changed too: a client drops its session and re-derives FK (the
// leader must re-add it with the new key). A leader's stored member FKs don't depend on its own key.
void onRekey() {
  detail::client::forgetKey();
  if (g_active && !g_leader) detail::client::dropSession("rekey");
}
void setEventSink(void (*fn)(const char* op, const EventInfo& x)) { g_eventSink = fn; }

bool fleetIdent(char id[17], char leader[13]) {
  id[0] = leader[0] = 0;
  uint8_t fid[8], sid[6];
  if (!g_active || !(g_leader ? detail::leader::ident(fid, sid) : detail::client::ident(fid, sid))) return false;
  for (int i = 0; i < 8; i++) snprintf(id + 2 * i, 3, "%02x", fid[i]);
  for (int i = 0; i < 6; i++) snprintf(leader + 2 * i, 3, "%02x", sid[i]);
  return true;
}
bool gpsIsFleetPosition() { return g_active && !g_leader && detail::client::gpsIsFleetPos(); }
void planString(char* out, size_t cap) {
  using namespace link_protocol;
  const ScanPlan p = currentPlan();
  // What the C5 really runs (c5/main.cpp gates): a phase needs its plan bit (legacy = all) AND every Scan Settings
  // source bit the C5 checks for it. Empty = nothing enabled.
  const uint8_t phases = (p.phases & PLAN_VALID) ? p.phases : 0xFF;
  const uint8_t mask = g_srcMask;
  static const struct { uint8_t bit, need; const char* name; } kPh[] = {
    {PLAN_AP24, MASK_WIFI24, "ap24"}, {PLAN_AP5, MASK_WIFI5, "ap5"}, {PLAN_PROM24, MASK_PROBE, "prom24"},
    {PLAN_PROM5, MASK_PROBE | MASK_WIFI5, "prom5"}, {PLAN_154, MASK_154, "154"}, {PLAN_BLE, MASK_BLE, "ble"}};
  size_t n = 0; out[0] = 0;
  for (const auto& e : kPh) {
    if (!(phases & e.bit) || (mask & e.need) != e.need) continue;
    int w = snprintf(out + n, cap - n, "%s%s", n ? "+" : "", e.name);
    if (w < 0 || (size_t)w >= cap - n) break;
    n += w;
  }
}

bool timeFix(uint32_t* utc, uint32_t* atMs) { return g_active && !g_leader && detail::client::timeFix(utc, atMs); }
bool timeFresh() { return g_active && !g_leader && detail::client::timeFresh(); }
bool posFix(float* lat, float* lon, uint32_t* ageMs, bool* live) {
  *live = false;
  if (!g_active) return false;
  return g_leader ? detail::leader::memberFix(lat, lon, ageMs, live) : detail::client::posFix(lat, lon, ageMs, live);
}
// The §8.3 role templates. A BLE role keeps the active AP scan (it coexists with BLE) but no passive windows, so BLE
// runs at ~100% duty; Wi-Fi roles drop BLE so the passive capture gets all the airtime (longer windows, short gap).
link_protocol::ScanPlan planForRole(uint8_t role, uint8_t gen) {
  link_protocol::ScanPlan p = {};
  using namespace link_protocol;
  switch (role) {
    case PLANROLE_WIFI:   p.phases = PLAN_AP24 | PLAN_AP5 | PLAN_PROM24 | PLAN_PROM5 | PLAN_154;
                          p.prom24Ms = 6000; p.prom5Ms = 5000; p.gapMs = 500; break;
    case PLANROLE_WIFI24: p.phases = PLAN_AP24 | PLAN_PROM24 | PLAN_154;
                          p.prom24Ms = 6000; p.dwellMs = 200; p.gapMs = 500; break;
    case PLANROLE_WIFI5:  p.phases = PLAN_AP5 | PLAN_PROM5;
                          p.prom5Ms = 6000; p.gapMs = 500; break;
    case PLANROLE_BLE:    p.phases = PLAN_BLE | PLAN_AP24 | PLAN_AP5; break;
    default:              return p;                  // FULL = legacy (all-zero)
  }
  p.phases |= PLAN_VALID;
  p.planId = gen;
  return p;
}
const char* planRoleName(uint8_t role) {
  return role == PLANROLE_WIFI ? "wifi" : role == PLANROLE_WIFI24 ? "wifi24" : role == PLANROLE_WIFI5 ? "wifi5"
       : role == PLANROLE_BLE ? "ble" : "full";
}
link_protocol::ScanPlan currentPlan() {
  uint8_t r = PLANROLE_FULL;
  if (!g_active) return link_protocol::ScanPlan{};
  return g_leader ? detail::leader::ownPlan(&r) : detail::client::plan(&r);
}
uint8_t currentPlanRole() {
  uint8_t r = PLANROLE_FULL;
  if (g_active) { if (g_leader) detail::leader::ownPlan(&r); else detail::client::plan(&r); }
  return r;
}

void forwardCycle() { if (g_active && !g_leader) detail::client::forwardCycle(); }
int  mergeRemote()  { return g_active && g_leader ? detail::leader::mergeRemote() : 0; }
bool hasRemote()    { return g_active && g_leader && detail::leader::hasRemote(); }
void alertMax(uint16_t* deauth, uint8_t* evil, uint8_t* rid) {
  *deauth = 0; *evil = 0; *rid = 0;
  if (g_active && g_leader) detail::leader::alertMax(deauth, evil, rid);
}
void fwdStats(FwdStats& s) {
  memset(&s, 0, sizeof(s));
  if (!g_active) return;
  if (g_leader) detail::leader::fwdStats(s); else detail::client::fwdStats(s);
}

bool isFleetSelf(const uint8_t mac[6], uint8_t source) {
  using link_protocol::Source;
  const int kind = source == (uint8_t)Source::BleScan ? 0 : source == (uint8_t)Source::WifiProbe ? 1 :
                   source == (uint8_t)Source::WifiScan ? 2 : -1;
  if (kind < 0 || !macIsSet(mac)) return false;
  for (int i = kind; i < FLEET_RADIOS; i += 3) if (!memcmp(g_known[i], mac, 6)) return true;   // also before the fleet is up
  if (!g_active || kind != 0) return false;                       // phones are BLE
  return g_leader ? detail::leader::isSelf(mac) : detail::client::isSelf(mac);
}

void uiSnapshot(Ui& u) {
  memset(&u, 0, sizeof(u));
  u.active = g_active; u.role = g_role; u.channel = g_ch;
  u.txOk = g_txOk; u.txFail = g_txFail; u.rxFrames = g_rxFrames; u.rxDrop = g_rxDrop;
  if (g_active) { if (g_leader) detail::leader::fillUi(u); else detail::client::fillUi(u); }
  u.planRole = currentPlanRole();
#ifdef FLEET_DEBUG
  if (!u.leaderSeen && g_dbg && g_dbg->haveLeader && millis() - g_dbg->lastRxMs < 6000) {
    u.leaderSeen = true;
    memcpy(u.leaderMac, g_dbg->leader, 6);
    u.rssi = g_dbg->lrssi;
    u.ageMs = millis() - g_dbg->lastRxMs;
  }
#endif
}

void printStatus() {
  char ms[18]; macStr(g_mac, ms);
  Serial.printf("[FLT] role %s  channel %u  active %s  mac %s  maxPayload %u\n", roleName(g_role), g_ch,
                g_active ? "yes" : "no", g_active ? ms : "-", g_maxPayload);
  Serial.printf("[FLT] self filter: %u fixed radios known, %lu rows dropped\n", (unsigned)knownRadioCount(), (unsigned long)g_selfDrops);
  Serial.printf("[FLT] tx ok %lu fail %lu  rx %lu drop %lu  rate %s  boot-guard %s  build %s\n",
                (unsigned long)g_txOk, (unsigned long)g_txFail, (unsigned long)g_rxFrames, (unsigned long)g_rxDrop,
                g_rate12 ? "12M" : "1M", g_bootCounted ? "counting" : "idle",
#ifdef FLEET_DEBUG
                "FLEET_DEBUG (ping/pong on)");
#else
                "release");
#endif
  if (g_active) { if (g_leader) detail::leader::printStatus(); else detail::client::printStatus(); }
  printHeap("now");
}

void dbgSetRate(int mbps) {
#ifdef FLEET_DEBUG
  g_rate12 = mbps == 12;
  g_rateWarned = false;
  if (g_active) {
    uint8_t bc[6]; memset(bc, 0xFF, 6);
    if (esp_now_is_peer_exist(bc)) applyRate(bc);
    if (g_dbg && g_dbg->haveLeader && esp_now_is_peer_exist(g_dbg->leader)) applyRate(g_dbg->leader);
  }
  Serial.printf("[FLT] peer rate now %s\n", g_rate12 ? "12M" : "1M");
#else
  (void)mbps;
#endif
}

void dbgSetTrace(bool on) {
#ifdef FLEET_DEBUG
  g_trace = on;
  Serial.printf("[FLT] ping trace %s\n", on ? "on" : "off");
#else
  (void)on;
#endif
}
void dbgSetPad(int len) {
#ifdef FLEET_DEBUG
  if (g_dbg) g_dbg->padOverride = len;
  Serial.printf("[FLT] ping size %s\n", len ? "fixed" : "alternating 250/1300");
#else
  (void)len;
#endif
}

}  // namespace fleet

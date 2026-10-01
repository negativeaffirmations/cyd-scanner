// ieee154.cpp — see ieee154.h.
#include "ieee154.h"

#include <Arduino.h>
#include <esp_ieee802154.h>
#include <string.h>

namespace {

ieee154::DetCb     g_cb       = nullptr;
QueueHandle_t      g_q        = nullptr;
volatile bool      g_active   = false;
volatile uint32_t  g_dropped  = 0;   // records lost to a full queue (ISR-updated)
uint32_t           g_loggedDrops = 0;

// FCF field layout (little-endian 16-bit).
constexpr uint16_t FCF_TYPE_MASK   = 0x0007;
constexpr uint16_t FCF_PANID_COMP  = 1 << 6;
constexpr uint16_t FCF_SEQ_SUPPRESS= 1 << 8;
constexpr uint16_t FCF_DST_SHIFT   = 10;
constexpr uint16_t FCF_VER_SHIFT   = 12;
constexpr uint16_t FCF_SRC_SHIFT   = 14;
constexpr uint8_t  FTYPE_BEACON    = 0;
constexpr uint8_t  FTYPE_ACK       = 2;
constexpr uint8_t  VER_2015        = 2;

inline int addrLen(uint8_t mode) {
  return mode == ieee154::ADDR_SHORT ? 2 : mode == ieee154::ADDR_EXT ? 8 : 0;
}

}  // namespace

// RX-done callback (ISR context): no mutex, no malloc, no logging. Overrides the
// driver's weak symbol. frame = [len][MHR][payload], FCS stripped.
extern "C" void IRAM_ATTR esp_ieee802154_receive_done(uint8_t* frame,
                                                      esp_ieee802154_frame_info_t* info) {
  const uint8_t len = frame[0];
  const uint8_t* p  = frame + 1;
  ieee154::Record r;
  bool ok = false;

  if (len >= 3 && g_q) {
    uint16_t fcf   = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    uint8_t  ftype = fcf & FCF_TYPE_MASK;
    uint8_t  dm    = (fcf >> FCF_DST_SHIFT) & 3;
    uint8_t  sm    = (fcf >> FCF_SRC_SHIFT) & 3;
    uint8_t  ver   = (fcf >> FCF_VER_SHIFT) & 3;
    bool     comp  = fcf & FCF_PANID_COMP;
    bool     noSeq = (fcf & FCF_SEQ_SUPPRESS) && ver == VER_2015;

    if (ftype != FTYPE_ACK) {
      // PAN ID presence per addressing mode + PAN-ID compression. Legacy (2003/2006)
      // rules are exact; 2015 uses the common cases (see ieee154 notes in the report).
      bool dstPan, srcPan;
      if (ver < VER_2015) {
        dstPan = dm != 0;
        srcPan = sm != 0 && !(dm != 0 && comp);
      } else if (dm != 0 && sm != 0) { dstPan = true;  srcPan = !comp; }
      else if (dm != 0)              { dstPan = !comp; srcPan = false; }
      else if (sm != 0)              { dstPan = false; srcPan = !comp; }
      else                           { dstPan = comp;  srcPan = false; }

      int o = 2 + (noSeq ? 0 : 1);
      uint16_t dpan = ieee154::PAN_NONE, span = ieee154::PAN_NONE;
      int dl = addrLen(dm), sl = addrLen(sm);
      int need = o + (dstPan ? 2 : 0) + dl + (srcPan ? 2 : 0) + sl;
      if (need <= len) {
        if (dstPan) { dpan = (uint16_t)p[o] | ((uint16_t)p[o + 1] << 8); o += 2; }
        o += dl;  // skip dest address
        if (srcPan) { span = (uint16_t)p[o] | ((uint16_t)p[o + 1] << 8); o += 2; }
        memset(r.addr, 0, sizeof(r.addr));
        for (int i = 0; i < sl; i++) r.addr[i] = p[o + i];
        r.channel  = info->channel;
        r.rssi     = info->rssi;
        r.lqi      = info->lqi;
        r.ftype    = ftype;
        r.pan      = (span != ieee154::PAN_NONE) ? span : dpan;
        r.addrMode = sm;
        ok = true;
      }
    }
  }

  if (ok) {
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(g_q, &r, &woken) != pdTRUE) g_dropped = g_dropped + 1;
    if (woken == pdTRUE) portYIELD_FROM_ISR();
  }
  esp_ieee802154_receive_handle_done(frame);  // ALWAYS: only 20 RX buffers
}

namespace ieee154 {

void begin(DetCb cb) {
  g_cb = cb;
  if (!g_q) g_q = xQueueCreate(QUEUE_DEPTH, sizeof(Record));
}

esp_err_t enable() {
  esp_err_t e = esp_ieee802154_enable();
  if (e != ESP_OK) return e;
  g_active = true;
  esp_ieee802154_set_promiscuous(true);      // accept all frames, no PAN/addr filter
  esp_ieee802154_set_rx_when_idle(true);     // stay in RX between frames
  esp_ieee802154_set_channel(CH_MIN);
  return esp_ieee802154_receive();           // receive-only; never transmits
}

void disable() {
  if (!g_active) return;
  if (esp_ieee802154_disable() == ESP_OK) g_active = false;
}

void setChannel(uint8_t ch) {
  if (ch < CH_MIN || ch > CH_MAX) return;
  esp_ieee802154_set_channel(ch);
  esp_ieee802154_receive();  // re-arm RX on the new channel
}

bool active() { return g_active; }

void tick() {
  if (!g_q) return;
  Record r;
  while (xQueueReceive(g_q, &r, 0) == pdTRUE) {
    if (g_cb && (r.addrMode == ADDR_SHORT || r.addrMode == ADDR_EXT)) {
      using namespace link_protocol;
      Detection d{};
      d.source  = (uint8_t)Source::Ieee802154;
      d.channel = r.channel;
      d.rssi    = r.rssi;
      d.panId   = (r.pan == PAN_NONE) ? 0 : r.pan;
      if (r.ftype == FTYPE_BEACON) d.flags |= FLAG_154_BEACON;
      // r.addr is little-endian as on air; store in display (big-endian) order.
      if (r.addrMode == ADDR_EXT) {
        d.flags |= FLAG_154_EXTENDED;
        for (int i = 0; i < 6; i++) d.mac[i] = r.addr[7 - i];  // EUI-64 bytes 0..5 (OUI first)
      } else {
        d.mac[0] = r.addr[1];
        d.mac[1] = r.addr[0];
      }
      g_cb(d);
    }
#ifdef IEEE154_TEST
    char a[20];
    if (r.addrMode == ADDR_SHORT)
      snprintf(a, sizeof(a), "%02X%02X", r.addr[1], r.addr[0]);
    else if (r.addrMode == ADDR_EXT)
      snprintf(a, sizeof(a), "%02X%02X%02X%02X%02X%02X%02X%02X", r.addr[7], r.addr[6],
               r.addr[5], r.addr[4], r.addr[3], r.addr[2], r.addr[1], r.addr[0]);
    else
      strcpy(a, "-");
    if (r.pan == PAN_NONE)
      Serial.printf("[154] ch%u pan=---- src=%s type=%u rssi=%d lqi=%u\n",
                    r.channel, a, r.ftype, r.rssi, r.lqi);
    else
      Serial.printf("[154] ch%u pan=%04X src=%s type=%u rssi=%d lqi=%u\n",
                    r.channel, r.pan, a, r.ftype, r.rssi, r.lqi);
#endif
  }
#ifdef IEEE154_TEST
  uint32_t dr = g_dropped;
  if (dr != g_loggedDrops) {
    Serial.printf("[154] queue overflow: %lu dropped total\n", (unsigned long)dr);
    g_loggedDrops = dr;
  }
#endif
}

}  // namespace ieee154

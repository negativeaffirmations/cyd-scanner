// host_link.cpp — see host_link.h.
#include <Arduino.h>
#include "host_link.h"
#include "host_core.h"
#include "phone.h"

HardwareSerial LinkSerial(1);  // UART1 (pins are supplied by the board via hostlink::begin)
FrameParser    parser;

void hostlink::begin(int rxPin, int txPin) {
  LinkSerial.setRxBufferSize(2048);
  LinkSerial.begin(LINK_BAUD, SERIAL_8N1, rxPin, txPin);
}

void sendFrame(uint8_t type, const void* payload, uint16_t len) {
  uint8_t out[MAX_FRAME];
  uint16_t n = encodeFrame(type, (const uint8_t*)payload, len, out);
  LinkSerial.write(out, n);
  LinkSerial.flush();
}

// Send a Ping and wait briefly for the C5's Pong — used to light the link dot on the
// menu BEFORE any scan is started. Safe when idle: the C5 answers Ping immediately
// (scanning is async and never blocks its link reader).
bool pingC5(uint32_t timeoutMs) {
  while (LinkSerial.available()) LinkSerial.read();
  parser.reset();
  sendFrame((uint8_t)Command::Ping, nullptr, 0);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (LinkSerial.available())
      if (parser.feed(LinkSerial.read()) && parser.type() == (uint8_t)Reply::Pong) return true;
    delay(2);
  }
  return false;
}

// One scan command -> full response, read atomically. The CYD⇄C5 link is STRICTLY synchronous:
// we send ONE StartScan and must read the C5's entire burst through its closing Status(scanning=0)
// before the next cycle can send anything, or the C5 drops the next command (it flushes its RX after
// streaming) and the boards desync. So this does NOT abort mid-transaction on user input -- the C5 is
// a fast non-blocking dumper (~hundreds of ms), so a tap is serviced on the next loop iteration with
// no perceptible lag.
//
// Returns true if a response was received (full burst or at least a Status), false on a MISS. A miss
// is detected fast: the C5 answers within ~12 ms when free, so if NOTHING has arrived by noRespMs we
// bail (the C5 is in its Wi-Fi-scan busy window) instead of burning the whole timeoutMs. Once bytes
// start, we read the full burst up to timeoutMs (which must exceed a worst-case dense-table dump).
// On a miss nothing is transmitted during the window (verified on-wire), so there is no late burst to
// desync the next poll. This does NOT touch g_linkOk -- the caller owns link state (sticky).
bool requestScan(uint32_t noRespMs, uint32_t timeoutMs) {
  g_detCount = 0;
  g_ridStoreCount = 0;
  bool sawStart = false, done = false, anyByte = false;
  uint8_t peer[6]; bool havePeer = phone::peerMac(peer);  // connected phone (dynamic)
  while (LinkSerial.available()) LinkSerial.read();
  parser.reset();
  ScanConfig cfg{};
  cfg.sources  = g_srcMask;
  cfg.dwell_ms = 0;
  sendFrame((uint8_t)Command::StartScan, &cfg, sizeof(cfg));
  uint32_t t0 = millis();
  uint32_t lastByte = millis();
  while (!done && millis() - t0 < timeoutMs) {
    if (!anyByte && millis() - t0 > noRespMs) break;  // no reply at all -> busy miss, bail fast
    bool got = false;
    while (LinkSerial.available()) {
      got = true; anyByte = true; lastByte = millis();
      if (!parser.feed(LinkSerial.read())) continue;
      uint8_t t = parser.type();
      if (t == (uint8_t)Reply::Detection && parser.length() >= sizeof(Detection)) {
        if (g_detCount < MAX_DET) {
          Detection& nd = g_dets[g_detCount];
          memcpy(&nd, parser.payload(), sizeof(Detection));
          bool isPhone = havePeer && nd.source == (uint8_t)Source::BleScan &&
                         memcmp(nd.mac, peer, 6) == 0;
          if (!isSelfDet(nd) && !isPhone) g_detCount++;  // drop our own + the paired phone
        }
      } else if (t == (uint8_t)Reply::RemoteId && parser.length() >= sizeof(RemoteId)) {
        // Non-terminating (like Detection): rides inside the Status(1)..Status(0) burst.
        const RemoteId* r = reinterpret_cast<const RemoteId*>(parser.payload());
        int slot = -1;
        for (int k = 0; k < g_ridStoreCount; k++)
          if (memcmp(g_rids[k].mac, r->mac, 6) == 0) { slot = k; break; }  // replace on MAC match
        if (slot < 0) {
          if (g_ridStoreCount < MAX_RID) slot = g_ridStoreCount++;
          else slot = 0;  // full: overwrite index 0 (store is rebuilt every poll, so rarely hit)
        }
        memcpy(&g_rids[slot], r, sizeof(RemoteId));
      } else if (t == (uint8_t)Reply::Status && parser.length() >= sizeof(Status)) {
        const Status* st = reinterpret_cast<const Status*>(parser.payload());
        g_deauthRecent = st->deauth_recent; g_evilCount = st->evil_count; g_ridCount = st->rid_count;
        g_onbValid = (st->gpsFix != 0) && (st->gpsLat != 0 || st->gpsLon != 0);  // v9 onboard GPS
        g_onbSats  = st->gpsSats;  // sats-in-use, even pre-fix (useful for "is the module seeing sky?")
        if (g_onbValid) { g_onbLat = st->gpsLat / 1e7; g_onbLon = st->gpsLon / 1e7; g_onbAtMs = millis(); }  // divide in double, then store
        if (st->gpsEpoch != 0) { g_gpsEpochUtc = st->gpsEpoch; g_gpsEpochAtMs = millis(); }  // v10: latch GPS UTC clock
        if (st->scanning != 0) sawStart = true;
        else                   done     = true;
      }
    }
    // Idle-gap backstop: burst started but the closing Status was lost -> don't wait the full timeout.
    if ((sawStart || g_detCount > 0) && millis() - lastByte > 250) done = true;
    if (!got) delay(1);  // yield instead of a tight spin
  }
  return done || sawStart || g_detCount > 0;
}

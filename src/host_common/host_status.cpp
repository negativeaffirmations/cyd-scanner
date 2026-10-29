// host_status.cpp — see host_status.h. Code moved verbatim from the CYD main.cpp (docs/mini-build.md §4).
#include <Arduino.h>
#include "host_status.h"
#include "host_core.h"
#include "host_log.h"
#include "host_board.h"
#include "host_ui.h"
#include "phone.h"
#include "webshare.h"
#include "whitelist.h"

void pushStatus() {
  int n24, n5, nble, nprb, n154; countBands(n24, n5, nble, nprb, n154);
  int susp, lk, conf; countTiers(susp, lk, conf);
  char sb[32]; sessBase(activeStream(), sb, sizeof(sb));  // sess= tracks the stream "current" downloads
  // STATUS goes out as TWO notifications. A notify carries at most MTU-3 bytes (NimBLE's ~255 MTU ->
  // ~252 B) and the one-line form outgrew that once name= was added (~273 B): the tail -- name=, and the
  // Wi-Fi AP credentials while a download AP is up -- was silently cut off. The web app handles each
  // notification's keys on their own and merges them, so splitting is backward compatible; keys that
  // are read together (dl + apssid/appsk/apurl) stay in the same part.
  //   part 1: live counts / state (~215 B)   part 2: dl + gav/tav/tpref/gpref + identity + pos/pfa (+ AP creds) (<= ~200 B)
  char s[300];
  snprintf(s, sizeof(s),
           "link=%d;w24=%d;w5=%d;ble=%d;prb=%d;z=%d;uniq=%d;time=%d;gps=%d;"
           "susp=%d;lk=%d;conf=%d;db=%d;scan=%d;bri=%d;src=%d;wl=%d;muted=%d;fol=%d;sess=%s;bg=%d;deauth=%d;evil=%d;rid=%d;lock=%d;bat=%d;gpssrc=%d;sats=%d;tsrc=%d",
           g_linkOk ? 1 : 0, n24, n5, nble, nprb, n154, (int)activeStream().uniqCount,
           timeHasSync() ? 1 : 0, posLive() ? 1 : 0,  // gps = LIVE fix; a held one shows as gpssrc=3
           susp, lk, conf, sigdb::loaded() ? 1 : 0,
           g_scanActive ? 1 : 0, (int)hostboard::brightness(), (int)g_srcMask,
           whitelist::count(), countMuted(), followAlertN(), sb, g_bgScan ? 1 : 0,
           (int)g_deauthRecent, (int)g_evilCount, (int)g_ridCount, g_touchLocked ? 1 : 0,
           hostboard::batteryPercent(), posSource(), (int)g_onbSats, timeSource());
  // gav/tav = per-source availability (bit0 phone, bit1 device) for the web status dots. In part 2:
  // part 1's worst case already sits near the notify limit.
  char s2[300];
  // led = status-LED brightness % (-1 = no dimmable LED: the web app hides its slider).
  int n2 = snprintf(s2, sizeof(s2), "dl=%d;gav=%d;tav=%d;tpref=%d;gpref=%d;led=%d;sid=%s;fw=%s;dbgen=%lu;name=%s",
                    webshare::active() ? 1 : 0,
                    (int)posAvailMask(), (int)timeAvailMask(), (int)g_timePref, (int)g_gpsPref,
                    hostboard::ledBrightness(),
                    scannerIdHex(), FW_VERSION, (unsigned long)DB_GEN, scannerName());
  // pos = the position the scanner is stamping on its log right now (same source order as the file
  // log; empty with no fix), pfa = the fix_age the log would carry (-1 = live, no marker). The web app
  // stamps live-POST rows with these, so they match the file log instead of the phone's last fix.
  // Always sent (pfa=-1 too): the web app merges keys across notifications, so a stale pfa would stick.
  if (n2 > 0 && n2 < (int)sizeof(s2)) {
    PosFix f = posSnapshot();
    if (f.src && isfinite(f.lat) && isfinite(f.lon))
      n2 += snprintf(s2 + n2, sizeof(s2) - n2, ";pos=%.6f,%.6f;pfa=%ld", f.lat, f.lon,
                     f.aged ? (long)(f.ageMs / 1000) : -1L);
    else
      n2 += snprintf(s2 + n2, sizeof(s2) - n2, ";pos=;pfa=-1");
  }
  if (webshare::active() && n2 > 0 && n2 < (int)sizeof(s2))  // download AP up: the phone draws the join QR from these
    snprintf(s2 + n2, sizeof(s2) - n2, ";apssid=%s;appsk=%s;apurl=%s",
             webshare::ssid(), webshare::apPass(), webshare::url());
  static bool warned = false;
  if (!warned && (strlen(s) > 240 || strlen(s2) > 240)) {
    warned = true;
    Serial.printf("[CYD] WARNING: STATUS part is %u/%u B -- near the ~252 B notify limit\n",
                  (unsigned)strlen(s), (unsigned)strlen(s2));
  }
  phone::setStatus(String(s));
  phone::setStatus(String(s2));
  hostui::onStatusPushed();
}

// Stream the live detection list to the phone so its app mirrors the CYD screen.
// DETS stream v4 = "seq-tagged atomic snapshot": a "D:<seq>:<groups>" header, then one
// row per device (one MAC, merged across sources), top-of-list first:
//   <seq>\t<tier>\t<mac>\t<bestRssi>\t<ie>\t<name>\t<tag:rssi,tag:rssi,...>\t<ftier>\t<fscore>\t<muted>
//     [\t<flags>\t<cat>\t<cidLabel>]
// where <ftier> is the follow tier (0 none / 1 PERSISTENT / 2 FOLLOWING), <fscore> the
// 0..100 follow score, and <muted> is 1 when the device matches a whitelist rule. These
// three trailing fields were appended in v3; older web parsers that read by fixed index
// (fields 0..6) ignore them, so the change is backward-compatible. v4 appends ONE more trailing
// field, <flags>: the decimal DetFlags bits (FLAGS_NOTABLE only: iBeacon 8 / Find My 16 /
// Pwnagotchi 32 / Evil Twin 64 / Drone RID 128) OR'd across the device's members, 0 if none.
// Older parsers read fields 0..9 and ignore it.
// v7 appends ONE more trailing per-row field, <cidLabel>: the BLE company maker name
// (sigdb::cidName of the device's company ID; empty when unknown/none) — shown on the web live list
// IN ADDITION to the MAC, so a rotating-random-address device is still identifiable by its maker.
// Older parsers read fields 0..10 (through <cat>) and ignore it.
// v8 appends THREE more trailing per-row fields for the live-POST rows (scan-analyzer contract
// "Live-POST v-next", 2026-10-29): <cid> the raw company ID as 4 hex (empty = none), <uuid> the
// primary BLE service UUID — 4-hex short form when it is a Bluetooth-base 16-bit UUID, else 32 hex
// (empty = none) — and <sig> the strongest-scoring member's detLabel (the file log's "sig").
// Worst-case row stays under the ~252 B notify cap (sig <= 19 chars, LABEL_LEN).
// v6 appends a trailing RID block AFTER the n detection rows, only while a Remote-ID emitter is
// held: an "R:<ridCount>" header then ridCount rows of 11 tab-separated fields (always 11 columns,
// unknown = empty): mac uas dlat dlon olat olon h alt spd hdg oid (lat/lon %.6f deg, h/alt m,
// spd m/s, hdg deg; spd+hdg are both empty when heading is unknown). Older parsers read exactly n
// rows then wait for the next "D:" header, so the appended block does not affect them.
// The app drops rows whose seq != the current header's and swaps the list in only when
// the snapshot is complete. Fallbacks considered and held in reserve if this proves
// lossy: (a) a length-prefixed blob like the log download, (b) a polled READ
// characteristic. Capped so the burst stays small on the BLE link.
static constexpr int DETS_STREAM_MAX = 12;
void pushDetections() {
  if (!phone::authed()) return;
  if (!g_scanActive) return;  // only stream while scanning
  static uint16_t g_detsSeq = 0;
  int n = min(g_groupCount, DETS_STREAM_MAX);
  g_detsSeq++;
  char hdr[24];
  snprintf(hdr, sizeof(hdr), "D:%u:%d", (unsigned)g_detsSeq, n);
  phone::detsNotify(String(hdr));
  for (int r = 0; r < n; r++) {
    const DevGroup& g = g_groups[r];
    char name[24];
    sanitizeField(name, sizeof(name), groupName(&g));
    char row[256];
    int len = snprintf(row, sizeof(row), "%u\t%d\t%02X:%02X:%02X:%02X:%02X:%02X\t%d\t%08lX\t%s\t",
                       (unsigned)g_detsSeq, g.tier,
                       g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4], g.mac[5],
                       g.bestRssi, (unsigned long)g.ie, name);
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    len += snprintf(row + len, sizeof(row) - len, "%s", g.srcs);  // list pre-built (de-duped) in buildGroups
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    // v3 trailing fields: follow tier / follow score / muted (whitelisted) — all CYD-computed.
    // follow tier/score forced to 0 while the follower-alerts feature is disabled (FOLLOW_ALERTS_ENABLED).
    const FollowState* f = FOLLOW_ALERTS_ENABLED ? findFollow(g.mac) : nullptr;
    len += snprintf(row + len, sizeof(row) - len, "\t%d\t%d\t%d",
                    f ? f->ftier : 0, f ? f->score : 0, g.whitelisted ? 1 : 0);
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    len += snprintf(row + len, sizeof(row) - len, "\t%u", (unsigned)g.flags);  // v4 trailing: notable flag bits
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    len += snprintf(row + len, sizeof(row) - len, "\t%u", (unsigned)groupCategory(g));  // v5 trailing: category id
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    // v7 trailing: BLE company maker label (empty if unknown/none), shown on the web live list.
    char mk[24]; sanitizeField(mk, sizeof(mk), sigdb::cidName(g.companyId));
    len += snprintf(row + len, sizeof(row) - len, "\t%s", mk);
    if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
    // v8 trailing: raw cid / primary uuid / sig. g_dets is still this cycle's table (buildGroups ran
    // just before), so scan the members: first non-zero svc, strongest-scoring member's label.
    const uint8_t* svc = nullptr; int best = -1;
    for (int i = 0; i < g_detCount; i++) {
      if (memcmp(g_dets[i].mac, g.mac, 6) != 0) continue;
      if (!svc) for (int j = 0; j < 16; j++) if (g_dets[i].svc[j]) { svc = g_dets[i].svc; break; }
      if (best < 0 || g_score[i].score > g_score[best].score) best = i;
    }
    char cid[5] = "";
    if (g.companyId) snprintf(cid, sizeof(cid), "%04X", g.companyId);
    char uuid[33] = "";
    if (svc) {
      static const uint8_t kBase[12] = {0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x80,0x5F,0x9B,0x34,0xFB};
      if (svc[0] == 0 && svc[1] == 0 && memcmp(svc + 4, kBase, 12) == 0)
        snprintf(uuid, sizeof(uuid), "%02X%02X", svc[2], svc[3]);
      else
        for (int j = 0; j < 16; j++) sprintf(uuid + j * 2, "%02X", svc[j]);
    }
    char sig[24] = "";  // tab/control-safe only: keep commas so it equals the file log's sig
    if (best >= 0) {
      const char* l = detLabel(best);
      size_t k = 0;
      for (; l && l[k] && k + 1 < sizeof(sig); k++) sig[k] = ((uint8_t)l[k] < 0x20 || l[k] == 0x7F) ? ' ' : l[k];
      sig[k] = 0;
    }
    snprintf(row + len, sizeof(row) - len, "\t%s\t%s\t%s", cid, uuid, sig);
    phone::detsNotify(String(row));
    delay(6);  // let the BLE stack drain each notification
  }

  // v6 trailing RID block (only while a Remote-ID emitter is held): "R:<n>" then n 11-field rows.
  if (g_ridStoreCount >= 1) {
    int ridCount = min(g_ridStoreCount, MAX_RID);
    char ridHdr[16];
    snprintf(ridHdr, sizeof(ridHdr), "R:%d", ridCount);
    phone::detsNotify(String(ridHdr));
    for (int i = 0; i < ridCount; i++) {
      const RemoteId& r = g_rids[i];
      char uas[24], oid[24];
      sanitizeField(uas, sizeof(uas), r.uasId);
      sanitizeField(oid, sizeof(oid), r.operatorId);
      char row[256];
      int len = snprintf(row, sizeof(row), "%02X:%02X:%02X:%02X:%02X:%02X\t%s\t",
                         r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5], uas);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      // Each unknown field is emitted EMPTY (separators always present -> always 11 columns).
      if (r.droneLat != RID_LL_UNK) len += snprintf(row + len, sizeof(row) - len, "%.6f", r.droneLat / 1e7);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      len += snprintf(row + len, sizeof(row) - len, "\t");
      if (r.droneLon != RID_LL_UNK) len += snprintf(row + len, sizeof(row) - len, "%.6f", r.droneLon / 1e7);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      len += snprintf(row + len, sizeof(row) - len, "\t");
      if (r.operatorLat != RID_LL_UNK) len += snprintf(row + len, sizeof(row) - len, "%.6f", r.operatorLat / 1e7);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      len += snprintf(row + len, sizeof(row) - len, "\t");
      if (r.operatorLon != RID_LL_UNK) len += snprintf(row + len, sizeof(row) - len, "%.6f", r.operatorLon / 1e7);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      len += snprintf(row + len, sizeof(row) - len, "\t");
      if (r.heightM != RID_ALT_UNK) len += snprintf(row + len, sizeof(row) - len, "%d", (int)r.heightM);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      len += snprintf(row + len, sizeof(row) - len, "\t");
      if (r.altGeoM != RID_ALT_UNK) len += snprintf(row + len, sizeof(row) - len, "%d", (int)r.altGeoM);
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      len += snprintf(row + len, sizeof(row) - len, "\t");
      if (r.headingDeg != RID_HDG_UNK)  // speed + heading are gated together
        len += snprintf(row + len, sizeof(row) - len, "%.1f\t%u\t", r.speedCms / 100.0, (unsigned)r.headingDeg);
      else
        len += snprintf(row + len, sizeof(row) - len, "\t\t");
      if (len < 0 || len >= (int)sizeof(row)) len = sizeof(row) - 1;
      snprintf(row + len, sizeof(row) - len, "%s", oid);
      phone::detsNotify(String(row));
      delay(6);
    }
  }
}

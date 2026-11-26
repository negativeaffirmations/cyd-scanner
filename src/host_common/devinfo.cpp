// devinfo.cpp — see devinfo.h.
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include "devinfo.h"
#include "host_core.h"
#include "phone.h"
#include "whitelist.h"

namespace devinfo {

static Entry* s_ents = nullptr;
static bool   s_on = true;

void load() {
  Preferences p;
  if (p.begin("cydui", true)) { s_on = p.getUChar("gattrd", 1) != 0; p.end(); }
}
bool enabled() { return s_on; }
void setEnabled(bool on) {
  if (on == s_on) return;
  s_on = on;
  Preferences p;
  if (p.begin("cydui", false)) { p.putUChar("gattrd", on ? 1 : 0); p.end(); }
}

static void term(char* s, size_t cap) { s[cap - 1] = 0; }

uint32_t fieldHash(const DevInfo& di) {
  uint32_t h = 2166136261u;
  auto mix = [&](uint8_t b) { h = (h ^ b) * 16777619u; };
  mix(di.have); mix(di.tried); mix(di.addrType); mix((uint8_t)di.appearance); mix((uint8_t)(di.appearance >> 8));
  for (const char* s : {di.name, di.mfr, di.model, di.fw, di.hw}) { for (; *s; s++) mix((uint8_t)*s); mix(0); }
  return h;
}

void ingest(const DevInfo& in) {
  DevInfo d = in;
  term(d.name, sizeof(d.name)); term(d.mfr, sizeof(d.mfr)); term(d.model, sizeof(d.model)); term(d.fw, sizeof(d.fw)); term(d.hw, sizeof(d.hw));
  Detection self{};                                    // our own radios / the paired phone: never keep a result for them
  self.source = (uint8_t)Source::BleScan;
  memcpy(self.mac, d.mac, 6);
  const uint32_t drops = g_selfDrops;                  // isSelfDet() counts a drop; this check isn't one of the table's
  const bool isSelf = isSelfDet(self);
  g_selfDrops = drops;
  if (isSelf) return;
  uint8_t peer[6];
  if (phone::peerMac(peer) && !memcmp(peer, d.mac, 6)) return;
  static const uint8_t zeroSvc[16] = {0};
  if (whitelist::match(d.mac, d.name, 0, zeroSvc, (uint8_t)(1u << (uint8_t)Source::BleScan))) return;
  if (!s_ents) {
    s_ents = (Entry*)calloc(HOST_MAX_DEVINFO, sizeof(Entry));
    if (!s_ents) return;
  }
  int slot = -1, oldest = 0;
  for (int i = 0; i < HOST_MAX_DEVINFO; i++) {
    if (!s_ents[i].used) { if (slot < 0) slot = i; continue; }
    if (!memcmp(s_ents[i].di.mac, d.mac, 6)) { slot = i; break; }
    if (!s_ents[oldest].used || (int32_t)(s_ents[i].heardMs - s_ents[oldest].heardMs) < 0) oldest = i;
  }
  const bool isNew = slot < 0 || !s_ents[slot].used || memcmp(s_ents[slot].di.mac, d.mac, 6) != 0;
  if (slot < 0) slot = oldest;
  Entry& e = s_ents[slot];
  if (isNew) memset(&e, 0, sizeof(e));
  if (isNew || fieldHash(e.di) != fieldHash(d)) e.readMs = millis();   // the C5 re-sends a cached read: keep its time
  e.di = d;
  e.heardMs = millis();
  e.used = true;
  e.pending = true;
}

const DevInfo* find(const uint8_t mac[6]) {
  if (!s_ents) return nullptr;
  for (int i = 0; i < HOST_MAX_DEVINFO; i++)
    if (s_ents[i].used && !memcmp(s_ents[i].di.mac, mac, 6)) return &s_ents[i].di;
  return nullptr;
}
int    capacity() { return s_ents ? HOST_MAX_DEVINFO : 0; }
Entry* entryAt(int i) { return (s_ents && i >= 0 && i < HOST_MAX_DEVINFO && s_ents[i].used) ? &s_ents[i] : nullptr; }

const char* nameOf(const DevInfo& di, char* buf, size_t cap) {
  if ((di.have & DI_NAME) && di.name[0]) return di.name;
  if ((di.have & DI_MODEL) && di.model[0]) {
    if ((di.have & DI_MFR) && di.mfr[0]) snprintf(buf, cap, "%.14s %.16s", di.mfr, di.model);
    else snprintf(buf, cap, "%.31s", di.model);
    return buf;
  }
  return "";
}

// Skip list: private addresses (RPA, top two bits 01: our own advert address, the phone, most personal devices) are left
// out, because the C5 never connects to those. Whitelisted BLE devices then rotate through the free slots, so every one is
// named within a few polls even when there are more than fit (the C5 keeps each skip for 10 min). At most
// WL_SLOTS of the 10 go to the whitelist, the rest to priority devices.
void fillHint(GattHint& h) {
  memset(&h, 0, sizeof(h));
  h.flags = GH_VALID | (s_on ? GH_ENABLE : 0);       // rssiFloor 0 = the C5's default
  constexpr uint8_t BLE_BIT = 1u << 2;               // srcTypeOf(): 0=2.4, 1=5G, 2=BLE
  constexpr int WL_SLOTS = 8;
  int n = 0;
  auto add = [&](const uint8_t* m) {
    if ((m[0] & 0xC0) == 0x40) return;               // RPA
    for (int i = 0; i < n; i++) if (!memcmp(h.mac[i], m, 6)) return;
    if (n < GATT_HINT_MAX) memcpy(h.mac[n++], m, 6);
  };
  int wl[MAX_DET], nwl = 0;                          // whitelisted BLE groups, stable order (by MAC)
  for (int k = 0; k < g_groupCount; k++)
    if (g_groups[k].whitelisted && (g_groups[k].srcMask & BLE_BIT) && (g_groups[k].mac[0] & 0xC0) != 0x40) wl[nwl++] = k;
  std::sort(wl, wl + nwl, [](int a, int b) { return memcmp(g_groups[a].mac, g_groups[b].mac, 6) < 0; });
  static uint16_t rot = 0;
  const int take = nwl < WL_SLOTS ? nwl : WL_SLOTS;
  for (int i = 0; i < take; i++) add(g_groups[wl[(rot + i) % nwl]].mac);
  if (nwl > WL_SLOTS) rot = (uint16_t)((rot + take) % nwl); else rot = 0;
  h.nSkip = (uint8_t)n;
  for (int k = 0; k < g_groupCount && n < GATT_HINT_MAX; k++)   // groups are sorted tier-first
    if (!g_groups[k].whitelisted && (g_groups[k].srcMask & BLE_BIT) && g_groups[k].tier >= (int)sigdb::Tier::Suspect) add(g_groups[k].mac);
  h.nPrio = (uint8_t)(n - h.nSkip);
}

}  // namespace devinfo

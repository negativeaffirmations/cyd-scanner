// devkey.cpp — see devkey.h. NVS "cydscan": "blekey" (16 B blob) + "blekeygen" (u32). Same persist-once
// pattern as webshare.cpp loadOrCreatePsk(); a normal reflash keeps NVS, erase_flash makes the sticker stale.
#include "devkey.h"
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include "host_log.h"  // scannerIdHex()

namespace {

constexpr size_t KEY_LEN = 16;
uint8_t  g_key[KEY_LEN] = {0};
uint32_t g_gen = 0;
char     g_hex[KEY_LEN * 2 + 1] = {0};
char     g_url[160];
bool     g_saved = false;   // the key in use is stored in NVS
// begin()/rekey() run on loop(); hmac() runs on the NimBLE task. Keep key + hex + gen consistent.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

void setHex() {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < KEY_LEN; i++) {
    g_hex[2 * i]     = kHex[g_key[i] >> 4];
    g_hex[2 * i + 1] = kHex[g_key[i] & 15];
  }
  g_hex[KEY_LEN * 2] = 0;
}

void generate(uint32_t gen) {
  uint8_t k[KEY_LEN];
  esp_fill_random(k, sizeof(k));
  portENTER_CRITICAL(&g_mux);
  memcpy(g_key, k, sizeof(k));
  g_gen = gen;
  setHex();
  portEXIT_CRITICAL(&g_mux);
  memset(k, 0, sizeof(k));
}

bool store() {
  Preferences p;
  bool ok = false;
  if (p.begin("cydscan", false)) {  // if NVS is unavailable the key lasts this boot only
    ok = p.putBytes("blekey", g_key, KEY_LEN) == KEY_LEN;
    ok = (p.putUInt("blekeygen", g_gen) == sizeof(uint32_t)) && ok;
    p.end();
  }
  g_saved = ok;
  return ok;
}

}  // namespace

namespace devkey {

void begin() {
  Preferences p;
  bool open = p.begin("cydscan", false);
  uint8_t k[KEY_LEN];
  if (open && p.getBytesLength("blekey") == KEY_LEN && p.getBytes("blekey", k, KEY_LEN) == KEY_LEN) {
    uint32_t gen = p.getUInt("blekeygen", 1);
    p.end();
    portENTER_CRITICAL(&g_mux);
    memcpy(g_key, k, KEY_LEN);
    g_gen = gen ? gen : 1;
    setHex();
    portEXIT_CRITICAL(&g_mux);
    memset(k, 0, sizeof(k));
    g_saved = true;
    return;
  }
  if (open) p.end();
  generate(1);
  if (!store()) Serial.println("[devkey] WARNING: new key NOT persisted (NVS write failed)");
  else Serial.println("[devkey] generated a new phone key");
}

const char* keyHex()   { return g_hex; }
const char* sid()      { return scannerIdHex(); }
const char* unitName() { return scannerName(); }
const char* fwVersion(){ return FW_VERSION; }
uint32_t    gen()    { return g_gen; }

const char* pairUrl() {
  snprintf(g_url, sizeof(g_url), "%s#devkey=%s&sid=%s&kg=%lu", APP_URL, g_hex, scannerIdHex(), (unsigned long)g_gen);
  return g_url;
}

bool rekey() {
  generate(g_gen + 1);
  bool ok = store();
  Serial.printf("[devkey] re-keyed (generation %lu)%s\n", (unsigned long)g_gen,
                ok ? "" : " -- rekey NOT persisted (NVS write failed)");
  logPhoneCfg("rekey");   // audit line; the key is replaced even when there is no SD card
  return ok;
}
bool persisted() { return g_saved; }

bool hmac(const uint8_t* msg, size_t len, uint8_t out[32]) {
  memset(out, 0, 32);
  uint8_t k[KEY_LEN];
  portENTER_CRITICAL(&g_mux);
  memcpy(k, g_key, KEY_LEN);
  portEXIT_CRITICAL(&g_mux);
  int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), k, KEY_LEN, msg, len, out);
  memset(k, 0, sizeof(k));
  if (rc != 0) { memset(out, 0, 32); return false; }
  return true;
}

bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

}  // namespace devkey

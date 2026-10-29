// host_serial.cpp — see host_serial.h.
#include <Arduino.h>
#include "host_serial.h"
#include "devkey.h"
#include "host_log.h"  // scannerIdHex(), scannerName(), FW_VERSION
#include "phone.h"

namespace {

constexpr uint32_t REKEY_CONFIRM_MS = 15000;
char     g_line[48];
size_t   g_len = 0;
bool     g_overflow = false;     // line too long: discard it up to the newline
uint32_t g_rekeyArmedAt = 0;     // millis() of `rekey` (0 = not armed)

void help() {
  Serial.println("[cmd] help               this list");
  Serial.println("[cmd] key                show name, sid, key generation, key and the pairing link");
  Serial.println("[cmd] rekey              rotate the phone key (then `rekey confirm` within 15 s)");
}

void handle(char* cmd) {
  // trim + lowercase in place
  while (*cmd == ' ') cmd++;
  size_t n = strlen(cmd);
  while (n && cmd[n - 1] == ' ') cmd[--n] = 0;
  for (char* c = cmd; *c; c++) *c = (char)tolower((unsigned char)*c);
  if (!*cmd) return;

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { help(); return; }
  if (!strcmp(cmd, "key")) {
    Serial.printf("[key] name %s\n", scannerName());
    Serial.printf("[key] sid  %s\n", scannerIdHex());
    Serial.printf("[key] gen  %lu\n", (unsigned long)devkey::gen());
    Serial.printf("[key] key  %s\n", devkey::keyHex());
    Serial.printf("[key] link %s\n", devkey::pairUrl());
    return;
  }
  if (!strcmp(cmd, "rekey")) {
    g_rekeyArmedAt = millis() | 1;   // |1: never 0
    Serial.println("[key] this invalidates the current key and disconnects the phone.");
    Serial.println("[key] type `rekey confirm` within 15 s to proceed.");
    return;
  }
  if (!strcmp(cmd, "rekey confirm")) {
    if (!g_rekeyArmedAt || millis() - g_rekeyArmedAt > REKEY_CONFIRM_MS) {
      g_rekeyArmedAt = 0;
      Serial.println("[key] not armed -- type `rekey` first");
      return;
    }
    g_rekeyArmedAt = 0;
    bool saved = phone::rekeyAndKick();
    Serial.printf("[key] re-keyed: generation %lu. Type `key` for the new key.\n", (unsigned long)devkey::gen());
    if (!saved) Serial.println("[key] WARNING: rekey NOT persisted (NVS write failed) -- the old key returns after a reboot");
    return;
  }
  Serial.println("[cmd] unknown command (try `help`)");
}

}  // namespace

void hostserial::service() {
  while (Serial.available()) {
    int ch = Serial.read();
    if (ch < 0) break;
    if (ch == '\r' || ch == '\n') {
      if (!g_overflow && g_len) { g_line[g_len] = 0; handle(g_line); }
      g_len = 0; g_overflow = false;
    } else if (g_len < sizeof(g_line) - 1) {
      g_line[g_len++] = (char)ch;
    } else {
      g_overflow = true;
    }
  }
}

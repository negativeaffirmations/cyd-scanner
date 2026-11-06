// host_serial.cpp — see host_serial.h.
#include <Arduino.h>
#include "host_serial.h"
#include "devkey.h"
#include "host_log.h"  // scannerIdHex(), scannerName(), FW_VERSION
#include "phone.h"
#include "fleet.h"
#include "host_board.h"

namespace {

constexpr uint32_t REKEY_CONFIRM_MS = 15000;
#ifdef FLEET_DEBUG
char     g_line[176];            // bench: `fleet add <sid> <fk> <name>` / `fleet m a:...` enrollment lines
#else
char     g_line[48];
#endif
size_t   g_len = 0;
bool     g_overflow = false;     // line too long: discard it up to the newline
uint32_t g_rekeyArmedAt = 0;     // millis() of `rekey` (0 = not armed)
uint32_t g_bondsArmedAt = 0;     // millis() of `bonds clear` (0 = not armed)

void help() {
  Serial.println("[cmd] help               this list");
  Serial.println("[cmd] key                show name, sid, key generation, key and the pairing link");
  Serial.println("[cmd] rekey              rotate the phone key (then `rekey confirm` within 15 s)");
  Serial.println("[cmd] bonds              phone-link security level, stored bonds, link state");
  Serial.println("[cmd] bonds clear        forget all paired phones (then `bonds confirm` within 15 s)");
  Serial.println("[cmd] fleet              fleet status; also: fleet leave | client | leader on|off | ch <1-13> | remac");
}

// Fleet mode (docs/fleet-mode.md §1.3): `fleet [leave|client|leader on|off|ch <1-13>|remac]`.
// Role changes persist to NVS and restart (the CYD's Bluetooth memory release is boot-time only).
void fleetRestart(const char* why) {
  Serial.printf("[fleet] %s -- restarting\n", why);
  Serial.flush();
  delay(300);
  ESP.restart();
}

void fleetCmd(const char* arg) {
  if (!*arg) { fleet::printStatus(); return; }
  if (!strcmp(arg, "leave")) {
    if (fleet::role() == fleet::ROLE_OFF) { Serial.println("[fleet] not in a fleet"); return; }
    logFleetEvent("leave");
    if (!fleet::setRole(fleet::ROLE_OFF)) { Serial.println("[fleet] NVS write failed"); return; }
    fleetRestart("left the fleet");
  } else if (!strcmp(arg, "client")) {
    if (!fleet::setRole(fleet::ROLE_CLIENT)) { Serial.println("[fleet] NVS write failed"); return; }
    fleetRestart("fleet client set");
  } else if (!strcmp(arg, "leader on") || !strcmp(arg, "leader off")) {
    if (!hostboard::fleetLeaderCapable()) { Serial.println("[fleet] this board can only be a fleet client"); return; }
    if (!strcmp(arg, "leader on")) {
      const uint8_t was = fleet::role();   // read before setRole() overwrites it
      if (!fleet::setRole(fleet::ROLE_LEADER)) { Serial.println("[fleet] NVS write failed"); return; }
      if (fleet::active() && was != fleet::ROLE_LEADER) fleetRestart("leader set (was running as a client)");
      else if (fleet::active()) Serial.println("[fleet] already running");
      else {
        const bool up = fleet::beginLeader();
        Serial.println(up ? "[fleet] leader up" : "[fleet] leader start FAILED");
        logFleetEvent(up ? "start" : "start_failed");   // the same anchor the boot / phone paths write
      }
    } else {
      logFleetEvent("leave");
      fleet::setRole(fleet::ROLE_OFF);
      fleetRestart("leader off");
    }
  } else if (!strncmp(arg, "ch ", 3)) {
    int ch = atoi(arg + 3);
    if (ch >= 1 && ch <= 13 && fleet::setChannel((uint8_t)ch))
      Serial.printf("[fleet] channel %d saved%s\n", ch, fleet::active() ? " and applied" : " (applies when fleet starts)");
    else Serial.println("[fleet] channel must be 1-13");
  } else if (!strcmp(arg, "remac")) {
    fleet::rerandomizeMac();
  } else if (!strncmp(arg, "rate ", 5)) {
    fleet::dbgSetRate(atoi(arg + 5));
  } else if (!strcmp(arg, "trace on") || !strcmp(arg, "trace off")) {
    fleet::dbgSetTrace(!strcmp(arg, "trace on"));
#ifdef FLEET_DEBUG
  } else if (!strncmp(arg, "m ", 2)) {   // "m <M: command>" bench passthrough to the phone-command handler
    char out[768];
    fleet::CmdAction act = fleet::handlePhoneCmd(arg + 2, out, sizeof(out));
    Serial.printf("[fleet] FLT= %s\n", out);
    if (act == fleet::CMD_RESTART) fleetRestart("fleet command");
  } else if (!strcmp(arg, "fk")) {   // bench enrollment without the web app: this unit's fleet key + the leader command
    fleet::dbgPrintFk();
  } else if (!strncmp(arg, "add ", 4)) {   // "add <sid> <fk> <name>" on the leader
    char sid[16] = {0}, fk[40] = {0}, name[24] = {0};
    if (sscanf(arg + 4, "%15s %39s %23s", sid, fk, name) == 3) fleet::dbgAddMember(sid, fk, name);
    else Serial.println("[fleet] usage: fleet add <sid 12hex> <fk 32hex> <name>");
#endif
  } else if (!strncmp(arg, "pad ", 4)) {
    fleet::dbgSetPad(atoi(arg + 4));
#ifdef FLEET_DEBUG
  } else if (!strcmp(arg, "crash")) {   // bench test of the boot-loop guard (docs §1.4): a real panic reset
    Serial.println("[fleet] crash test -- abort()");
    Serial.flush();
    abort();
#endif
  } else {
    Serial.println("[fleet] usage: fleet | leave | client | leader on|off | ch <1-13> | remac | rate <1|12> | pad <250|1300|0>");
  }
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
  if (!strcmp(cmd, "bonds")) {
    Serial.printf("[bonds] sec %d, stored %d, link %s\n", PHONE_BLE_SEC, phone::bondCount(),
                  !phone::connected() ? "none" : phone::linkEncrypted() ? "encrypted" : "not encrypted");
    return;
  }
  if (!strcmp(cmd, "bonds clear")) {
    g_bondsArmedAt = millis() | 1;   // |1: never 0
    Serial.println("[bonds] every paired phone must then forget this scanner in Android Bluetooth settings.");
    Serial.println("[bonds] type `bonds confirm` within 15 s to proceed.");
    return;
  }
  if (!strcmp(cmd, "bonds confirm")) {
    if (!g_bondsArmedAt || millis() - g_bondsArmedAt > REKEY_CONFIRM_MS) {
      g_bondsArmedAt = 0;
      Serial.println("[bonds] not armed -- type `bonds clear` first");
      return;
    }
    g_bondsArmedAt = 0;
    Serial.println(phone::clearBonds() ? "[bonds] cleared" : "[bonds] clear FAILED");
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
    fleet::onRekey();   // a fleet session was keyed from the old key
    Serial.printf("[key] re-keyed: generation %lu. Type `key` for the new key.\n", (unsigned long)devkey::gen());
    if (!saved) Serial.println("[key] WARNING: rekey NOT persisted (NVS write failed) -- the old key returns after a reboot");
    return;
  }
  if (!strncmp(cmd, "fleet", 5) && (cmd[5] == 0 || cmd[5] == ' ')) {
    const char* a = cmd + 5;
    while (*a == ' ') a++;
    fleetCmd(a);
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

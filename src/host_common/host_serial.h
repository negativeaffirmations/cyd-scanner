// host_serial.h — tiny USB-serial command line for the hosts (docs/ble-auth.md "Key management").
// Physical USB access is trusted (owner decision 2026-10-29), so the phone-link key can be read here --
// but only on request: `key` prints it, nothing ever prints it unprompted.
//   help          list the commands
//   key           unit name, sid, key generation, key, and the pairing link (QR payload)
//   rekey         arm a key rotation; `rekey confirm` within 15 s performs it (kicks the phone)
#pragma once

namespace hostserial {
// Non-blocking: drains whatever bytes are waiting, acts on complete lines. Called every loop().
void service();
}

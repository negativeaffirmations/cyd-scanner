# BLE phone-link authentication (SEC-H3) — design

Status: **Phase A in progress** (2026-10-29). Unblocks the fw 0.18.0 SD-card file explorer, which must not be
flashed to any unit or have its web app pushed until Phase A lands and passes review + hardware tests.

## Problem
The host's (CYD / XIAO-S3 mini) phone GATT service has no pairing or auth. Any central in range that connects
while the owner's phone isn't connected can use every CMD (file explorer `Z`/`U`, `F:` any path, `D:`, whitelist,
DB reload…), set TIME/GPS, and receive STATUS (incl. the SoftAP PSK), LOGDATA and DETS.

Note: in NimBLE-Arduino 2.5.1 the CCCD carries one global permission (`ble_gatts.c`
`ble_gatts_register_clt_cfg_dsc`) and notifications are not security-checked, so `READ_ENC` on a characteristic
does NOT stop an unauthenticated peer subscribing to its notifications. Gating must be done in firmware.

## Approach
A per-device random 128-bit key in NVS, delivered to the phone by a QR (printed sticker / CYD screen / serial),
proven per connection by HMAC-SHA256 challenge-response. BLE encryption is layered on later.

| Phase | Content | Ships |
|---|---|---|
| **A** | key + HMAC challenge-response, firmware gate on every characteristic, rekey (CYD menu + serial), web app auth + key store, `phonecfg` audit log line | with fw 0.18.0 |
| **B** | LESC just-works bonding requested by the device only AFTER HMAC success (`PHONE_BLE_SEC=1`, opportunistic) | after hardware test T10 |
| **C** | `PHONE_BLE_SEC=2`: gate = authed AND encrypted; AP creds only over an encrypted link | after B is proven |

Per-command MACs are not planned: B/C's AES-CCM link MIC defeats post-auth injection.

## Owner decisions (2026-10-29)
1. Ship Phase A first; B/C follow.
2. Limits: 30 s auth timeout, 3 bad tries per connection → kick, 6 fails / 10 min → 60 s lockout doubling to 16 min.
3. Key generation number goes in the URL (`&kg=N`) and on the sticker.
4. CYD Phone-key screen shows the key as text as well as the QR.
5. Phone key store must not be readable by other pages on the origin → **non-extractable `CryptoKey`s in
   IndexedDB** (not localStorage).
6. Keep backward compatibility with old (no-AUTH) firmware for now: warning badge only.
7. Add the `phonecfg` audit line to the scan log.
8. Anyone with physical USB access may print the key over serial (physical access = trusted).
9. Defer letting the owner pre-empt an unauthenticated squatter.

## Wire format

### AUTH characteristic
UUID `9a1e0007-2b7e-4c1a-9b00-1a2b3c4d5e6f`, `READ | WRITE`, never ENC. Its presence is the new-firmware signal.

Read value (ASCII):
```
v=1;sid=<12hex>;name=<…>;fw=<x.y.z>;sec=<0|1|2>;kg=<gen>;st=<0|1|2>[;n=<32hex>][;f=<fails>][;wait=<s>][;dp=<64hex>][;enc=<0|1>]
```
`st` 0 = locked (nonce `n` pending), 1 = authenticated on this connection, 2 = rate-limited (`wait` s).
`n` only while `st=0`; `dp` only at `st=1`; `enc` link encrypted (always 0 in Phase A).

Write: `H:<cn 32hex>:<mac 64hex>` — exactly 99 bytes, hex case-insensitive. `cn` = 16-byte client nonce.

MAC inputs (ASCII, lowercase hex, distinct labels prevent reflection):
- phone: `mac = HMAC-SHA256(key, "CFLO-AUTH-1|" + sid + "|" + n + "|" + cn)`
- device: `dp  = HMAC-SHA256(key, "CFLO-DEV-1|"  + sid + "|" + n + "|" + cn)` — the app verifies `dp` before
  sending TIME/GPS (a look-alike device can't harvest the owner's position).

### Device state machine (`phone.cpp`)
- `onConnect`: store conn handle, `authHandle = NONE`, `connMs`, `connFails = 0`, fresh nonce (`esp_fill_random`
  after NimBLE init), rebuild AUTH value.
- AUTH `onWrite`, in order: wrong connection / already authed → ignore; lockout active → `st=2;wait=N`, kick;
  strict parse (fail counts); constant-time compare. Success → `authHandle = handle`, wipe nonce, `st=1;dp=…;enc=0`,
  reset fail window, flag loop() to push STATUS. Failure → `connFails++`, global fail ring, NEW nonce, `st=0;f=N`;
  `connFails >= 3` → kick.
- TIME/GPS/CMD `onWrite`: first statement `if (!isAuthed(ci)) { log ignored; return; }` (doesn't count to lockout).
- `onDisconnect`: clear handles + nonce, AUTH → `st=0` (no `n`), and also clear queued TRANSFER requests
  (log/file/export/list/whitelist-list/fs/upload) so nothing streams to the next peer.
- `phone::service()` (each loop, via `core::servicePhoneCommands`): unauthed > 30 s → disconnect; `kick` flag →
  disconnect (from loop context, never inside a callback).
- Lockout: RAM ring of last 6 failure times; 6 in 10 min → 60 s, doubling per further lockout to 16 min; success resets.
- All notifications go to `authHandle` only (explicit-buffer + conn-handle notify overload).

### Pre-auth surface
| Surface | Before auth | After auth |
|---|---|---|
| AUTH read/write | yes | read (st=1) |
| TIME / GPS / CMD write | ignored | yes |
| STATUS read | fixed placeholder `auth=0` (real STATUS never stored in the attribute) | same; data only by notify |
| STATUS / LOGDATA / DETS notify | never sent | to authHandle only |

`phone::authed()` replaces `phone::connected()` as the "phone is there" gate in the transfer loops (host_log),
host_files, DETS push (host_status) and phone-GPS freshness (host_core). `connected()` stays for UI: CYD BLE icon
grey when connected-unauthed, blue when authed; HOME phone icon green only when authed.

## Key management
- `src/host_common/devkey.*`: 16-byte key in NVS `cydscan`/`blekey`, generation `cydscan`/`blekeygen` (u32).
  `begin()` after `NimBLEDevice::init`; `keyHex()`, `gen()`, `pairUrl()`, `rekey()`, HMAC via mbedtls, `ctEqual()`.
- Pair URL: `https://negativeaffirmations.github.io/cyd-scanner/webapp/#devkey=<32hex>&sid=<12hex>&kg=<gen>`
  (move the base URL to a shared constant). Needs QR version 6 on the CYD (scale 5).
- CYD: Settings → **Phone key** (`SCR_PHONEKEY`): QR + name + `gen N` + key as text (4×4 hex groups) + `NEW KEY…`
  → confirm screen (`SCR_REKEYCONFIRM`) → `phone::rekeyAndKick()`.
- Serial (`src/host_common/host_serial.*`, both hosts, non-blocking line reader): `help`, `key` (name, sid, gen,
  key, URL), `rekey` → `rekey confirm` within 15 s. Never prints the key unprompted.
- A normal upload keeps NVS (key survives reflash); `erase_flash` makes the sticker obsolete.

## Audit line (scan log)
Whenever the phone changes device config or card files, one NDJSON line on the active stream:
`{"epoch":…,"ms":…,"lat":…,"lon":…,"evt":"phonecfg","op":"upload|delete|rename|mkdir|wladd|wlrm|reload|rekey","path":"…"}`
(`path` omitted when not applicable; `rename` uses `path` + `to`). Readers that don't know it skip it by `evt`.

## Web app
- New `js/ble/auth.js` (new file → never stale): key store in **IndexedDB** holding non-extractable HMAC
  `CryptoKey`s (`sign`+`verify`) keyed by sid, plus `{name, kg, ts}` metadata; `handleDevKeyHash()` (runs before
  `handlePairHash()`, strips only its own params via `history.replaceState`); `authenticate(authChar)`;
  paste parser (full URL / `sid key` / key alone).
- `connection.js`: after `getPrimaryService`, try AUTH. Absent → legacy path + "unsecured firmware" badge.
  Present → authenticate; on no key / rejected → `#authDlg`, disconnect. On ok → subscribe STATUS/LOG/DETS, then
  time sync + GPS.
- Settings card **Scanner keys** (link state, stored keys with Forget, paste field); `#authDlg` (adds a
  "scanner was re-keyed" line when `kg` mismatches).
- Bump `?v=` on style.css + main.js and `#uiver`.

## Migration / stickers
1. Deploy the web app first (works with fw ≤ 0.17).
2. Flash each unit; pair from CYD Settings → Phone key, or serial `key` on the minis (open the console without
   resetting). Scan in **Chrome** (the key store is per browser).
3. Render sticker QRs **offline only** (the URL is the secret). Label with unit name + key gen. Put inside the
   case / battery door. Never commit keys or sticker images.

## Known limitations (accepted for Phase A; from the 2026-10-29 review + security passes)
- **Cleartext link.** A passive BLE sniffer following the owner's session sees STATUS (incl. the SoftAP PSK while
  Wi-Fi download is up), GPS, DETS and log contents; an active link-layer injector could issue commands after auth.
  Closed by Phase B/C encryption; prioritise Phase C for the AP credentials.
- **Relay.** The HMAC isn't bound to the BLE connection: an impostor the owner picks in the Chrome chooser can relay
  the real scanner's nonce to the phone and the phone's `H:` back, gaining an authed session on the real scanner.
  LESC just-works (Phase B) does not stop this; only MITM-protected pairing (passkey on the CYD; the minis have no
  display) or a channel binding visible to the web app would. Mitigation: the web app warns when the device name
  doesn't match the stored key's name.
- **Slot squatting / lockout DoS.** A scripted central can hold the single connection 30 s at a time, and 6 bad
  tries from anyone lock out the owner too (up to 16 min). No data exposure; scanning/logging continue. Fix =
  deferred owner pre-emption (decision 9).
- **Key in URL history.** `replaceState` strips the fragment from the current URL, but the QR-scanner app's history
  and Chrome history / History Sync may keep the original `#devkey=` URL. An in-app QR reader (BarcodeDetector)
  would avoid ever making the key a URL.
- **Shared origin.** IndexedDB is shared by every `negativeaffirmations.github.io/*` page. Keys are non-extractable
  (can't be read out) but another page on that origin could still use them to sign. Acceptable while only the owner
  publishes there; a dedicated origin removes it.
- **Physical access is trusted** (decision 8): the key is unencrypted in NVS, printable over serial, shown on the
  CYD Phone-key screen.

## Hardware test plan
T1 builds + flash/RAM delta · T2 new app vs 0.17 (legacy) · T3 happy path · T4 deep link (CYD QR + serial URL,
combined with `#server=`) · T5 attacker with nRF Connect (no notifications, writes ignored, kicks, lockout) ·
T6 replay rejected · T7 rekey (CYD + serial) · T8 20 connect/disconnect cycles incl. mid-transfer ·
T9 deferred 0.18.0 explorer tests · T10 Phase B on mini #2 · T11 reviewer + security passes before flashing.

# Fleet mode — cooperating scanners over ESP-NOW (design)

Status: **DESIGN (2026-10-30), not started.** Plan only; the build follows the phases in §13.
**Owner decisions of 2026-10-31 are in §16 and override earlier sections where they conflict.**
Scope: the owner's own scanners coordinate with each other over ESP-NOW. Nothing here transmits
to, about, or at a third-party device. Fleet frames go only between enrolled units, on one fixed
channel, at a low duty cycle. There are no floods, and no frame is ever sent to an address the
fleet didn't learn from its own handshake.

**Measured basis (real CYD, fw 0.19.0 + throwaway probes, 2026-10-30):**
- **Normal CYD boot** (NimBLE phone link up): ~58.7 KB free heap. Starting Wi-Fi STA costs ~51 KB, which
  leaves ~6 KB. The board OOM-aborted on boot. **Not viable.**
- **Fleet-client boot** (no NimBLE, `esp_bt_mem_release(ESP_BT_MODE_BTDM)`, then Wi-Fi STA + ESP-NOW):
  ~114 KB free steady, minimum 109 KB, largest block 69.6 KB. Stable for 90 s with scanning normal.
  - A real dual-mode binary keeps the NimBLE code linked (−8.5 KB static RAM), so expect **~105 KB**.
- **Mode switching:** the Bluetooth memory release can't be undone until a reboot, so fleet mode is a
  boot-time mode, and **the CYD can only ever be a fleet client** (owner decision 2026-10-30).

## 0. Summary — who owns what, what crosses which link

| Link | Change |
|---|---|
| **Host ⇄ host (new)** | ESP-NOW on the HOST radios (CYD ESP32 / mini S3), one fixed 2.4 GHz channel. New host-only protocol `src/host_common/fleet_proto.h` (the C5s never include it). |
| **CYD/S3 ⇄ C5 UART** | `ScanConfig` gains an 18 B `ScanPlan`, making it 21 B → **`PROTOCOL_VERSION` 11 → 12**. Every host env and both C5 envs rebuild and are flashed together. `Status` is unchanged (28 B). |
| **Phone ⇄ leader BLE** | New CMD `M:<op>…`, a `FLT=` reply on LOGDATA, a STATUS part-2 key `flt=`, and **DETS v9** (a per-row `via` member mask). |
| **Phone ⇄ CYD (normal mode)** | CMD `M:j` = "reboot into fleet client". |

Roles: the **leader is always a mini** (XIAO-S3: 512 KB SRAM + 8 MB PSRAM, NimBLE phone link stays up).
A **client is a CYD** (no NimBLE at all, BT memory released at boot) **or another mini**. The leader
pushes time, position and a scan plan; clients forward their detections; the leader merges them for the
phone. Every unit keeps its own C5 scanning and its own SD log.

## 1. Roles, modes, enter/exit

### 1.1 NVS
Namespace `cydscan`:
- `fltrole` u8: 0 off / 1 client / 2 leader. The CYD refuses 2 (`hostboard::fleetLeaderCapable()` is false).
- `fltch` u8: fleet channel (default 1). The client also stores the last channel a SYNC announced.
- `fltmem` blob (leader only): `FleetMember[4]`, 48 B each = 192 B: `sid[6]`, `fk[16]`, `kg u32`,
  `name[20]`, `roleOverride u8`, `flags u8`.
- `fltboot` u8: crash counter for boot-loop fallback (§1.4).
- `fltlogr` u8 (leader): also log remote-only rows (default 0, §9.5).

### 1.2 CYD → fleet client (boot-time mode, irreversible until reboot)
Entry points (both write `fltrole=1` and then call `ESP.restart()`):
- **CYD Settings → "Fleet client…"** opens `SCR_FLEETENTER`, a confirm screen: "Reboots as a fleet
  client. Phone link OFF until you leave the fleet." The [Cancel] button is selected by default.
- **Web app (authed BLE to the CYD) → Settings → Fleet → "Make this scanner a fleet client"** sends
  CMD `M:j`. The CYD replies `FLT=OK\treboot` and restarts about 500 ms later.

Fleet-client `setup()` on the CYD (order matters):
1. `Preferences` read `fltrole`, then the crash-fallback check (§1.4).
2. `esp_bt_mem_release(ESP_BT_MODE_BTDM)` **before anything touches BT**. Measured: +42.9 KB.
   `phone::begin()` is never called. NimBLE stays linked (dual-mode binary) but never starts.
3. `hostboard::begin()`, `initSD()`, `sigdb::begin()`, `whitelist::begin()`, touch (as today).
4. `phone::loadTz()`: new, split out of `phone::begin()`, so local time still works with no phone.
5. `fleet::beginClient()`: Wi-Fi STA with a random MAC on the fleet channel, ESP-NOW, heap buffers (§3).
6. `devkey::begin()`: moved out of `phone::begin()` for this path. It runs **after** Wi-Fi starts so
   `esp_fill_random` gets radio entropy. In normal mode it still runs inside `phone::begin()`.
7. `pingC5()`. Scanning is forced on: `g_scanActive = true`, logging to the bg stream regardless of
   `g_bgScan`, because scanning is the reason a client exists.
8. `drawFleetClient()`. `SCR_FLEET` replaces HOME as the root screen.

### 1.3 Exit
- **On-device:** the FLEET CLIENT screen has a "Leave fleet" button, which opens `SCR_FLEETLEAVE`
  (confirm, Cancel default), then sets `fltrole=0`, writes `{"evt":"fleet","op":"leave"}` and calls
  `ESP.restart()`. It is reachable by touch **and** by the BOOT button (short = next button, long =
  select). It respects the touch lock: `serviceTouchLock()` already gates input before the screen
  switch, so a locked, bagged CYD can't leave by accident.
- **Remote release (optional, recommended):** web app → leader → CTRL `RELEASE` (encrypted, inside
  the session, §5). The client logs `op=release`, sets `fltrole=0` and reboots. Authorization is the
  member key: only the enrolled leader can send it.
- **Serial (any host):** `fleet` shows status, `fleet leave` exits and reboots, `fleet client`
  enters client mode and reboots, `fleet leader on|off` (minis only).
- **Remove member** (`M:r`) is NOT exit. The leader forgets the key and sends CTRL `UNJOIN`; the
  client stays in client mode, falls back to scanning on its own, and keeps answering probes.

### 1.4 Safety: boot-loop fallback
At boot `fltboot` counts CONSECUTIVE crash boots: `n = crash ? n+1 : 0`, where crash is a panic, WDT or brownout
`esp_reset_reason()`. After 60 s of stable uptime, set `fltboot=0`, so a crash later than 60 s after boot never
counts (and Leave stays reachable). At `n >= 3`, clear `fltrole` and boot normally (logged `op=fallback`). Without this, a fleet-mode OOM loop would leave the CYD with no phone link to fix it.

### 1.5 No leader present
The client still does everything on its own: full legacy scan plan, SD logging, its own C5 GPS time
and position if a NEO-6M is fitted (otherwise `epoch:0` rows, later backfilled by the timesync anchor
on the first fleet SYNC). The screen shows "Searching…". The client listens on its last channel for
9 s, then on 1, 6 and 11 in turn (9 s each), and answers at most one probe every 2 s.

### 1.6 Mini as client / leader
A mini keeps NimBLE in either role (it has RAM to spare). The leader starts fleet at runtime with
`fleet::beginLeader()` after `phone::begin()`; no reboot is needed. A mini client also keeps its phone
link (owner question Q1). The phone, when connected, outranks the fleet as a time/position source.

## 2. CYD FLEET CLIENT screen (`SCR_FLEET`, root while `fltrole=1`)
240×320 portrait. Reuses the existing status bar, counts line, threat tiles and button style
(`UI_ACCENT`):
- **Status bar:** as today, but the phone-link icon is replaced by a **fleet glyph** (two linked
  nodes): grey = searching, green = joined, amber = joined but leader quiet > 6 s. The clock gets a
  new tint when fleet-timed (green; cyan stays GPS).
- Title **FLEET CLIENT** (magenta).
- `Leader: CFLO-MINI-KOBALI`, or `Searching… ch 6`.
- `slot 2/3 · role WiFi · -54 dBm · 0.4 s ago`.
- `Time: fleet  Pos: GPS` (sources in use; `none` when absent).
- Counts line (reuse): `2.4 n · 5G n · BLE n · PRB n · U:n`. Cells for sources the role disables
  are drawn dim.
- `TX 1234 ok · 1.2% fail · C5 link ●`.
- Threat tiles (reuse `drawMenuThreats()`, 12 tiles), from the client's own table.
- Buttons: **[Live scan] [Show key] [Settings] [Leave fleet]**.
  - Show key opens the existing `SCR_PHONEKEY` (QR of `devkey::pairUrl()` plus hex). Back returns to
    `SCR_FLEET`. This lets a never-paired CYD be enrolled.
  - NEW KEY in fleet mode calls `devkey::rekey()` and `fleet::onRekey()`, which drops the session
    and shows "Key changed — re-add this scanner on the leader". `phone::rekeyAndKick()` is
    null-safe here but not needed.
  - Live scan opens the existing `SCR_SCAN`, with no phone/web-app controls.
- Repaints on the 2 s idle cadence. No heap use (TFT_eSPI direct draw).

## 3. Radio / memory architecture

### 3.1 Why host radios on one fixed channel
The host Wi-Fi radios are otherwise idle; the C5s keep their full time-sliced scan. Putting ESP-NOW on
a hopping C5 would need rendezvous slots and would steal passive-capture airtime. Rejected.

### 3.2 Bring-up (`fleet_link.cpp`, both roles)
- `WiFi.persistent(false); WiFi.mode(WIFI_STA)`.
- **Random locally-administered STA MAC per boot:** `esp_fill_random`, then
  `mac[0] = (mac[0] & 0xFC) | 0x02`, then `esp_wifi_set_mac(WIFI_IF_STA, mac)`. Verify in P1 that
  this works after `WiFi.mode`; if not, do it between `esp_wifi_init` and `esp_wifi_start`.
  **This is required:** `scannerIdHex()` is the eFuse base MAC, which is the default STA MAC.
  Without this every fleet frame broadcasts the scanner ID, which defeats §6.
- `esp_wifi_set_channel(fltch, WIFI_SECOND_CHAN_NONE)`.
- CYD: `esp_wifi_set_ps(WIFI_PS_NONE)`. S3: leave the default; WIFI_PS_NONE may be rejected while BT
  runs. An unassociated STA doesn't DTIM-sleep anyway. Verify in P1.
- `esp_wifi_set_max_tx_power(FLEET_TXPWR_QDBM)`, default 52 (13 dBm). Keeps fleet airtime from
  desensing the unit's own C5 a few cm away (owner question Q5).
- `esp_now_init()`, then `esp_now_get_version(&v)`: `maxPayload` = 1470 if v ≥ 2, else 250.
- `esp_now_set_pmk(FLEET_PMK)`: a fixed project constant. Secrecy rests on the per-session LMK.
- `esp_now_register_recv_cb(onRecv)` with signature
  `(const esp_now_recv_info_t*, const uint8_t*, int)`. Copy into an RX pool, take RSSI from
  `info->rx_ctrl->rssi`, and do nothing else in Wi-Fi task context.
- `esp_now_register_send_cb(onSent)` with signature `(const esp_now_send_info_t*, esp_now_send_status_t)`
  (IDF 5.5; the type is `wifi_tx_info_t`). Updates per-peer ok/fail counters.
- Leader only: add the broadcast peer `FF:FF:FF:FF:FF:FF` (unencrypted) for PROBE.
- Peer rate: try `esp_now_set_peer_rate_config(peer, {WIFI_PHY_MODE_11G, WIFI_PHY_RATE_12M})` and
  fall back to the 1 Mbps default if P1 shows worse delivery. At 12 Mbps a 1.4 KB frame is about
  1 ms on air, versus about 12 ms at 1 Mbps.

### 3.3 RX/TX buffers (heap-allocated in `begin*`, NOT static)
In normal mode the fleet module's static footprint must stay under 300 B. Normal CYD mode has only
58.7 KB free, so every buffer is `malloc`ed only when fleet mode starts.
- CYD client: RX pool 8 × 256 B = 2 KB (leader→client frames are all ≤ 74 B; a larger frame is
  dropped). One TX frame buffer of maxPayload (1.5 KB). Sent-delta table 96 × 20 B = 1.9 KB. Misc
  about 1 KB. **≈ 6.5 KB total.**
- S3 leader: RX pool 16 × 1472 B = 23.5 KB plus the remote table (§9), both
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.
- Hand-off: a FreeRTOS queue of pool indices (`xQueueSendFromISR`-style from the Wi-Fi task). Full
  queue → drop and count. `fleet::service()` drains at most 8 frames per `loop()` pass.

### 3.4 S3 leader coexistence (Wi-Fi ESP-NOW + NimBLE peripheral)
One 2.4 GHz radio, software coex (`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`). BLE connection events and
ESP-NOW RX share airtime. Unicast ESP-NOW is MAC-acked and retried, and delta forwarding with a 10 s
refresh (§9) tolerates loss.
- **P1 must measure** delivery with the phone connected and idle, during DETS pushes, and during a BLE
  log download.
- Tuning knob: request a longer phone connection interval (30–50 ms) via `updateConnParams` once
  authed.
- Long BLE transfers block `loop()`, so call `fleet::service()` from the transfer loops (hook in
  `phone::transferAlive()` via a `core::idleHook`) so members don't hit the 15 s "leader lost" timeout.

### 3.5 Leader SoftAP must share the fleet channel (`webshare.cpp`)
- `start()`: `WiFi.mode(fleet::active() ? WIFI_AP_STA : WIFI_AP)` and
  `WiFi.softAP(ssid, pw, fleet::active() ? fltch : 1)`.
- `stop()`: `WiFi.mode(fleet::active() ? WIFI_STA : WIFI_OFF)`. It must not tear the fleet down.
- `runDownload()` on the S3 also calls `fleet::service()`.
- The 8 dBm fleet TX cap (`esp_wifi_set_max_tx_power`) is radio-wide, so it applies to the leader's SoftAP too: the download range is shorter while a fleet is up. The AP is limited to 1 client (`max_connection=1`) because SoftAP and ESP-NOW share hardware key slots. `stop()` calls `fleet::retune()` because `softAPdisconnect` leaves the radio on channel 1.
- The SoftAP MAC (base+1) and the MAC-derived SSID still reveal identity during a download. That is
  existing behaviour, flagged as separate hardening.

### 3.6 UART clocks
The probe saw the CYD's C5 link behave normally with Wi-Fi up. Original-ESP32 APB stays at 80 MHz
without DFS. Hardening option (CLAUDE.md note): `LinkSerial.setClockSource(UART_CLK_SRC_REF_TICK)`
on the CYD in fleet mode and `UART_CLK_SRC_XTAL` on the S3, behind a test in P1. The C5 side is
already on XTAL.

## 4. Fleet protocol (`src/host_common/fleet_proto.h`, host-only)
All structs are packed and little-endian. `FLEET_PROTO_VERSION = 1` is independent of link and DETS
versions. Every frame starts with `Hdr`. Discovery frames (Probe/Hello/Join) are **cleartext but
nonce- and HMAC-bound**. Everything after Join is **ESP-NOW-encrypted (LMK) plus an app-level seq
replay check**.

```cpp
namespace fleet_proto {
constexpr uint8_t  FLEET_PROTO_VERSION = 1;
constexpr uint8_t  FLEET_DEFAULT_CH    = 1;
constexpr uint16_t FLEET_V1_MAX = 250, FLEET_V2_MAX = 1470;
constexpr int      FLEET_MAX_MEMBERS = 4;     // clients per leader (prebuilt libs: 7 encrypted peers)
constexpr int      DETS_PER_V1 = 2, DETS_PER_V2 = 16, RIDS_PER_V1 = 3, RIDS_PER_V2 = 8;
enum class Msg : uint8_t { Probe=0x01, Hello=0x02, Join=0x03, JoinAck=0x04,
                           Sync=0x10, Plan=0x11, Ctrl=0x12,
                           MStat=0x20, DetBatch=0x21, RidBatch=0x22 };
enum CtrlOp : uint8_t { CTRL_RELEASE=1, CTRL_UNJOIN=2, CTRL_NEWSESSION=3, CTRL_SETCH=4 };
enum PlanRole : uint8_t { PLANROLE_FULL=0, PLANROLE_WIFI=1, PLANROLE_WIFI24=2, PLANROLE_WIFI5=3, PLANROLE_BLE=4 };   // scan-plan role; not fleet::ROLE_*
#pragma pack(push,1)
struct Hdr     { uint8_t ver, type; uint16_t sess; uint32_t seq; };                       //  8
struct Probe   { Hdr h; uint8_t nl[16]; uint8_t ch, rsv; };                                // 26 bcast, clear
struct Hello   { Hdr h; uint8_t nl[16], nc[16], tag[16]; uint16_t maxPayload; };           // 58 unicast, clear
struct Join    { Hdr h; uint8_t nl[16], nc[16]; uint8_t slot, nMembers; uint16_t maxPayload;
                 uint8_t proof[16]; };                                                     // 60 unicast, clear
struct JoinAck { Hdr h; uint8_t proof[16]; uint8_t board, caps; uint8_t bleMac[6];
                 char fw[12]; char name[20]; };                                            // 64 encrypted
struct Sync    { Hdr h; uint32_t utc; int16_t tzOffMin; uint8_t tsrc, psrc;
                 int32_t lat, lon; uint32_t fixAgeMs; uint8_t ch, flags; uint8_t leaderSid[6];
                 uint8_t nSelf; uint8_t selfMacs[9][6]; };                                 // 91 encrypted, every 2 s
struct Plan    { Hdr h; uint8_t planGen, role, slot, nMembers;
                 link_protocol::ScanPlan plan; };                                          // 30 encrypted
struct Ctrl    { Hdr h; uint8_t op; uint8_t arg[3]; };                                     // 12 encrypted
struct MStat   { Hdr h; uint8_t planGen, c5Link; int8_t bat; uint8_t gpsFix;
                 uint16_t dets, deauth; uint8_t evil, rid, tsrc, psrc; uint32_t uniq;
                 uint16_t heapKB, txFailPm; int32_t lat, lon; };                           // 36 encrypted, every 2 s
struct DetBatchHdr { Hdr h; uint16_t cycle; uint8_t idx, cnt; };                          // 12 + cnt*81
struct RidBatchHdr { Hdr h; uint8_t cnt, rsv; };                                          // 10 + cnt*75
#pragma pack(pop)
static_assert(sizeof(Sync) == 91 && sizeof(MStat) == 36 && sizeof(Hello) == 58, "fleet wire size");
}
```
Notes:
- `caps` bits: GPS fitted, 802.15.4 enabled, 5 GHz antenna, phone-link present.
- `board`: 1 CYD / 2 MINI.
- `selfMacs[9]` (Sync = 91 B): 9 = leader BLE MAC + phone `peerMac` + SoftAP BSSID + up to 6 member `bleMac`s. The leader's own BLE MAC, its phone's `peerMac`, its SoftAP BSSID while up, and each
  member's `JoinAck.bleMac`. Every member drops these from its own detections (`isSelfDet`).
- DetBatch v1: 12 + 2×81 = 174 B. v2: 12 + 16×81 = 1308 B, under 1470 with margin.
- `seq` counts per direction per session, starting at 1. `sess` is random and non-zero, chosen by the
  leader in Join; 0 means discovery.

## 5. Pairing / auth / keys

### 5.1 Member key (FK) — the phone key never leaves its device
`FK_c = HMAC-SHA256(K_c, "CFLO-FLT-KEY-1|" + sid_c)[0..15]`, where `K_c` is the client's 128-bit
devkey and `sid_c` is 12 lowercase hex.
- The **client** computes it with a new `devkey::fleetKey(uint8_t out[16])` (wraps `devkey::hmac`).
- The **web app** computes it with `crypto.subtle.sign('HMAC', K_c, …)` on the non-extractable
  CryptoKey it already stores for that sid (from any earlier Phone-key QR, sticker or serial pairing).
- The **leader** only ever holds `FK_c`. A compromised leader can't reach a client's phone link, and
  a client rekey invalidates its FK automatically.

### 5.2 Enrollment (web app → leader, authed BLE)
The BLE link is cleartext in Phase A, so FK is masked under the leader's key:
- `w` = 16 random bytes (web).
- `pad = HMAC(K_L, "CFLO-FLT-WRAP-1|"+sid_L+"|"+hex(w)+"|"+sid_c)[0..15]`.
- `wrapped = FK_c XOR pad`.
- `tag = HMAC(K_L, "CFLO-FLT-ADD-1|"+sid_L+"|"+hex(w)+"|"+sid_c+"|"+kg+"|"+hex(wrapped)+"|"+name)[0..7]`.
- CMD `M:a:<sid_c 12hex>:<kg>:<w 32hex>:<wrapped 32hex>:<tag 16hex>:<name ≤20>` (≤ 125 B).

The leader verifies the tag (constant-time), rejects a `w` seen in its last 8, unwraps, and stores the
member in `fltmem` (replacing any entry with the same sid, so re-enrolling after a rekey just works).
It logs `phonecfg op=fleetadd path=<sid>`. At most `FLEET_MAX_MEMBERS` members (else `ERR\tfull`).

### 5.3 Handshake
Labels are ASCII; fields are raw bytes concatenated after the label.
1. Leader → broadcast **Probe**: `nl` is fresh each probe. The leader honours its last 2 nonces for 10 s.
2. Client (not joined) → unicast **Hello** to the probe's source MAC. The leader is first added as an
   unencrypted peer. `nc` is fresh, and
   `tag = HMAC(FK_c, "CFLO-FLT-HELLO-1" | nl | nc | macL | macC)[0..15]`. Rate limit: at most 1 Hello
   per 2 s.
3. Leader: for each un-joined member, compute the expected tag and compare constant-time (≤ 4 HMACs;
   at most 10 Hellos evaluated per second). A match identifies the sid **without it ever being on the
   air**. The leader picks a `sess`, then sends **Join** (unencrypted peer) with
   `proof = HMAC(FK_c, "CFLO-FLT-JOIN-1" | nl | nc | sess | slot | nMembers)[0..15]`. On the Join
   send-callback, `esp_now_mod_peer` turns the client entry into an encrypted peer with LMK = SK.
4. Client verifies `proof`, which authenticates the leader (no rogue leaders). It accepts a Join only
   for its outstanding (nl, nc) and only within 5 s; one use. It derives
   `SK = HMAC(FK_c, "CFLO-FLT-SK-1" | nl | nc | sess | macL | macC)[0..15]`, makes the leader an
   encrypted peer with LMK = SK, and sends **JoinAck** (encrypted) with
   `proof = HMAC(FK_c,"CFLO-FLT-ACK-1"|nl|nc|sess)[0..15]`. It resends every 300 ms (up to 5 times)
   until the first Sync arrives. That covers the race where the ack lands before the leader's
   `mod_peer`.
5. Joined. The leader sends Plan + Sync. The client sends MStat + DetBatch.

### 5.4 Replay, loss, limits
- Fresh nonces give a fresh SK each session, so a frame from an old session never decrypts.
- Within a session, the app-level `seq` uses a 32-wide sliding window. Duplicates and frames older
  than the window are dropped. ESP-NOW CCMP replay behaviour is not relied on.
- Cleartext frames are bound by nonce lifetime and are one-use.
- **Lost:** the leader marks a member lost after no MStat for 10 s. The client marks the leader lost
  after no Sync for 15 s, then deletes the peer (or mods it back to unencrypted; verify in P2) and
  returns to answering Probes.
- **Rekey:** a client rekey changes FK, so rejoin fails (Hello tags no longer match). The leader shows
  the member "not found". The owner re-adds it from the new key; the FLEET screen's Show key offers it.
  A leader rekey doesn't affect stored FKs.
- **Peer cap:** CYD client = 1 encrypted peer. Leader ≤ 4 encrypted plus broadcast (the limit is 7).
- **Hostile frames:** malformed, wrong-version or oversized frames are dropped before any crypto. The
  RX queue is bounded. The worst case is DoS of the fleet itself; scanning and logging continue on
  every unit.

## 6. Privacy-preserving discovery
- No stable identifier is ever sent in the clear. The STA MAC is random per boot (§3.2). Probe and
  Hello carry only random nonces and HMAC tags (unlinkable across probes, because `nc` is fresh). The
  sid, name, fw, board and BLE MAC travel only in encrypted frames.
- Probes are sent only while an enrolled member is un-joined: every 3 s for 2 min, then every 15 s.
  When all members are joined they stop. Clients never broadcast.
- Residual exposure, accepted:
  - The frames are recognisably ESP-NOW (vendor action frames, OUI 18:FE:34). That is common hobby
    traffic, and the payloads are high-entropy.
  - A per-boot MAC is linkable within one boot.
  - Option (Q11): rotate the MAC at each re-join.

## 7. Leader → client time / tz / position
- The leader sends Sync every 2 s: `utc = epochLocal() + tzOffsetMin*60` (as UTC), `tzOffMin`, its
  `timeSource()` and `posSource()`, `lat/lon` (deg·1e7), and `fixAgeMs` = the leader's fix age.
- Client side (`host_core.cpp`), a new **source 4 = fleet**:
  - Latch `g_fleetUtc` / `g_fleetAtMs` and `g_fleetLat/Lon/FixAt`. The fix age includes the leader's
    `fixAgeMs` plus the time since receipt.
  - **Time** under prefer-phone: phone (1) > **fleet (4)** > fresh GPS (2) > RTC (3) > latched GPS.
    Under prefer-device: fleet goes below every device clock and above the phone. Under device-only:
    fleet is ignored (it is an external source, like the phone).
    `epochLocal()`: `g_fleetUtc + elapsed − tzOff*60`.
  - **Position:** live onboard GPS (2) > phone (1, mini clients) > **fresh fleet fix (4, < 10 s)** >
    held (3, now including a fleet fix). The client's own fix is better for its own location (Q4).
    `fix_age` follows the existing rules: present when held or older than 10 s, using the leader's age.
  - `tzOffMin` from Sync goes through `phone::setTzOffsetMin()` (new), which writes NVS only on change.
  - The first Sync makes `timeHasSync()` true. The existing `renameSessionOnSync()` (called every
    loop from `servicePhoneCommands`) then appends the `timesync` anchor and renames the file. No new
    logic needed.
- STATUS `tsrc` / `gpssrc` value **4 = fleet** (mini clients with a phone). The web app treats any
  unknown non-zero value as "device".
- `fleet::isFleetSelf(mac)` checks `Sync.selfMacs` and is folded into `isSelfDet()`, so a client
  never logs the leader's BLE advert, the leader's phone, or the leader's SoftAP.

## 8. Coverage splitting — `ScanPlan` (link v12)

### 8.1 Wire (`lib/link_protocol/link_protocol.h`)
```cpp
static constexpr uint8_t PROTOCOL_VERSION = 12;  // v12: ScanConfig carries a ScanPlan (fleet coverage split)
enum PlanPhase : uint8_t { PLAN_AP24=1<<0, PLAN_AP5=1<<1, PLAN_PROM24=1<<2, PLAN_PROM5=1<<3,
                           PLAN_154=1<<4, PLAN_BLE=1<<5, PLAN_VALID=1<<7 };
struct ScanPlan {            // all-zero (PLAN_VALID clear) = legacy full time-slice, exactly today's behaviour
  uint8_t  phases;           // PlanPhase bits; effective = phases AND ScanConfig.sources
  uint8_t  planId;           // leader plan generation (diagnostic; printed on the C5 console)
  uint16_t ch24Mask;         // bit n = 2.4 GHz channel n+1 for PROM24 (0 = default 1..11)
  uint16_t ch5Mask;          // bit i = kHopChans5[i] for PROM5 (0 = all 9)
  uint8_t  ch154Mask;        // bit i = k154Chans[i] (0 = all 5)
  uint8_t  rsv;
  uint16_t prom24Ms, prom5Ms, win154Ms;   // window lengths (0 = 4500 / 4000 / 3000)
  uint16_t dwellMs;          // promisc per-channel dwell (0 = 160)
  uint16_t gapMs;            // inter-cycle gap (0 = WIFI_GAP_MS 3000)
};                           // 18 B
struct ScanConfig { uint8_t sources; uint16_t dwell_ms; ScanPlan plan; };   // 21 B
static_assert(sizeof(ScanPlan) == 18 && sizeof(ScanConfig) == 21, "bump PROTOCOL_VERSION");
```
**Why it goes in `ScanConfig` and not a new `Command`:** the plan rides on every 2 s StartScan, so the
C5 holds no state the host must restore. A C5 reboot or link glitch self-heals on the next poll.
Normal (non-fleet) hosts send an all-zero plan, which means today's behaviour.

### 8.2 C5 (`src/c5/main.cpp`)
- In `handleFrame(StartScan)`, if `memcmp(plan, g_plan)` differs, call `applyPlan()`:
  - Rebuild runtime hop arrays `g_hop24[≤13]`, `g_hop5[≤9]`, `g_hop154[≤5]` from the masks
    (replacing direct use of `kHopChans*`).
  - Set the window/dwell/gap values.
  - If the current passive window is no longer allowed, end it (`finishPromisc` / 15.4 exit) and go
    to PH_SCAN.
  - Print `[C5] plan id=N phases=…`.
- Gating:
  - `planAllows(bit)` = `!(phases & PLAN_VALID) || (phases & bit)`.
  - `bleWanted()` = `(g_srcMask & MASK_BLE) && planAllows(PLAN_BLE)`. It replaces every
    `g_srcMask & MASK_BLE` test, including the watchdog, `applyBleMask`, `finishPromisc` and the 15.4
    exit.
  - AP scan: `planAllows(PLAN_AP24|PLAN_AP5)`. Results are filtered per band by AP24/AP5 as well as
    the source mask.
  - `afterScanPhase`: PROM24 = `MASK_PROBE && planAllows(PLAN_PROM24)`. PROM5 = `MASK_WIFI5 &&
    planAllows(PLAN_PROM5)` (a 5 GHz-only member enters PROM5 directly).
  - 15.4: `planAllows(PLAN_154)`.
- **A BLE-only plan has no passive phases**, so `blePaused()` is never true and BLE runs at about
  100% duty. AP scan can stay (it coexists). A Wi-Fi plan drops BLE entirely, so passive capture gets
  all the airtime and the windows can be longer with a shorter gap.
- Optional, verify first: restrict the active AP scan to one band (`esp_wifi_set_band_mode`, reset to
  AUTO before PROM5) to shorten the busy window.
- Host: `requestScan()` sets `cfg.plan = fleet::currentPlan()` (zeros when not in a fleet).

### 8.3 Role templates (leader `assignRoles()`), N = joined units including the leader

| N | Unit | Role | phases | Notes |
|---|---|---|---|---|
| 1 | — | FULL | 0 (legacy) | |
| 2 | client (CYD default) | WIFI | AP24\|AP5\|PROM24\|PROM5\|154* | BLE off; prom24 6000, prom5 5000, gap 500 |
| 2 | leader | BLE | BLE\|AP24\|AP5 | BLE ≈ continuous; AP scan coexists |
| 3 | A | WIFI24 | AP24\|PROM24\|154* | 1..11, dwell 200 |
| 3 | B | WIFI5 | AP5\|PROM5 | 9 non-DFS channels |
| 3 | C (leader) | BLE | BLE\|AP24\|AP5 | |
| 4+ | extras | | | split PROM24 channels 1–6 / 7–11, or a 2nd BLE |

\*802.15.4 only if `MASK_154` is on. Assignment prefers members whose `caps` say they have a 5 GHz
antenna for WIFI5 / WIFI. `M:o` overrides a member's role by hand. Modes (`M:p`): 0 auto/static
(default), 1 manual, 2 rotate (shift roles every 10 min, useful only when units are physically spread
out), 3 off (everyone runs FULL).

### 8.4 Failover
- Member lost (10 s) → `assignRoles()` for the rest. A new Plan goes out with `planGen+1` (resent
  every 10 s anyway).
- Leader lost on the client (15 s) → the client reverts to FULL locally and logs `op=lost`. On rejoin
  it takes the assigned plan.
- The leader's own C5 link down → the leader keeps planning. Its own C5 just isn't contributing.

## 9. Detection forwarding + merge

### 9.1 Client forwarding (`fleet_client.cpp`, called at the end of `core::runScanCycle()`)
Per local `g_dets[i]`, a sent-table entry `{src, mac[6], pan, rssi, flags, nameHash, lastSentMs}`
(20 B × 96). Send the row if:
- it is new, OR
- |Δrssi| ≥ 6 dB, OR
- flags, name, IE or company ID changed, OR
- more than `FLT_REFRESH_MS` (10 s) since it was last sent.

Rows are packed into DetBatch (16/frame on v2, 2 on v1), at most 6 frames per cycle, 5 ms apart.
RIDs go as a RidBatch each cycle while present. MStat goes every cycle (2 s) and doubles as the
heartbeat. Expected traffic: about 1 frame per cycle steady state, worst case 6 × 1.3 KB per 2 s
(about 6 ms airtime at 12 Mbps, under 1% duty). Rows are raw `Detection` (81 B) so the leader can
re-score with its own DB. Nothing is sent while not joined.

### 9.2 Leader merge (`fleet_leader.cpp` + `host_core`)
- Remote table `RemoteRow { Detection d; uint8_t member; uint32_t lastMs; }` (≈ 86 B) × 256, in
  PSRAM. Upsert by (member, source, MAC, PAN). TTL `FLT_REMOTE_TTL_MS` = 30 s, matching the C5 table
  TTL.
- Remote RIDs: one slot per emitter, merged into `g_rids` after `requestScan` (MAX_RID 8).
- In `core::runScanCycle()`, right after a successful `requestScan()`, call
  `fleet::mergeRemote()`:
  - For each fresh remote row, dedup against `g_dets` by source + MAC (+ PAN for short 15.4).
  - On a match: keep the stronger RSSI, OR the flags, fill blank name/IE/company ID/UUID from the
    remote row, and set `g_detVia[i] |= 1<<slot`.
  - Otherwise append the row (until `MAX_DET`) with `g_detVia = 1<<slot`. Local rows get bit 0.
  - Remote rows go through the same `isSelfDet` / phone-`peerMac` filter.
- **Fleet-only cycle:** when `requestScan()` misses AND the link is down (not just a sticky busy-miss)
  AND remote rows exist, run the pipeline on remote rows alone, so the merged view keeps working when
  the leader's own C5 is dead.
- `MAX_DET`: `#ifndef HOST_MAX_DET` → 96. `[env:xiao_s3*]` sets `-DHOST_MAX_DET=192`. That is about
  270 B/row across `g_dets`/score/label/category/sortIdx/groups/follow/ring/via, so +26 KB internal
  SRAM on the S3; measure first (Q10). Audit `int idx[MAX_DET]` stack arrays (the S3 loop stack is
  8 KB). The CYD stays at 96.
- `DevGroup` gains `uint8_t via` (OR across members). `buildGroups()` fills it.
- Categories, threat tiles, `countBands` and `countTiers` are **merged** (fleet totals). `uniq`
  stays the leader's own stream count.
- **Follow engine:** `updateFollowState()` uses only rows with `g_detVia & 1`. A client's position may
  differ from the leader's, and follow alerts are disabled anyway (`FOLLOW_ALERTS_ENABLED=false`).
- **Alerts:** `g_deauthRecent = max(local, fresh members' MStat.deauth)`, and the same for evil/rid.
  The banner text names the source ("deauth 34/s · via CFLO-CYD-…"). Event lines are written only by
  the unit that observed them (each client logs its own).

### 9.3 DETS v9 (CYD/S3 → phone; not the link version)
- Append field `via` (decimal member mask: bit 0 = the leader itself, bit k = member slot k) to each
  detection row. Older web parsers ignore extra fields.
- Member names come from `FLT=`.

### 9.4 STATUS / CMD
- Part 2 gains `flt=<role>,<joined>/<enrolled>,<ch>` (role 0/1/2). Part 2 is ≤ ~200 B of 252, so
  there is room.
- `FLT=` (LOGDATA, answer to `M:l`): line 1 `OK\t<ch>\t<planMode>\t<maxPayload>`, then per member
  `slot\tsid\tname\tkg\tstate(0 enrolled/1 joining/2 joined/3 lost)\trssi\tage_s\trole\tdets\tbat\tc5link\ttsrc\tpsrc\ttxFailPm`.
  FKs are never listed.

### 9.5 Logging model
- **Every unit logs its own detections to its own SD** (unchanged pipeline; the client logs to the bg
  stream). Provenance stays per file: each file's `meta` line carries that unit's `scanner_id`.
  Nothing is double-counted downstream.
- **The leader skips remote-only rows by default.** `logNewDetections()` skips `!(g_detVia[i] & 1)`
  unless `fltlogr=1`. That option exists for a client with no SD card; such rows add `"via":"<sid>"`
  (contract addition, Q3).
- New event line (readers skip unknown `evt`):
  `{"epoch","ms","lat","lon","evt":"fleet","op":"join|lost|leave|release|fallback|plan|member_join|member_lost","role":"client|leader","peer":"<12hex sid>","slot":k,"n":N,"plan":"ap24+ap5+prom24+prom5"}`.
  scan-analyzer needs it: a WIFI-role client file has **no BLE rows by design**, and this line tells
  the reader why.
- `meta` adds `"fleet_role":"client|leader"` only while in a fleet (contract addition).
- Optional `"pos_via":"fleet"` on rows positioned from a leader fix (Q4).
- **Live-POST (web):** v1 posts only `via & 1` rows (the leader's own; scanner position matches).
  Posting members' rows needs a "Live-POST fleet" contract with per-row `scanner_id`, coordinated via
  REMOTE.md (P6).

## 10. Web app UX
- **New file `js/features/fleet.js`** (new file, so the 10-minute cache can't serve a stale copy).
  DOM wiring goes in `main.js`. Bump `style.css?v=`, `main.js?v=` and `#uiver`. If `auth.js` or
  `connection.js` gain exports, bump `./ble/connection.js?v=` too.
- **Crypto helpers** in a new `js/ble/fleetcrypto.js`: `deriveFleetKey(sid)` and
  `wrapForLeader(leaderSid, memberSid, kg, name)`. They need an `auth.js` export
  `getKey(sid) → CryptoKey` (sign-only use).
- **Settings → Fleet card**:
  - Connected to a CYD or mini in normal mode: "Make this scanner a fleet client" (confirm dialog
    explains the phone link goes off; sends `M:j`).
  - Connected to a mini: "Fleet leader" toggle (`M:L1`/`M:L0`), channel select 1–13 (`M:c:<ch>`),
    plan mode (`M:p:<n>`).
  - **Members** list from `M:l`, polled every 5 s while the card is open: name, state dot,
    role/plan, RSSI, last seen, detection count, battery, C5 link, time/position source. Actions:
    Remove (`M:r:<sid>`), Release (`M:x:<sid>`, confirm "it reboots into normal mode"), Role override
    (`M:o:<sid>:<role>`).
  - **Add member**: pick from stored scanner keys (Scanner keys card) other than the connected leader,
    or scan the member's Phone-key QR first. The app computes FK, wraps it and sends `M:a:…`.
- **Home:** a fleet chip under the summary card ("Fleet · 2/2 joined", from `flt=`). Tap opens the
  Fleet card.
- **Merged list:** each row shows small member chips (initial letter, colour per slot) from `via`.
  The device modal gets "Seen by: <leader> · CFLO-CYD-…". The threat strip/tiles are fleet totals.
- **Scan summary dialog:** adds "Fleet: N members, merged".

## 11. Files

**Create**
- `src/host_common/fleet_proto.h` — §4 wire structs and constants.
- `src/host_common/fleet.h` — facade:
  - Lifecycle: `role()`, `loadRole()`, `setRole(uint8_t)`, `bootCrashGuard()`, `beginClient()`,
    `beginLeader()`, `active()`, `service()`.
  - Detections and plan: `currentPlan(ScanPlan&)`, `mergeRemote()`, `forwardCycle()`,
    `isFleetSelf(const uint8_t*)`.
  - Time and position: `timeFix(uint32_t* utc, uint32_t* atMs)`,
    `posFix(float*, float*, uint32_t* ageMs)`.
  - Phone commands and UI: `handlePhoneCmd(const char*)`, `statusField(char*, size_t)`,
    `onRekey()`, `uiSnapshot(FleetUi&)`.
- `src/host_common/fleet_link.cpp` — Wi-Fi/ESP-NOW bring-up, random MAC, peers, RX pool/queue, send
  + stats, HMAC/derivations, seq window.
- `src/host_common/fleet_client.cpp` — search/hello/joined/lost state machine, Sync/Plan/Ctrl
  handling, delta forwarding, MStat.
- `src/host_common/fleet_leader.cpp` — `fltmem` store, probes, Hello matching, Join, member table,
  `assignRoles`/failover, remote table + merge, `M:` commands, `FLT=` reply.
- `webapp/js/features/fleet.js`, `webapp/js/ble/fleetcrypto.js`, `webapp/scss/_fleet.scss`.
- `docs/fleet-mode.md` (this document).

**Modify**
- `lib/link_protocol/link_protocol.h` — v12 `ScanPlan` + `ScanConfig` (§8.1).
- `src/c5/main.cpp` — `applyPlan`, runtime hop arrays, `planAllows` / `bleWanted` gating (§8.2).
- `src/host_common/host_link.cpp` — `cfg.plan`; remote filter shares `isSelfDet`.
- `src/host_common/host_core.{h,cpp}`:
  - `HOST_MAX_DET` and `g_detVia[MAX_DET]`; `DevGroup.via`.
  - Fleet source 4 in `posSnapshot()` / `timeSource()` / `epochLocal()` / avail masks.
  - `isSelfDet()` calls `fleet::isFleetSelf()`; `updateEvents()` uses member maxima.
  - `runScanCycle()`: `mergeRemote` after `requestScan`, fleet-only cycle, `forwardCycle` at the end.
  - Follow engine filters on via bit 0.
- `src/host_common/host_status.cpp` — `flt=` in part 2; DETS v9 `via`.
- `src/host_common/phone.{h,cpp}` — CMD `M` → `fleetCmdRequested(char*, size_t)` (one slot,
  consume-on-read); `loadTz()` split out of `begin()`; `setTzOffsetMin()`; a `devkey::begin()` call
  path usable without NimBLE.
- `src/host_common/host_phone_cmds.cpp` — dispatch `M:` → `fleet::handlePhoneCmd`, reply via
  `logNotify("FLT=…")`; `M:j` → `setRole(1)` + deferred restart.
- `src/host_common/devkey.{h,cpp}` — `fleetKey(out16)`.
- `src/host_common/host_log.{h,cpp}` — `logFleetEvent()`, meta `fleet_role`, remote-row skip / `via`
  option.
- `src/host_common/host_serial.cpp` — `fleet`, `fleet leave`, `fleet client`, `fleet leader on|off`.
- `src/host_common/webshare.cpp` — AP_STA on the fleet channel; `stop()` keeps STA (§3.5).
- `src/host_common/host_board.h` — `bool fleetLeaderCapable()`.
- `src/cyd/main.cpp`:
  - `setup()`: fleet-client branch (§1.2).
  - Screens `SCR_FLEET`, `SCR_FLEETLEAVE`, `SCR_FLEETENTER`; Settings item "Fleet client…".
  - Status-bar fleet glyph and fleet-time clock tint; Phone-key back target.
  - Fleet-mode `loop()` root (forced scan; `fleet::service()` every pass).
- `src/xiao_s3/main.cpp` + `host_board.cpp` — `beginLeader()` / `beginClient()` after `phone::begin()`;
  `fleet::service()` in `loop()` and `runDownload()`; `fleetLeaderCapable()=true`.
- `platformio.ini` — `-DHOST_MAX_DET=192` on `[env:xiao_s3]`. Diff for IDE clobber; keep ArduinoJson ^6.
- `CLAUDE.md` (architecture + fleet section) and HANDOFF.

## 12. Cost estimate

| Env | Flash | RAM / heap |
|---|---|---|
| cyd | +~25 KB (ESP-NOW ~8, fleet ~12, screens ~4) → ~1.52 / 3 MB (51%) | Normal mode: static < 0.3 KB, heap unchanged (~58 KB). **Fleet client: ~105 KB est. − ~6.5 KB fleet buffers ≈ 98 KB free steady**, largest block ~65 KB. Plenty. |
| xiao_s3 / _rgb | +~30 KB → ~1.37 / 3 MB | Wi-Fi STA + ESP-NOW ~40–50 KB (part may land in PSRAM via `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). RX pool 23.5 KB + remote table 22 KB in PSRAM. HOST_MAX_DET 192: +26 KB internal. **Measure S3 free internal heap in P1 before raising MAX_DET.** |
| c5 / xiao_c5 | +~1.5 KB | +~50 B (plan + hop arrays) |

No env comes near its 3 MB app partition.

Framework facts checked (Arduino-ESP32 3.3.12, IDF 5.5): ESP-NOW maximum payload is 250 B for v1 and
1470 B for v2, and `esp_now.h` is identical on ESP32 and S3. The prebuilt libraries allow 7 encrypted
peers and 20 peers total. The C5's `promisc.cpp` handles only management subtypes 4, 5, 8, 10 and 12,
so ESP-NOW action frames (subtype 13) never show up as detections.

## 13. Phases (each ends with tester build + reviewer; security pass on P2 and P5)
Bench: **mini #2 = leader, CYD = client** for P1–P5. **Mini #1** is the third unit for N=3 roles and
failover (P4/P5).

- **P1 — transport + boot modes (no crypto, `-DFLEET_DEBUG` cleartext ping frames).**
  - Build: `fleet_proto.h`, `fleet_link.cpp`; CYD fleet-client boot path, `SCR_FLEET` skeleton with
    Leave, Settings entry, serial `fleet`, crash fallback, `loadTz` split, devkey order; S3 leader
    bring-up with NimBLE; webshare AP_STA.
  - Envs: cyd, xiao_s3, xiao_s3_rgb. The C5 is untouched.
  - Checks: CYD heap ≥ 95 KB steady for 30 min with the scan running; S3 internal/PSRAM heap logged;
    random MAC confirmed by sniffing (the frame's addr2 ≠ `scannerIdHex`); `esp_now_get_version`;
    delivery ratio leader⇄client at 250 / 1300 B and 1 / 12 Mbps with the phone idle, during DETS,
    and during a BLE log download; SoftAP download while the fleet is up; C5 link loss = 0 on both.
- **P2 — enrollment + auth.**
  - Build: FK, `M:a/r/l/x/L/c`, `fltmem`, Probe/Hello/Join/JoinAck, LMK peers, seq window,
    CTRL RELEASE/UNJOIN; web `fleet.js` + `fleetcrypto.js` (add/list/remove/release).
  - Checks: join; wrong key rejected; client rekey → no rejoin → re-add works; reboot either side →
    auto-rejoin; release reboots the CYD normal; replay of a captured frame dropped (bench harness on
    the spare S3, own fleet only); probe stops when everyone is joined; no sid/name in any cleartext
    frame (sniff).
- **P3 — time/position push.**
  - Build: Sync, source 4, `selfMacs`/`isFleetSelf`, `fleet` evt lines, meta `fleet_role`, FLEET
    screen sources.
  - Checks: a CYD with no NEO-6M gets dated rows after join; the timesync anchor lands; tz correct;
    leader BLE MAC not in the client's log.
- **P4 — link v12 scan plan.**
  - Build: link_protocol, C5 `applyPlan`, `requestScan` plan, `assignRoles`, Plan frame, failover.
  - Envs: **all of** c5, xiao_c5, cyd, xiao_s3, xiao_s3_rgb; flash all units together.
  - Checks: non-fleet units behave identically (zero plan); C5 console shows plan ids; the WIFI member's
    `mgmt/data` capture-health counts rise vs FULL; the BLE member's BLE count rises; kill a member →
    roles reassigned within ~12 s; leader off → client back to FULL within 15 s.
- **P5 — forwarding + merge + DETS v9.**
  - Build: DetBatch/RidBatch/MStat, delta table, remote table, `mergeRemote`, `g_detVia`, fleet-only
    cycle, HOST_MAX_DET, alert maxima, transfer-loop `fleet::service`; web via chips / seen-by.
  - Checks: a device only the CYD hears shows on the phone with "via CYD"; overlap dedups to one row
    with both bits; client power-off → its rows age out at 30 s; airtime/frame counts match the budget.
- **P6 — contracts.** Agree on the `fleet` evt, meta `fleet_role`, `pos_via`, Live-POST fleet
  (per-row `scanner_id`) and the `fltlogr` `via` key with scan-analyzer (REMOTE.md + HANDOFF #11).
  Then implement the web live-POST of member rows.

Protocol/version bumps: link v12 (P4), DETS v9 (P5), FW_VERSION 0.20.0 (P1) → 0.21 (P4) → 0.22 (P5).
Respect "no flash until BLE auth" (already satisfied; Phase A shipped).

## 14. Risks
1. **S3 coex loss** (ESP-NOW vs a busy NimBLE link). Mitigated by unicast acks, delta + 10 s refresh,
   and connection-interval tuning. Measure in P1.
2. **Near-field desense** of each unit's own C5 by host TX on the same band. Mitigated by low TX
   power, under 1% duty, and the fleet channel being only one of 11 hopped.
3. **ESP-NOW encrypted-peer details:** v2 + encryption together; unencrypted Probe/Join to a peer
   that is later encrypted; the JoinAck race. Covered by retries; verify in P2.
4. **`esp_wifi_set_mac` timing** on the Arduino stack (P1).
5. **CYD Wi-Fi + UART** over long runs (the probe was 90 s). 30-min soak in P1; REF_TICK fallback.
6. **Merged-table cap:** CYD clients don't merge. On the leader, 96 → 192 rows needs measured S3 RAM.
7. **Lost-leader churn while the leader runs long transfers.** Mitigated by `fleet::service()` from
   the transfer loops.
8. **Downstream misreading** of role-limited client logs (no BLE rows). Mitigated by the `fleet`
   evt + meta.
9. **Cleartext BLE (Phase A)** while sending `M:a`. FK is wrapped under the leader key; an active BLE
   injector after auth remains the accepted Phase A limitation.
10. **IDE clobber of platformio.ini** when adding `HOST_MAX_DET`.

## 15. Open questions for the owner
Q1. Mini as fleet **client**: keep its NimBLE phone link up? (Recommend yes.)
Q2. Default N=2 split: CYD = Wi-Fi-only (2.4+5 promisc + AP scan, 802.15.4 if on), mini leader = BLE
    + AP scan? Or the reverse, based on which C5 has the better 5 GHz antenna?
Q3. Should the leader log members' remote-only rows (default off, `fltlogr`)? And approve the
    scan-analyzer contract additions: `evt:"fleet"`, meta `fleet_role`, per-row `via`/`scanner_id`
    for live-POST of member rows.
Q4. Client position precedence: its own live onboard GPS above the leader's pushed fix (recommended)?
    Mark borrowed positions with `"pos_via":"fleet"`?
Q5. Expected separation: same vehicle/bag (low TX power ~8–13 dBm, 12 Mbps) or spread out (higher
    power, 1 Mbps, and rotate mode becomes useful)?
Q6. Fleet channel: default 1, user-selectable 1–13? (The leader's SoftAP moves to it while a fleet
    is active.)
Q7. Member cap of 4 clients per leader (limit 7 encrypted peers): enough?
Q8. Allow the remote **Release** (`M:x`) from the leader, in addition to on-device Leave?
Q9. Approve the automatic boot-loop fallback (3 consecutive crash boots → leave client mode)?
Q10. Raise S3 `HOST_MAX_DET` 96 → 192 (pending a P1 heap measurement)?
Q11. MAC randomization per boot (recommended) or also per re-join?
Q12. Client on leader loss: revert to the full standalone plan (recommended) or keep its assigned role?
Q13. Fleet-client CYD: scanning always on regardless of the bg-scan setting (recommended)?

## 16. Owner decisions (2026-10-31) — these SUPERSEDE the sections above where they conflict

Decided: Q1 yes · Q2 default split, user-editable · Q5 units a few feet apart · Q6 channel 1 default,
user-selectable · Q8 remote release + auto-reassign/restore · Q9 approved · Q10 yes · Q11 boot + rejoin ·
Q12 yes · Q13 yes · Q3 the leader logs everything (store-and-forward, §16.7) · Q7 stay under the encryption cap (§16.5).

### 16.1 Roles are a user-editable layout (Q2, Q8)
- The default is the §8.3 template. The web Fleet card has a **layout editor**: per member, a role
  (FULL / WIFI / WIFI24 / WIFI5 / BLE / custom phases + channel masks), plus presets ("Default",
  "Even split", "All full"). It is saved on the leader as the **saved layout** (`fltlay` NVS blob,
  keyed by member sid, so it survives reboots and slot changes).
- **Drop-out → temporary layout:** a member silent past the grace period (`fltgrace`, default 30 s,
  user-set 10–300 s; longer than the 10 s "lost" mark so a quick reboot rejoins without a shuffle)
  triggers `assignRoles()` over the units still present. The missing coverage is folded into
  survivors by the same template rules. Logged `evt:"fleet","op":"plan","why":"drop"`.
- **Rejoin → restore:** once the member has been back and stable for 10 s (hysteresis against
  flapping), the leader re-sends the **saved** layout. Logged `op:"plan","why":"restore"`.
- If the leader itself drops, clients revert to FULL (Q12), and resume their saved role on rejoin.

### 16.2 Radio settings for units a few feet apart (Q5)
- Default TX power **8 dBm** (`FLEET_TXPWR_QDBM` 32) at **12 Mbps**: plenty for 1–2 m and the least
  desense on each unit's own C5. User-adjustable on the Fleet card (advanced).
- **Rotate mode (`M:p` 2) is dropped**: it only helps spread-out units. Modes: auto / manual (layout) / off.

### 16.3 Channel (Q6)
Default **1**, user-selectable 1–13. The channel is **not a security control**: secrecy and
authenticity come from the keys (§5); any listener can hear every channel. The cost of channel 1 is
congestion (the most common AP channel), which only adds retries. The Fleet card suggests 1 / 6 / 11.
The leader announces it in Sync; clients follow (`CTRL_SETCH`) and fall back to the §1.5 search.

### 16.4 MAC randomization at boot AND each rejoin (Q11)
This is safe for rejoin because **nothing stores a member's radio MAC**:
- The leader identifies a member only by its HMAC tag in Hello (§5.3) and learns the current MAC from
  that frame's source address.
- The client learns the leader's current MAC from each Probe.
- Changing the MAC requires stopping the Wi-Fi interface. The client does it only while **not
  joined**, just before answering a new Probe: `esp_now_deinit` → `esp_wifi_stop` →
  `esp_wifi_set_mac` → `esp_wifi_start` → re-init (~100–300 ms, once per rejoin). A joined client
  never changes its MAC.
- The leader randomizes **only at boot** (rotating it would drop every joined member at once).
- **P1 check:** time the stop/start cycle, and confirm a rejoin succeeds after both a client reboot
  and a leader reboot. If either fails, fall back to boot-only randomization (owner-approved fallback).

### 16.5 Member cap (Q7) — DECIDED: stay under the native ESP-NOW encryption cap
- Keep ESP-NOW's built-in peer encryption (LMK/CCMP, §5). The prebuilt libraries allow **7
  encrypted peers**; the broadcast Probe peer is unencrypted and doesn't count.
- **`FLEET_MAX_MEMBERS` = 6 clients (7 units including the leader).** That leaves one encrypted slot
  free, so a rejoining member's new entry can be added before its stale entry is deleted.
- A CYD client needs only 1 encrypted peer (the leader).
- No app-layer encryption and no IDF rebuild.

### 16.6 Fleet position = one entity, user-chosen source (Q4)
- The leader picks **one fleet position** and pushes it in Sync. **Every unit stamps every row with
  it**, so a fleet scan has one consistent track.
- **Source setting** (Fleet card): **Phone (default)** / Leader GPS / a named member's GPS.
- If the chosen source has no live fix, the fallback order is phone → leader GPS → any member with a
  live fix → held last fix (`fix_age`, as today).
- The chosen member's fix travels up in MStat (`lat/lon`, already there) and back down in Sync.
- Time follows the same rule (phone default, else the leader's best clock).
- **Each unit's own GPS is still logged, marked as not used:** every unit with a fix writes
  `{"epoch","ms","lat","lon","evt":"devpos","scanner":"<12hex sid>","dlat":…,"dlon":…,"sats":N,"used":0|1}`
  every 30 s (and on ≥ 25 m movement).
  - `lat/lon` is the fleet position, as on every line. `dlat/dlon` is that device's own fix.
  - `used` = 1 only on the unit currently supplying the fleet position.
  - Readers skip `devpos` like other `evt` lines; it's for diagnostics and future per-device geometry.

### 16.7 Logging + upload (Q3) — DECIDED: the leader logs everything (store-and-forward)
Goal: every row from every unit reaches the server from **one** phone connection, without the
leader doing heavy work, and with accidental duplicate uploads detectable.

**Why not the alternatives:**
- *Phone pulls each client's file through the leader (relay, no leader storage):* needs a file-transfer
  protocol over ESP-NOW + BLE, retries and resume. It's the most complex option and the slowest
  (BLE-bound).
- *Live-POST only:* DETS carries only the top 12 rows, so data would be lost.
- *Leader re-logs its merged live table:* the leader's dedup/relog timing differs from the client's,
  so rows can't be matched 1:1 against a client file, which breaks duplicate detection.

**Decided:** clients forward their **log rows verbatim**, and the leader appends them.
- Each unit (client and leader) still writes its own SD log exactly as today. On a client, that file
  is its **backup**.
- Every row a client logs also goes into an **outbound queue** to the leader: the exact NDJSON line,
  plus an origin id. That is a second ESP-NOW stream (`Msg::LogRows`, encrypted, acked), separate from
  the live detection deltas that feed the leader's live view.
- **Reliable + resumable:**
  - The client tracks `ackedThrough` (row number) per session file.
  - The leader acks after each SD append.
  - After a dropout or a leader reboot, the client re-reads its own SD file from `ackedThrough + 1`
    and resends, so nothing is lost while out of range.
  - The leader dedups by origin id, so a resend is harmless.
- **Leader work stays small:** it appends lines it already received to a dedicated fleet stream
  (`/logs/fleet-…jsonl`), with no scoring and no dedup tables (beyond a small per-member
  last-row-number). Data rate is about 1 MB/hour per unit, well inside the S3 + SD budget and the
  512 MB cap. One Data-page download or upload of the leader's fleet scan covers every unit.
- **Provenance on every forwarded row:** the leader adds `"scanner":"<origin sid>"`,
  `"ofile":"<origin file basename>"` and `"orow":N` (the row's line number in the origin file).
  The leader's own rows get `scanner` = its own sid.
- **Fleet scan id:** at the first join the leader generates a random 64-bit `fleet_id` per fleet
  session and sends it in Sync. Every unit's `meta` line gains
  `"fleet_id":"<16hex>","fleet_role":"leader|client","fleet_leader":"<leader sid>"`; the leader's fleet
  stream also lists the members.
- **Duplicate / partial detection on the server:**
  - A client file carries `fleet_role:"client"` + `fleet_id`. Each of its rows maps to exactly one
    leader-file row via (`scanner`, `ofile`, `orow`).
  - **Leader file already uploaded** → every client row is a known duplicate. The server can drop
    them, or only confirm that they match.
  - **Leader file not uploaded yet** → the client file is **partial data of fleet X**. The server
    stores it tagged that way, and folds it in or dedups it when the leader file arrives.
    Order of upload never matters.
  - Rows a client logged while the leader was unreachable, and never forwarded (e.g. its battery died
    first), exist only in the client file. The server keeps those as the only copy.
- These are scan-analyzer contract additions (meta keys, row keys `scanner`/`ofile`/`orow`, `devpos`
  and `fleet` events). Coordinate via REMOTE.md before P6. scan-analyzer's existing row-level dedup
  (2026-10-22) makes re-uploads cheap already.
- **Phase change:** adds a **P5b** (LogRows stream + ack/resume + fleet stream + meta) after P5.

## 17. P2 as built (2026-10-31) — deviations and specifics (supersede §4-§5 / §9.4 where they differ)
Files: `fleet_internal.h` (shared internals), `fleet_leader.cpp`, `fleet_client.cpp`; transport and crypto
helpers stay in `fleet_link.cpp`. Web: `js/ble/fleetcrypto.js`, `js/features/fleet.js`, the Settings **Fleet** card.

- **App-level tag on every session frame.** JoinAck, Sync, MStat and Ctrl carry `HMAC-SHA256(SK, header+payload)[0..8)`
  appended after the struct (on-air length = `sizeof + 8`). It is checked before the seq window, so a forged frame
  can't advance it. Reason: whether ESP-NOW drops UNencrypted frames that spoof an encrypted peer's MAC is
  unconfirmed. Without the tag, a forged Ctrl RELEASE could reboot a member. ESP-NOW CCMP (LMK = SK) still
  provides secrecy.
- **Sync = 111 B:** adds `char leaderName[20]` (encrypted) so a client can show its leader. P2 fills only `ch`,
  `leaderSid` and `leaderName`; time, position and selfMacs are P3.
- **MStat in P2:** only `heapKB`, `txFailPm`, `bat=-1`. It is the client's heartbeat. The leader marks a member lost
  after 10 s without one; the client marks the leader lost after 15 s without a Sync.
- **Hello replay guard:** the leader skips a Hello whose `nc` started the member's current session.
- **CMD framing:** `M<seq>:<op>[:args]` (seq 1-255, as with the explorer's `Z`). Reply: LOGDATA `FLT=<n>:<seq>` + `<n>`
  bytes, `OK\t…` / `ERR\t<why>`. `M:` without a seq is still accepted (seq 0).
  - `l` → `OK\t<role>\t<active>\t<ch>\t<planMode 0>\t<maxPayload>`, then one line per member:
    `slot\tsid\tname\tkg\tstate\trssi\tage_s\trole\tdets\tbat\tc5link\ttsrc\tpsrc\ttxFailPm\tfw`
    (state 0 enrolled / 1 joining / 2 joined / 3 lost; P2 sends 0 / -1 for the plan + scan columns).
  - `a:<sid>:<kg>:<w>:<wrapped>:<tag>:<name>`. The name is 1-20 of `[A-Za-z0-9 _.-]`. The tag covers the exact `kg`
    digits sent.
  - `r:<sid>` remove (CTRL_UNJOIN ×2 if joined), `x:<sid>` release (CTRL_RELEASE ×3, joined only), `c:<1-13>`.
    With members joined, `c` sends CTRL_SETCH and the leader follows 300 ms later.
  - `L1` / `L0` leader on / off (also how a mini CLIENT leaves), `j` become a client. A role change replies first,
    then reboots.
  - Audited as `phonecfg` `fleetadd` / `fleetrm` (path = member sid). Fleet events go to `evt:"fleet"` with ops
    `join`, `lost`, `unjoin`, `release`, `member_add`, `member_remove`, `member_join`, `member_lost`,
    `member_release`, `client_set`, `start`, `leave`.
- **STATUS part 2:** `flt=<role>,<joined>/<enrolled>,<ch>` (a client reports `0/1` or `1/1`).
- **Probing:** only while an enrolled member is un-joined: every 3 s for 2 min from the first miss, then every 15 s.
  A rebooted client waits for the leader's 10 s loss mark before probes resume (~10 s rejoin). A released member
  stays enrolled (shown "lost"), so slow probing continues until it is removed.
- **Client search:** dwell 9 s on the saved channel, then 1 / 6 / 11. It saves the channel it joined on. A new MAC
  is drawn before the first Hello after any session ends.
- **Bench-only (`-DFLEET_DEBUG`):** `fleet fk` prints this unit's FK as a ready `fleet add <sid> <fk> <name>` line;
  `fleet add …` stores a member directly; `fleet m <M-command>` runs a phone command over serial; the serial line
  buffer is 176 B in these envs (48 in release).
- **Bench results (2026-10-31, mini #2 leader + CYD client):** all PASS.
  - Join ~1 s after enrollment; the leader stops probing once all members are joined.
  - Wrong FK: never joins. Re-add with the correct FK: joins in 4 s.
  - Channel change: members follow and stay joined.
  - Leader reboot: the client marks it lost at 15 s, then rejoins in 2 s from a fresh MAC.
  - Client reboot: rejoins ~9 s after the reset.
  - Remove → the client searches; release → the CYD reboots normal with its phone link.
  - The real `fleetcrypto.js` (run in Node) built an `M:a` the leader accepted, and the CYD then joined with its
    on-device FK (web ≡ firmware). A tampered name → `bad tag`; a replayed `M:a` → `replayed`.
- **Not yet done:** web-app enrollment from the phone (card built, untested on a phone); the sniffer check that no
  sid/name/real MAC goes out in the clear (P1 step 9 + P2); a replay of captured session frames (spare-S3
  harness).

### 17.1 Review + security pass (2026-10-31) — fixes applied, all re-bench-tested
- **HMAC fail-open (both reports, high):** every `Hmac::finish()` is now checked (`[[nodiscard]]`). On a crypto error
  it fills the output with random bytes, never zeros, so a forgotten check still can't be matched. The context lives
  inside the object (no per-call `new`).
- **M:a replay (medium):** the tag now binds a persistent enrollment counter (`CFLO-FLT-ADD-2`, NVS `fltactr`, sent
  as the 7th field of the `M:l` header). `M:a` = `a:<sid>:<kg>:<ctr>:<w>:<wrapped>:<tag>:<name>`; the counter
  advances on each add. Bench: replay at once → `stale`; replay after a leader reboot → `stale`.
- **Hello replay vs a live session (medium):** a member with a session only rejoins through a probe sent after that
  session began.
- **Channel moves:** CTRL_SETCH is sent ×3. A searching client alternates the preferred pass (saved / 1 / 6 / 11) with
  a full 1-13 pass. Bench: ch 3 and back OK.
- **A live SoftAP is never moved:** `M:c` is refused while the AP is up. A client defers SETCH and stops channel
  hopping while its AP is up. `M:c` is also refused on an active client.
- **Encrypt / delete only after frames LEAVE the radio:** the send callback records unicast completions
  (`sendDone`). The leader clears stale completions before a Ctrl burst, then waits for them. The bench caught a
  release lost to a stale Sync completion; fixed and re-tested.
- **Failure handling:** if `rerandomizeMac` fails, the unit reboots (it neither stays offline nor reuses its MAC).
  RAM and NVS stay consistent (NVS is written first). A failed Hello send deletes its peer.
- **Hygiene:**
  - Junk Hellos can't eat the rate budget (the freshness check runs first).
  - The member `fw` string is sanitised before it enters the TSV.
  - Wrapped key material is zeroed in `phone.cpp` and `handlePhoneCmd`.
  - `flt=` moved after the AP credentials in STATUS part 2 and gained a `canLead` field.
  - The leader re-randomizes its MAC every 15 min while nobody is joined (anti-tracking).
  - The web key store opens without a fixed version.
- **Accepted / documented:**
  - Join's cleartext `slot` / `nMembers`.
  - Probes continue at 15 s for a released (still enrolled) member.
  - Stored FKs sit in NVS like the devkey; revoking one means rekeying that member.
  - Member slot numbers go stale after a removal until rejoin — **P4 must re-send slots** (Plan).

## 18. P3 as built (2026-10-31) — fleet time + position (supersedes §7 where they differ; follows §16.6)
- **Sync, filled by the leader every 2 s:**
  - `utc` (= `epochLocal()` + tz back to UTC) and `tzOffMin` (`phone::tzOffsetMin()`).
  - `tsrc`/`psrc` = the leader's sources. `lat`/`lon` (deg·1e7) and `fixAgeMs` = the leader's `posSnapshot()`.
  - `flags`: bit0 time valid, bit1 position valid, bit2 position live.
  - `selfMacs`: the leader's BLE MAC, its connected phone's, its SoftAP BSSID while up, and each joined member's
    `JoinAck.bleMac` (a mini client sends its own; a CYD client has none).
- **One fleet position (§16.6):** host_core source **4 = fleet**.
  - A joined **client** stamps the fleet position ahead of its own sources while the fix is under 15 s old.
  - The **leader** uses a member's live GPS (`MStat.gpsFix`/`lat`/`lon`, under 10 s) only when it has no live fix of
    its own. Held fallback = the most recent of phone / onboard / fleet.
  - Device-only ignores the fleet, like the phone.
- **Time:**
  - Under prefer-phone, a client's fresh fleet clock (a Sync within 15 s) ranks first.
  - Under prefer-device it ranks below fresh GPS and the RTC.
  - After the leader is gone the latched fleet clock free-runs as the last resort.
  - The tz is taken from Sync (`phone::setTzOffsetMin`, which writes NVS only on change).
  - The first fleet time triggers the existing session rename + `timesync` anchor. Bench: rename 0.4 s after join,
    clock = PC clock.
  - A mini client's RTC is disciplined from the fleet clock.
- **STATUS:** `tsrc`/`gpssrc` can now be **4**. `tav`/`gav` gained **bit2** (fleet available). The web source icons
  draw 4 as a device source, with a "fleet" tooltip.
- **Self filtering:** `isSelfDet()` also drops `fleet::isFleetSelf(mac)`: a client the leader's `selfMacs`, a leader
  its members' BLE MACs.
- **MStat now carries** `bat`, `c5Link`, and the member's OWN live onboard fix (`gpsFix`/`lat`/`lon`).
- **Deferred to P6 (scan-analyzer contract first):** the `devpos` per-unit GPS line, the meta keys
  `fleet_id`/`fleet_role`/`fleet_leader`, and the member-chosen fleet-position source setting.
- **Position bench (2026-11-01):** with the owner's phone on the leader, the CYD client shows `pos src 4 live` (fix
  age 4 s) and receives 2 self MACs (the leader's BLE + the phone). The owner confirmed the CYD screen reads `Pos: fleet`.
  Self-MAC drops are verified by inspection only.

### 18.1 P3 review (2026-11-01) — fixes applied
- **REV-001: "live" is the leader's call.** Sync flags bit2 (position live) is latched. A client's fleet fix counts as
  live only while the leader says so AND its Syncs are current, so every unit agrees on live vs held. `fix_age` still
  uses the leader's age plus transit.
- **REV-002: no fake tz.** Sync flags **bit3 = tz known**. It is set only when the leader's tz came from a phone (now
  or saved in NVS) or from its own leader. Clients ignore the tz otherwise, and never overwrite a real one with 0.
- **REV-003** was already handled (the CYD fleet-client boot calls `phone::loadTz()`).
- **REV-004:** the RTC write-back takes its source from `timeSource()` (the same ranking the logs use).
- **REV-005:** documented. Position ignores the prefer-device setting on a client (one shared track), but time
  does not.
- **REV-006/007:** the leader's member-GPS fallback is **sticky** (it keeps the same member while that fix is live)
  and returns the last member fix with its true age for the held fallback.
- **REV-008:** MStat is now **42 B** (`phoneMac[6]`: a mini client's own phone). The leader adds members' phones to
  `selfMacs`, and its own `isSelf` also covers members' phones and its own SoftAP BSSID.
- **REV-009:** Sync flags bit0 is set only for UTC ≥ 2023, the same bar the client applies.
- **Re-bench:** rejoin 4.5 s, time src 4 with the leader's tz, log renamed. Both boards must flash together (MStat
  size).

## 19. P4 as built (2026-11-01) — coverage split (link v12)
- **Link v12:** `ScanConfig` = `{sources, dwell_ms, ScanPlan plan}` (21 B; `ScanPlan` 18 B as in §8.1). It is sent on
  every StartScan; all-zero = legacy. Every host and C5 must run v12 together (mini #1 is still v11 as a pair: it works
  on its own but can't mix halves).
- **C5:**
  - `planAllows()` / `bleWanted()` gate every phase.
  - The hop lists (2.4 / 5 GHz / 802.15.4) and window / dwell / gap timings are rebuilt from the plan (`buildPlanRuntime`).
  - A new plan ends any passive window it no longer allows.
  - A plan with 5 GHz but no 2.4 GHz capture enters the 5 GHz window directly (legacy never does).
  - Each plan change prints `[C5] plan id=… phases=…`.
- **Roles** (`fleet::planForRole`, §8.3):
  - WIFI: AP + PROM24 + PROM5 (+15.4), 6 s / 5 s windows, 0.5 s gap, no BLE.
  - WIFI24: AP24 + PROM24 (+15.4), dwell 200.
  - WIFI5: AP5 + PROM5, 6 s.
  - BLE: BLE + AP scan, no passive windows, so BLE is never paused.
  - FULL: zeros.
- **Layout (`assignRoles`, every tick):**
  - N=2: member WIFI + leader BLE. N=3: WIFI24 + WIFI5 + leader BLE. A 3rd+ member runs FULL. Mode `M:p:3` = everyone
    FULL (NVS `fltpmode`); `M:p:0` = auto.
  - A member counts once joined ≥ 10 s.
  - It keeps counting through a 30 s grace after it is lost, or while it re-joins holding a role, so a quick reboot
    never reshuffles.
  - Plans are re-sent every 10 s. A client that loses its leader reverts to FULL.
  - `M:l` reports each member's role (column 8) and the leader's own role + plan mode in the header.
- **Bench (2026-11-01, mini #2 leader + CYD client, all four boards on v12):**
  - Roles applied on both C5s (`0xa3` BLE leader, `0x9f` WIFI client). The CYD's BLE count went 20 → 0; the mini's
    probes went 17 → 0.
  - CYD held off: lost at 8.7 s, roles kept through the 30 s grace, then the leader went FULL. Released: FULL until
    stable 10 s, then roles restored.
  - Leader crash: the client went FULL at +14 s and rejoined 1.2 s later; roles were back 10 s after that.
  - Quick CYD reboot: rejoined with its role and **no** plan change. (A transient while the member was re-joining was
    found and fixed: joining members holding a role keep counting.)
- **Consequence until P5:** each unit's own table / log / phone view covers only its share. The leader's phone view
  lacks the client's Wi-Fi-only finds (they are on the client's SD). Use `M:p:3` (everyone FULL) to field-test
  without P5.
- **Deferred:** the web layout editor + saved layout (`fltlay`, §16.1), the `M:o` per-member override, a user-set
  grace (`fltgrace`), the 4+-member channel split, and the 5 GHz-caps preference (every C5 has 5 GHz).
  FW_VERSION stays 0.20.0 on the bench builds; bump to 0.21.0 at the release flash.

### 19.1 P4 review (2026-11-01) — fixes applied
- **REV-001/008:** a joined member that sends a new Hello (a reboot faster than the 10 s loss mark) gets `lostAtMs`
  stamped, so the role grace applies (no reshuffle) and caps it at 30 s.
- **REV-002:** a released member gives its role back at once. Bench: the leader went FULL 0.1 s after the release.
- **REV-003:** MStat is now **43 B** (`srcMask`). Roles go only to units that can run them:
  - the leader must have BLE on;
  - a Wi-Fi member needs PRB plus a Wi-Fi band;
  - WIFI5 prefers a member with 5 GHz on;
  - anyone else runs FULL.
- **REV-004:** C5 5 GHz passive capture needs PRB as well (as in legacy), on both the direct-entry and the plan-change
  paths.
- **REV-005/006:** uniform BLE stop guards. A volatile `g_planPhases` copy is the only plan state the NimBLE task reads.
- **REV-009:** ch 12/13 are allowed by design (receive-only); documented on the struct.
- **REV-010:** a member reporting an older `planGen` gets the Plan re-sent at once.
- **Deferred (REV-007):** restricting a split role's AP scan to its own band. It needs a band-select API verified on the
  C5 first; currently each role still runs the dual-band AP scan and drops the other band's results.
- Re-bench: roles, then release (leader FULL at once), then rejoin (roles back after 10 s). All hosts + C5s flash
  together (MStat size).

## 20. P5 as built (2026-11-01) — forwarding + merge + DETS v9
- **Client** (`fleet::forwardCycle()`, end of each successful `runScanCycle()`):
  - A heap sent-table (`MAX_DET` × ~32 B) forwards a row when it is new, its RSSI moved ≥ 6 dB, its flags / name hash /
    IE / company ID changed, or every 10 s.
  - Rows go as raw `Detection`s in sealed **DetBatch** frames (16/frame on v2), ≤ 6 frames per cycle, 5 ms apart. A
    capped cycle resumes at the next row next time. Remote-IDs go as a **RidBatch** every cycle while present.
  - The sent-table resets on each new session.
  - MStat now carries `dets` (the last successful cycle), `deauth`, `evil`, `rid`.
- **Leader:**
  - **Remote table:** 768 rows in PSRAM (20 s TTL, members refresh every 10 s), keyed by member + source + MAC (+ PAN
    for a short 802.15.4 address). Member RIDs: 16 slots. RX pool 48 slots (PSRAM).
  - **`fleet::mergeRemote()`** runs right after `requestScan()`:
    - a row the leader also heard keeps the leader's **own** values; it gains the member's `g_detVia` bit, and the
      member's strongest RSSI / flags go to display-only `g_detFleetRssi` / `g_detFleetFlags`;
    - other member rows are **appended** with only the member's bit;
    - the same self / phone filters apply. RIDs merge the same way (`g_ridVia`).
  - **Fleet-only cycle:** when its own C5 is past the sticky window and member rows are fresh, the pipeline runs on the
    member rows alone at the normal 2 s cadence.
  - **No coverage split while the leader's own C5 is down** (it holds the BLE share); everyone runs FULL until it is
    back. This deviates from §8.4's "the leader keeps planning", found on the bench.
- **What uses which rows:**
  - **Merged (fleet-wide):** groups / `DevGroup.via`, categories + HOME / web threat tiles, band + tier counts, the
    tier LED, the DETS phone list, STATUS `deauth`/`evil`/`rid` (max of local and fresh members' MStat).
  - **Own only (bit 0):** the SD log (detections, `lastseen`, RID events, the evil-twin BSSID list), the follow
    engine, `uniq`, and the deauth / evil-twin event lines + LED (`updateEvents`). Each unit logs what it heard; nothing
    is double-counted, and **no log-schema change** (the `via` / `pos_via` / fleet meta keys wait for P6).
- **`HOST_MAX_DET=192`** on the S3 envs (static +25 KB; S3 internal free heap ~88 KB, min ~84 KB). The CYD stays at 96
  (+~0.4 KB static, ~4.5 KB heap for forwarding; client free heap ~78 KB, min ~73 KB).
- **DETS v9:** `\tvia` (decimal mask, bit 0 = the connected scanner, bit k = member slot k) is appended to each row.
- **DETS v10 (P6 item 5, fw 0.23.0):** `\tunits` = each unit's OWN strongest RSSI, `<via bit>:<rssi>` comma-joined,
  only on rows a member heard (else empty). `g_detFleetRssi` became per member slot (`[MAX_DET][6]`). Live-POST
  (`remote.js` `unitsOf()`) sends one row per unit: member rows carry `"scanner":<member sid>` + `pos_src:"fleet"`, the
  connected unit's own row stays untagged; one phone-ms sequence per batch. v9 fleet rows post own-only.
- **Web:**
  - Per-row "seen by" chips (one letter per unit, coloured by slot, shown only when a member contributed) and "Seen by"
    in the device modal (`detections.js` / `categories.js`; `viaUnits()` is inlined in each for cache safety).
  - `fleet.js` publishes `state.fleetMembers` (refreshed every 30 s, or 5 s while the card is open) and shows each
    member's scan role.
  - Tokens `20261101b`, `#uiver` `fleet-p5 b`. **Not deployed.**
- **Bench (mini #2 leader + CYD client, all PASS):**
  - The BLE-role leader shows the client's probes (13-17 PRB that its own C5 doesn't capture in that role).
  - Overlap dedups: e.g. 82 rows = 10 own + 14 member-only + 58 both.
  - Client off → its rows age out (re-tested with a 20 s TTL).
  - ~0.75 DetBatch frames per cycle; leader `rx drop 0`.
  - Leader C5 parked in its bootloader → fleet-only view (50/50 member rows), and roles drop to FULL within ~8 s;
    restored → the split returns in 2 s.
- **Review (`77b65bf`) fixed:**
  - REV-001: member data must never reach the leader's own log rows.
  - REV-002: the RX pool.
  - REV-003: forward starvation.
  - REV-004: remote-table size.
  - REV-005: unterminated RID strings.
  - REV-006: the fleet-only cadence.
- **Accepted / open:**
  - A group's `rep` (display + the follow line's `src`) can be a member row.
  - `needsSend` doesn't track bssid / channel / svc16 changes (they arrive with the 10 s refresh).
  - A failed send isn't retried before the next refresh.
  - Multi-member (N ≥ 2 clients) delivery is untested: only one client is on the bench.

### 20.1 Deferred P4 items done (2026-11-01)
- **Band-restricted AP scan** (§19.1 REV-007): the C5 sets `esp_wifi_set_band_mode()` from the plan at each AP-scan
  launch.
  - 2.4 only for a role with no 5 GHz phase (WIFI24), 5 only for a role with no 2.4 phase (WIFI5), auto otherwise
    (legacy / WIFI / BLE).
  - Passive entry switches back to auto if a mid-cycle plan change needs the other band.
  - Bench (CYD forced via `M:o`): AP scan **12.4 s dual-band → 3.0 s 2.4-only → 9.5 s 5-only**.
  - Running AP counts unchanged: 2.4 GHz 10.3 → 10.0, 5 GHz 16.8 → 18.0.
  - A WIFI24 member's cycle drops from ~24 s to ~9.5 s.
- **`M:o:<sid>:<full|wifi|wifi24|wifi5|ble|auto>`:** a per-member role override (runtime only, not saved; it applies
  once the member counts). There is no web control yet; serial `fleet m o:…` or a future layout editor (§16.1).
- **3-unit bench (2026-11-01, release fw 0.22.0: mini #2 leader + CYD + mini #1):**
  - Layout `leader ble, CYD wifi24, mini #1 wifi5`; the ramp works (a new member runs `full` until it has been stable
    10 s).
  - The merge from two clients: e.g. 77 rows = 24 own + 24 member-only + 29 both. Leader `rx drop 0`.
  - Mini #1 takes the fleet clock + position, with 3 self MACs.
  - Fix: a member enrolled with a placeholder name (the phone's key had no unit name → "member") now takes the name it
    reports in its authenticated JoinAck, saved to `fltmem`.

## 21. P6 as built (2026-11-01) — fleet logging (contract agreed with scan-analyzer; owner-approved)

All additions are optional NDJSON keys (old readers ignore them). Item 6 (P5b store-and-forward) was deferred here and built in fw 0.25.0 (§22); until then per-unit
file uploads grouped by `fleet_id` cover it.

- **Item 5 — live-POST member rows (fw 0.23.0, DETS v10):** see §20. Field-tested 2026-11-01 (3 per-scanner server
  sessions, per-unit RSSI, `pos_src:"fleet"` on member rows).
- **Fleet proto v2 (fw 0.24.0):** `Sync` + `fleetId[8]` (random per leader boot, `esp_fill_random` in `leader::begin()`)
  + `fixSlot` (member slot whose GPS IS the fleet position, 0 = none) → 120 B. `FLEET_PROTO_VERSION` 2: a v1 unit's
  frames are dropped at RX, so **every host unit flashes together** (the C5s don't change; link stays v12).
- **Item 1 — `meta`:** + `fleet_role` (`leader|client`, while a role is set) and `fleet_id` / `fleet_leader` (16 / 12 hex,
  while in a session: leader active, client joined). A client that joins after its file was created gets them from the
  `join` evt line.
- **Item 2 — `"evt":"fleet"`:** + `fleet_id` / `fleet_leader` (in a session), `peer` (the other unit's sid: a client's
  leader, a leader's member), `peer_name`, `slot` (member slot 1..6), `n` (leader side: units now = 1 + joined members),
  `plan` on `plan` / `start` (a client's Plan arrives just after its `join`; `+`-joined `ap24`/`ap5`/`prom24`/`prom5`/`154`/`ble`, or `full` = legacy, all the
  Scan Settings allow; every phase is masked by the Scan Settings sources the C5 checks, so it is what really runs). Ops: client `join|plan|lost|unjoin|release`, leader `start|plan|member_add|member_join|
  member_lost|member_release|member_remove`, plus `leave|fallback|client_set|radio_fail|start_failed`.
- **Item 4 — `pos_src`:** on every line that carries `lat`/`lon`: `phone|gps|fleet` = where that fix came from
  (`PosFix.from`; a held fix keeps its origin, `fix_age` marks staleness). `fleet` = a client stamping the leader's fleet
  position, or a leader using a member's GPS. scan-analyzer weights `fleet` fixes 0.25.
- **Item 3 — `"evt":"devpos"`:** `{epoch,ms,[lat,lon,fix_age,pos_src],"evt":"devpos","scanner":<own sid>,"dlat","dlon",
  "sats","used"}` from any unit in a fleet with a LIVE onboard GPS fix, every 30 s or after 25 m of movement
  (`logDevPos()`, each scan cycle). `used` = 1 when that own fix is the fleet position (leader: `PosFix.from == 2`;
  client: `Sync.fixSlot` == its slot). Record-only on the server.
- **STATUS part 2:** `;fid=<16hex>;fld=<12hex>` after `flt=`, only while in a session and only whole (skipped past 249 B,
  i.e. with the download AP's creds, when live-POST is paused anyway). The web app (`ble/status.js`) re-reads them with
  every `flt=` and adds `fleet_role` / `fleet_id` / `fleet_leader` to the live-POST meta (`remote.js` `metaLine()`).

## 22. P5b / P6 item 6 — store-and-forward (BUILT fw 0.25.0, see §22.1; design 2026-11-01; owner-approved, contract agreed with scan-analyzer)

Supersedes §16.7 where they differ. scan-analyzer server `343a5d4` (schema v12) already accepts it.

**NDJSON contract (all optional keys):**
- **`file_id`** on EVERY `meta` line of every stream (fg/bg/fleet, fleet or not): 8 random lowercase hex, fixed for that
  file's life (`LogStream.fileId`, made when the file is created; survives the sync rename).
- **Leader fleet file** `/logs/fleet-NNNNN.jsonl` → renamed `fleet-YYYYMMDD-HHMMSS.jsonl` on time sync, like bg/fg. One per
  leader fleet session (`fleet_id`), created on the first forwarded line. Line 1 = a `meta` with `"stream":"fleet"`, the
  leader's `scanner_id`/`fw`/`db_gen`, `fleet_role:"leader"`, `fleet_id`, `fleet_leader`, its own `file_id`. It holds
  ONLY member lines; the leader's own rows stay in its own bg/fg file (no double SD write). It is a protected live file,
  counts toward the 512 MB cap, gets a timesync anchor on sync like the others, and is listed by `Q`/the exports.
- **Forwarded line** = the member's line VERBATIM with `"scanner":"<origin sid>","ofile":"<origin file_id>","orow":N`
  inserted right after `{` (`orow` = 1-based line number in the origin file; informational — the server dedups by content).
  Every line type is forwarded: detection rows and all `evt` lines (meta, timesync, fleet, lastseen, devpos, rid, …).
- A forwarded `meta`/`timesync` is that ORIGIN file's provenance/backfill anchor on the server.

**Behaviour:**
- **Client forwards its CURRENT active log file from line 1** (owner decision: whole file, even when it joins mid-file),
  reading its own SD file from a saved byte offset — no RAM queue. It tracks `{fileId, byteOff, row}` acked; a new file
  (New Session / reboot) restarts at 0. Files from earlier boots are not forwarded (the member's own file is the backup).
- **New fleet session (leader reboot → new `fleet_id`) or rejoin:** the client first re-sends its file's `meta` line and its
  `timesync` line (if it already has one) — scan-analyzer's request (c) — then continues from its acked offset.
- **Wire:** new sealed messages `Msg::LogRows` (client→leader: fileId, first row, byte offset, flags bit0 = re-sent anchor,
  then ≥1 complete `\n`-terminated lines, ≤ maxPayload) and `Msg::LogAck` (leader→client: fileId, through-row, next
  byte offset). Stop-and-wait: one frame in flight, resent after ~600 ms, rate-limited so DetBatch/Sync keep airtime.
  `FLEET_PROTO_VERSION` 2 → 3: all host units flash together.
- **Leader:** per member `{fileId, nextRow}`; appends a frame's lines it hasn't stored yet (re-sent anchors always), then
  acks. No scoring, no dedup tables. Fleet lines raised during a phone transfer are held like the other streams'
  (XferScope); a LogRows frame arriving during a transfer is not appended/acked until it ends (the client resends).
- **Web:** the Data page lists/groups the `fleet-` stream as its own scan kind ("Fleet"), and a leader's scan download /
  upload offers its own file + its fleet file together (grouped by time span, as the stream groups are today).

### 22.1 As built (fw 0.25.0, 2026-11-01)

- **`file_id`:** `LogStream.fileId[9]` (8 lowercase hex, `esp_fill_random` in `ensureLogFile` when the file is created, cleared
  by `beginSessionNamed`); `writeSessionMeta(f, st)` appends `"file_id"` LAST on every meta line. The joined-export
  `sessionMetaLine()` (no single file) carries none.
- **Wire (fleet proto v3, `fleet_proto.h`):** `Msg::LogRows` 0x23, `Msg::LogAck` 0x24.
  `LogRowsHdr` = `Hdr`(8) + `fileId[4]` + `firstRow u32` + `byteOff u32` + `flags u8` (bit0 anchor) + `nLines u8` = 22 B, then the
  lines verbatim, then the 8-byte tag. `LogAck` = `Hdr` + `fileId[4]` + `throughRow u32` + `nextByteOff u32` = 20 B (+ tag).
  Frame payload cap = `min(maxPayload, 1470) - 8 - 22` (1440 B on a v2 link; on a v1 link only lines <= 220 B fit, longer are skipped).
- **Client (`fleet_client.cpp`):** `LogFwd` (heap, ~100 B) = `{id, ackOff, ackRow}` + the in-flight frame + anchor state.
  `logFwdSource()` (host_log) keeps forwarding the file whose `file_id` it holds while a stream still has it (the path changes
  on the sync rename, the id does not) and only moves to the active stream's file once the old one is fully acked; a file no
  stream holds (New Session rotated it) is abandoned and the new one starts at row 1. After a join the acked part of the file is
  scanned 1 KB per tick for line 1 (meta) and the first `timesync` line; both are re-sent flagged as anchors, then data
  continues at `ackOff`. A frame is rebuilt from the file on every (re)send. Lines are sent only when complete (`
`) and
  starting with `{`; a leading bad/oversized line is skipped and counted. Nothing is read while a phone transfer runs.
- **Leader (`fleet_leader.cpp` + `host_log.cpp`):** `onLogRows` drops a frame un-acked when the tag/seq fails, a line is not a
  `{"` object, or the line count != `nLines`; otherwise `fleetLogStore()` appends (anchors always; data rows only when
  `row >= nextRow`), then `LogAck`. `fleetLogStore` returns false (no ack) during `XferScope` or on an SD error. The fleet file
  is created on the first forwarded line (NVS counter `fltfcnt`; born date-named if the clock is already set), one per leader boot.
- **Exports:** `MultiExport` marks `/logs/fleet-*` entries: no timesync backfill from the file's own anchor and no ms-since-boot
  windowing (member `ms` is a foreign boot clock); only lines already carrying an epoch are exported. Forwarded meta/timesync
  lines are dropped from joined exports like any other file's.
- **A forwarded `devpos` line** carries its own `"scanner"` too (same sid as the inserted one; last key wins in JSON parsers).
- **Open:** a New Session / bg toggle on a client abandons the old file's un-forwarded tail (its own card still has it); a
  member removed and re-enrolled starts a fresh `{file,nextRow}`; leader reboot -> new fleet file, and a client continues from
  its acked offset (rows before it live in the previous leader file).

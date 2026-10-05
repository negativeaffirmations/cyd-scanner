# Signature Matching — Research & Design Notes

Reference for how cyd-scanner should identify surveillance devices (Flock/ALPR
cameras and similar) from observed RF. Survey of prior art + a proposed
layered engine tailored to this project's two-board hardware. Scope stays **detection
only** — it scans (actively, including BLE scan-response solicitation) and fingerprints,
but never attacks or interferes with the devices it observes.

## Implementation status (2026-10-05)

This document began as *pre-implementation* design notes, and most of the "proposed / target /
future" language below has since shipped. Authoritative current state (details in §6):

| Phase / layer | Status |
|---|---|
| **Phase 1** — SD signature DB + weighted confidence scoring (§4.1, §4.3) | **Shipped** — `src/cyd/sigdb.*`, `/signatures.csv` |
| **Phase 2** — promiscuous-mode Wi-Fi capture: client probe requests + 802.11 IE fingerprints (§4.2) | **Shipped** — `src/c5/promisc.*`, the `PH_PROMISC` time-slice hopping 2.4 GHz 1/6/11 |
| **Phase 3** — BLE signature matching: company ID + service UUID (§4.4) | **Shipped** — `Detection.companyId`/`svc[16]` + `svc16[]` (extra 16-bit UUIDs, proto v7); `bleuuid`/`blecid` rules match any advertised UUID |
| **Phase 4** — 802.15.4 presence/fingerprint (§4.5) | **Shipped** — `src/c5/ieee154.*`, receive-only sniffer, **opt-in / default-off** |
| **Phase 5** — spatial/temporal correlation (§4.6) | **Mostly shipped** — "following me" heuristic + whitelist + follow-UI (device + web app, incl. a session track map); rotating-RPA self-filter guard remains |
| **Phase 6** — supervised capture → offline correlation (§7) | Planned |
| Export — PCAP/KML (§4.7) | Not started — on-device logs are NDJSON (`.jsonl`); pcap/SQLite **import** deferred to a host-side tool |

**Additional shipped layers** (beyond the original phase model): **behavioral detectors** on the C5
(iBeacon / Find My / Pwnagotchi / evil-twin / deauth-flood / OpenDroneID presence) that raise threat
tiers + fire live alerts, and a **threat-category** taxonomy (Flock/Axon/ALPR/Cam/Ring/Raven/Glass/
Tracker/Drone/Deauth/Flipper/Skim) with a tappable on-device readout + drill-down, mirrored in the
web app. Active BLE scanning is on (scan-response names/UUIDs); the project is **detection-only** —
it scans and fingerprints but never attacks/interferes with observed devices (see CLAUDE.md scope).

**So: promiscuous-mode capture IS implemented.** Where the body below calls it "the biggest gap"
or "a future architecture addition," read that as the original (now-superseded) framing, kept for
the design rationale. The living build state is in **HANDOFF.md**.

## 1. Prior art — how existing Flock detectors work

A cluster of open-source ESP32 projects converge on a **layered, confidence-tiered
signature approach** (one of them, LuxStatera/flock-hunter-cyd-wifi, runs on this
exact CYD board):

| Technique | Detail |
|---|---|
| **MAC OUI matching** | ~32–34 prefixes from two provenances: community field research + a camera **firmware dump**. Flock's own IEEE block `b4:1e:52`, Qualcomm `00:03:7f` (QCA9377 radio), plus `70:c9:4e`, `3c:91:80`, `d8:f3:bc`, `80:30:49`, `b8:35:32`, `82:6b:f2`, … |
| **SSID patterns** | `Flock-XXXXXX` (SoftAP), bare `Flock`, and wildcard (zero-length) probe-request SSIDs |
| **Probe-request behavior** | Cameras act as Wi-Fi *clients*: wildcard probes, channel-hop 1/6/11 at ~125 ms |
| **802.11 IE fingerprinting** | Highest confidence — Information-Element layout of probe requests, extracted from firmware |
| **BLE** | Penguin battery packs (`Penguin-…`, `FS Ext Battery`), Flock GATT UUID `e8ccbb38-9532-46a8-9fe5-1814df172e6f`, Raven camera services `0x3100–0x3500` |
| **Confidence tiers** | T4 = OUI + wildcard SSID + IE match … T1 = OUI-only. Also `WILD_PROBE` / `OUI_TX` / `OUI_RX` |
| **Capture** | **Promiscuous mode**, ~150–250 ms channel dwell, RSSI floor (−95 dBm), per-MAC cooldown, PCAP + GPS + KML to SD |

Two critical takeaways:

1. **They sniff in promiscuous mode, not `WiFi.scanNetworks()`.** Flock's strongest
   tell is its *client probe-request* behavior; `scanNetworks()` only sees **APs/beacons**
   and would miss it. This was the biggest gap in our original scan-only design — and it has
   since been **closed**: the C5 now runs passive promiscuous capture alongside the AP scan
   (Phase 2 — see the status box above and §6).
2. **OUI-alone is noisy.** Several "Flock OUIs" are generic Espressif/Qualcomm blocks
   shared by countless IoT devices → false positives. The tiering (OUI **+** behavior
   **+** IE/SSID) is what makes detection reliable.

## 2. The broader menu of signature-matching techniques

From the wider Wi-Fi / BLE / Zigbee / IoT fingerprinting literature, techniques fall
into five layers plus a matching-engine axis:

- **A. Identity ("what it claims to be")** — MAC OUI/vendor lookup; SSID/hostname/
  device-name regex; BLE service UUIDs, manufacturer company-ID, GATT Device-Info.
  Cheap, deterministic, first line.
- **B. Structural ("how its packets are shaped")** — 802.11 IE fingerprint (supported
  rates, HT/VHT caps, tag order); BLE advertisement/mfg-data layout. **Survives MAC
  randomization.**
- **C. Behavioral ("how it acts over time")** — probe timing & channel-hop cadence;
  beacon intervals; state-machine models (StateFi); periodic-reporting intervals
  (strong for Zigbee); camera traffic bitrate patterns.
- **D. Physical-layer RF fingerprinting** — hardware imperfections. Powerful but needs
  an **SDR**; **not feasible on ESP32 → out of scope.**
- **E. Spatial/temporal context** (counter-surveillance-specific) — RSSI proximity;
  "**does it follow me?**" (persistence across movement); GPS geotag + multi-sighting
  correlation (the DeFlock mapping model).

**Matching-engine style** is orthogonal: **rule-based signature DB** (Snort/Suricata
analogy — explainable, what these projects use) vs. anomaly/behavioral vs. ML
classifiers (accurate but heavy → realistically off-device, e.g. on the phone/host).

## 3. This project's advantages over the single-ESP32 detectors

- **5 GHz (C5)** — catch 5 GHz APs/backhaul the 2.4-only detectors miss.
- **802.15.4 (C5)** — **no existing Flock detector does this**; Zigbee/Thread
  surveillance sensors + reporting-interval fingerprinting are a novel angle.
- **Two radios** — sniff promiscuously on one while the other does BLE/AP scan, or
  split bands.
- **Touchscreen + SD + phone GPS/time** — already built → geotagged, timestamped,
  multi-technique logging with on-device confidence display.

## 4. Proposed layered engine (target design)

Status tags reflect what has shipped (see the status box up top); the actual on-SD format is
CSV, not the illustrative JSON sketched below.

1. **Signature DB on the SD card** (CSV) — OUI list, name patterns, BLE UUIDs/company-IDs,
   IE fingerprints. **Updatable without reflashing** (edit the file, or push via the phone
   link). **[Phase 1 — shipped]**
2. **Promiscuous-mode Wi-Fi capture** as a scan source (probe requests + beacons + IEs) —
   the key upgrade over scan-only (`scanNetworks`) detection. **[Phase 2 — shipped]**
3. **Weighted confidence scoring** (tiers) — bare Espressif OUI = *low*; OUI +
   wildcard-probe + IE (or a BLE UUID hit) = *high*. Kills false positives. **[Phase 1 — shipped]**
4. **BLE signature matching** — service UUID, company ID, name regex (C5 continuous scan).
   **[Phase 3 — shipped]**
5. **802.15.4 presence/fingerprint** — the differentiator (C5). **[Phase 4 — shipped, opt-in]**
6. **Spatial/temporal correlation** — RSSI proximity + "seen at N GPS points" +
   "following me" heuristic. Turns raw hits into counter-surveillance signal.
   **[Phase 5 — in progress]** (on-device "following me" heuristic + a user whitelist landed;
   the follow-UI redesign, phone-side multi-sighting map correlation, and the rotating-RPA
   self-filter guard remain).
7. **Export** — PCAP/KML to SD (offline analysis, DeFlock contribution). **[not started]**
   (On-device logs are currently NDJSON; pcap/KML/SQLite **import + conversion** is deferred to
   a host-side tool rather than the device — see §7 and HANDOFF.md.)

### Sketch: SD signature DB schema (illustrative, not final)

```jsonc
{
  "version": 3,
  "oui":  [ {"prefix":"b4:1e:52","label":"Flock (IEEE)","weight":40},
            {"prefix":"00:03:7f","label":"Qualcomm QCA9377","weight":15} ],
  "ssid": [ {"regex":"^Flock(-[0-9A-F]{6})?$","label":"Flock SoftAP","weight":50} ],
  "ble":  [ {"uuid":"e8ccbb38-9532-46a8-9fe5-1814df172e6f","label":"Flock GATT","weight":60},
            {"name_regex":"^Penguin-|^FS Ext Battery$","label":"Flock Penguin","weight":50} ],
  "behavior": [ {"kind":"wildcard_probe","weight":25} ],
  "score_thresholds": { "suspect": 40, "likely": 70, "confirmed": 100 }
}
```
Weights from multiple layers sum into a per-device score → suspect/likely/confirmed.

## 5. Constraints & realities

- **Promiscuous mode was a real architecture addition** over the original scan-based design
  — now done (Phase 2): the single 2.4 GHz radio is time-sliced between the AP scan and a
  passive promiscuous window (plus a `PH_154` window when 802.15.4 is enabled).
- **CYD has no PSRAM** (320 KB SRAM) — keep signature tables/parsers lean; the SD DB
  loads into bounded structures.
- **OUI lists drift** — the SD-based updatable DB matters; don't hard-code in firmware.
- **MAC randomization** — not a big issue for Flock (fixed infra MACs) but is for phones;
  IE/behavioral fingerprinting mitigates.
- **ML is overkill on-device** — do rule-based weighted scoring on-device; defer any ML
  to the phone/host.
- **Physical-layer RF fingerprinting needs an SDR** — out of scope.

## 6. Build order + per-phase status

The original plan was to **begin** with (1) SD signature DB + (3) weighted confidence scoring
applied to the detection table already streamed from the C5 — high value, no promiscuous-mode
rework — then add promiscuous capture, BLE matching, 802.15.4, and spatial correlation. That
order was followed; the status block below tracks each phase as built.

> **Status:** (1) + (3) shipped (Phase 1). **(2) promiscuous-mode Wi-Fi capture is now
> implemented** (`src/c5/promisc.*`): the C5 time-slices the radio between the async AP
> scan and a passive promiscuous window that hops 2.4 GHz 1/6/11, capturing client
> **probe requests** (new `PRB` source, wildcard-probe flagged) and **beacon/probe-resp
> 802.11 IE fingerprints**. The fingerprint is a 32-bit FNV-1a hash of the ordered IE
> element-ID list (folding vendor-specific OUIs in) — compact, MAC-randomization-robust,
> carried in `Detection.ie_hash` (link protocol bumped to v2). The CYD scores it via a
> new `ie,<hash>,…` DB rule layer and logs the hash (`ie` column, log schema v3) so field
> capture can catalogue real Flock fingerprints. **Not yet:** 5 GHz promiscuous hopping,
> IE-content (HT/VHT/HE cap) enrichment beyond the OUI fold, and behavioral scoring of the
> wildcard-probe flag — all straightforward follow-ons.
>
> **(4) BLE signature matching is now implemented** (Phase 3): the C5 extracts each BLE
> device's **manufacturer company ID** and **primary service UUID** (normalized to
> canonical 128-bit) into `Detection.companyId` / `svc[16]` (link protocol bumped to v3),
> and the CYD scores them via new `bleuuid,<uuid>,…` (16- or 128-bit) and `blecid,<hex>,…`
> DB rule layers, logging both (`cid`/`uuid` columns, log schema v4). The Flock GATT UUID
> `e8ccbb38-9532-46a8-9fe5-1814df172e6f` is seeded. **Multiple advertised UUIDs are now
> matched** (protocol v7): the active scan collects up to `SVC16_MAX` extra 16-bit service
> UUIDs per device into `Detection.svc16[]` (unioned across the advert + scan-response reports),
> and `sigdb::score()` matches each against the `bleuuid` rules — so a signature UUID is caught
> even when it isn't advertised first. The Raven 0x3100–0x3500 rules are seeded. **Not yet:** a
> *secondary 128-bit* UUID beyond the primary `svc[16]` (all current `bleuuid` signatures are
> 16-bit except the Flock GATT UUID, which is the primary/sole UUID when present).
>
> **(5) 802.15.4 presence is now implemented** (Phase 4): the C5 passively sniffs 802.15.4
> (`src/c5/ieee154.*`) in a `PH_154` time-slice that hops channels {11,15,20,25,26}, fully
> receive-only (promiscuous; no TX/ED/CCA-TX/auto-ACK). Each frame carries the PAN ID +
> short/EUI-64 source address (the OUI packed into `Detection.mac[]` so existing `oui,…,4/A`
> rules match 15.4 for free) + beacon/extended flags; the link protocol is at **v5** (Detection
> carries `panId`). **15.4 is opt-in / default-OFF** — a continuous BLE scan (~99% duty) starves
> 15.4 RX on the shared 2.4 GHz radio, so enabling it time-slices BLE off only during the
> `PH_154` window. **Caveat:** the surveillance *value* is still speculative (no confirmed
> research that Zigbee/Thread is a real surveillance vector), and reception is validated only
> against a bench transmitter (`env:c5test`), not real Zigbee/Thread gear yet.
>
> **(6) spatial/temporal correlation is in progress** (Phase 5): a CYD-side **"following me"
> heuristic** (`FollowState` in `src/cyd/main.cpp`) scores per-device temporal persistence +
> GPS span-from-anchor + distinct GPS fixes + RSSI stability into two tiers — **PERSISTENT**
> (time-only; cannot escalate without GPS movement) and **FOLLOWING** (GPS-movement-corroborated)
> — surfaced on-device (magenta LED + alert line + `>` row glyph). Entirely CYD-side, **no
> link-protocol bump**. A user **whitelist / ignore-list** (`src/cyd/whitelist.*`,
> `/whitelist.csv`, phone-editable) mutes known devices by MAC / name / company-ID / UUID and
> excludes them from the alert. **Not yet:** the follow-UI redesign (alert banner → scrollable
> modal of all flagged devices → per-device detail / add-to-whitelist, both surfaces), the richer
> phone-side multi-sighting **map** correlation, and a **self-filter guard** for the rotating-RPA
> phone false positive (a hidden-name/random-address heuristic guard — the one follow-work branch
> that touches the C5 → link protocol v6). See HANDOFF.md for live status.

## 7. Supervised signature discovery (labeled captures → merge → offline correlation)

The principled way to *derive* new signatures: capture near a KNOWN device, label it,
repeat over hours/days/weeks, and correlate offline. Fully passive — you record
broadcast RF from a device you already know about; nothing is probed.

### Why it works
Fingerprints seen in *most* sessions of known-device X, but *absent* from "control"
sessions (no known device), are X-specific candidates (discriminative / TF-IDF-style
ranking = intersection + control subtraction). MAC randomization *helps* here: fixed
surveillance infrastructure persists across sessions; transient random-MAC devices
(phones, passersby) don't repeat and fall out.

### Capture sessions (device)
- A session has a shared **session id**, agreed at start over BLE (phone generates id +
  label, sends to device; device tags every record).
- During the session the device writes the FULL deduped detection set (all sources) to
  SD, e.g. `/sessions/<id>.jsonl` — each record tagged with session id, GPS, time, RSSI,
  channel/behavior. This is distinct from the normal per-session **first-seen** NDJSON logs
  (`/logs/*.jsonl`).
- Capture **control** sessions (no known device) too, so the offline tool can subtract
  ambient noise.

### Session metadata (phone web app)
- Start/stop, name, mark **"known device present" + type/model**, free-text **notes**,
  attach **photos** (phone camera), and a **map with a marker per GPS'd capture**
  (Leaflet + OpenStreetMap). Photos + rich notes live on the phone (IndexedDB); RF
  captures live on the device SD; the two are joined by session id.

### Merge flow (when a capture is complete)
1. User ends the session in the app.
2. App sends a BLE command → device enters transfer mode and shows a **Wi-Fi-join QR**
   on screen (reuse `webshare` + the existing QR render).
3. Phone scans the QR → joins the device SoftAP (it leaves its normal Wi-Fi; on the
   phone, BLE and Wi-Fi are independent radios so the BLE link stays up. Device-side
   BLE-peripheral + SoftAP coexistence is designed in but still needs real-phone testing).
4. Phone pulls the session capture file over HTTP (`http://192.168.4.1/session/<id>…`)
   — **bulk transfer over Wi-Fi, not BLE**.
5. Phone **merges** the device's RF captures with its own session metadata (notes,
   photos, GPS) by session id → a combined bundle stored on the phone.
6. On transfer complete, phone sends a BLE "end transfer" command → device tears down
   the SoftAP; phone rejoins its normal Wi-Fi; BLE resumes as the primary control link.
   - BLE remains connected throughout, so "reconnect BLE" is really "AP down → BLE-primary
     resumes." Fallback if coexistence is flaky: drop BLE during Wi-Fi, phone signals
     completion via an HTTP `/done` endpoint, then the device re-advertises and the phone
     reconnects BLE.

### Offline correlation (desktop/host script — NOT on the device or phone)
- Export merged session bundles from the phone to a desktop tool.
- The tool runs the discriminative correlation across many sessions and emits ranked
  **candidate signature rules** in the `/signatures.csv` schema (§4) for human review.
- Reviewed rules are pushed back to the device via the Phase-1 DB-update path
  (edit-on-SD / phone push) — closing the loop from field capture to live detection.

### Reuse
`webshare` (Wi-Fi AP + QR + HTTP file serving), the phone BLE link (session control +
metadata + teardown command), the SD stack (session files), GPS/time (phone link), and
the Phase-1 signature DB (promotion target). The only genuinely new off-board piece is
the desktop correlation script.

## Sources

- DeFlock — https://deflock.org/
- colonelpanichacks/flock-you — https://github.com/colonelpanichacks/flock-you
- LuxStatera/flock-hunter-cyd-wifi — https://github.com/LuxStatera/flock-hunter-cyd-wifi
- simeononsecurity/flock-you-esp32 — https://github.com/simeononsecurity/flock-you-esp32
- Hackaday, ESP32 camera detection — https://hackaday.com/2025/09/26/detecting-surveillance-cameras-with-the-esp32/
- Kismet — https://www.kismetwireless.net/
- Passive 802.11 fingerprinting — https://arxiv.org/pdf/1404.6457
- StateFi (Wi-Fi state transitions) — https://arxiv.org/pdf/2507.02478
- BLE IoT fingerprinting (NSF) — https://par.nsf.gov/servlets/purl/10184996
- IoT device fingerprinting review — https://www.sciencedirect.com/science/article/pii/S2542660525002719
- IoTSense (behavioral) — https://arxiv.org/pdf/1804.03852
- Z-IoT (ZigBee/Z-Wave) — https://csl.fiu.edu/wp-content/uploads/2023/05/z_iot.pdf
- ZLeaks (Zigbee inference) — https://arxiv.org/pdf/2107.10830
- CamLoPA (hidden camera localization) — https://arxiv.org/pdf/2409.15169

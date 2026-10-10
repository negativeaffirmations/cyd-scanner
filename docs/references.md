# External reference projects

A catalog of outside open-source projects that may inform cyd-scanner's design. These are
**references for ideas and patterns**, not vendored dependencies.

## ⚠️ Directive to Claude — licensing & attribution (READ BEFORE USING ANY CODE)

Before copying, adapting, or closely porting **any** code, data, or signatures from a project
listed here (or any other outside source), you **must**:

1. **Check the license.** Open the project's `LICENSE`/`COPYING` (or the license field on the
   repo). Confirm it actually permits the intended use. This project's use is
   **non-commercial / personal research** — verify the license allows that. Treat a repo with
   **no license** as *all rights reserved*: do **not** copy its code; you may still read it to
   understand an approach and reimplement cleanly from that understanding.
2. **Check compatibility.** cyd-scanner currently ships **no `LICENSE` file**. Pulling in
   copyleft code (GPL/AGPL/LGPL) or share-alike data (CC-BY-SA) imposes obligations on this
   repo — flag that to the user and do not incorporate such code without an explicit decision.
3. **Attribute properly.** When you do use or adapt something, credit the original in-place:
   - a comment at the code that was adapted — original project name, repo URL, author, and the
     license (e.g. `// Adapted from <Project> (<url>) by <author>, licensed <SPDX>.`), and
   - an entry in this file's **"Code actually used"** section below.
4. **When unsure, ask.** If the license is missing, ambiguous, or incompatible, stop and ask the
   user rather than copying.

Reading a project for *inspiration* and writing original code is always fine; **verbatim or
near-verbatim reuse is what triggers the rules above.**

## Reference catalog

### Chasing-Your-Tail-NG
- **URL:** https://github.com/ArgeliusLabs/Chasing-Your-Tail-NG
- **Platform / language:** Raspberry Pi, mostly Python (differs from cyd-scanner's ESP32/Arduino C++).
- **Why it's here:** tail-identification logic (detecting a device that follows you) and
  false-positive-filtering patterns — candidates to improve the Phase 5 "following me" heuristic
  and the self-filter problem (your own phone getting flagged). See
  `phase5-followme-direction` (memory) and [docs/signature-matching.md](signature-matching.md).
- **License:** **MIT** (permissive) — verified via the repo's `LICENSE`
  (https://github.com/ArgeliusLabs/Chasing-Your-Tail-NG/blob/main/LICENSE). Code may be
  copied/adapted/ported into cyd-scanner under any license (no copyleft; does **not** constrain
  this repo's licensing). Only obligation: preserve the MIT copyright + license notice for any
  reused code, plus the in-repo attribution the directive above requires. (It's Python/RPi, so
  expect to port *logic* into C++ rather than drop in files.)
- **Status:** reference only; no code used yet.

### SquachWatch-CYD
- **URL:** https://github.com/skizzophrenic/SquachWatch-CYD · site https://squachwatch.com
  (web flasher + in-browser emulator)
- **Platform / language:** ESP32 **CYD** (same base board as this project) + many other boards
  incl. a single-board **ESP32-C5**; C/C++ (TFT_eSPI + NimBLE), some Python. Very active (200+
  stars, 30+ releases).
- **Why it's here:** the closest sibling to this project — a standalone CYD surveillance-device
  detector. Deep, directly-relevant prior art for signatures and detection. Ideas worth mining:
  - **Per-signature confidence grading audited against the IEEE OUI registry** (High/Med/Low),
    with a minimum-confidence ALERT gate — exactly this project's `sigdb` false-positive problem,
    done thoroughly (they caught a Sonos OUI mislabeled as a plate reader).
  - **Large detection taxonomy** this project lacks: Meta/Snap camera glasses, Apple/Google/
    Samsung/Tile trackers, BT skimmers, Flipper/Pwnagotchi/Pineapple, Ring, ALPR, generic cameras,
    **Remote-ID drone decode (ASTM F3411 → operator location)**, behavioral **deauth** + **evil-twin** detection.
  - **`regulars` / `ignore_list`** — their recurring-device + self-filter approach (compare to our
    follow heuristic + whitelist + the rotating-RPA self-filter problem).
  - **PC/browser emulator** (same C++ compiled for desktop) — a dev/test idea we don't have.
- **License:** **GPL-3.0 (copyleft).** cyd-scanner is **now GPL-3.0 too** (see `/LICENSE`, adopted
  2026-10-04), so its code and signature data **may be reused/ported with attribution** — no longer
  all-rights-reserved-blocked. Still: curated signature lists are their work, so attribute reuse
  (and prefer re-deriving OUIs from the IEEE registry where practical). Reimplement their *logic*
  cleanly rather than copying large C++ blocks verbatim. Transmit note: its only active TX is the
  **"squad" mesh, which rides BLE advertising** (not ESP-NOW) — manufacturer data, company ID
  0xFFFF, encrypted chat via AES-128-CCM keyed by a shared 5-word phrase — plus Wi-Fi for OTA/NTP.
  Its **LoRa feature is receive-only** (a passive sniffer/decoder for Meshtastic/MeshCore/LoRaWAN/
  APRS/FANET, CrowPanel-7 only). Note which parts transmit — this project's charter allows active
  scanning and the tool's own links but still forbids *attacking/interfering with* observed devices,
  so a mesh beacon addressed at the user's own fleet is fine; anything that floods or disrupts others
  is not.
- **Status:** **signature DATA reused** (2026-10-04) — see "Code actually used" below.

## Code actually used

### SquachWatch-CYD — surveillance-device signature DATA (2026-10-04)

- **What was used:** signature *data only* (MAC OUI prefixes, BLE service UUIDs, BLE manufacturer
  company IDs, and SSID/BLE-name patterns) for surveillance/tracker device classes cyd-scanner
  previously lacked: **Axon** body cameras, **ALPR** (Motorola/Vigilant + Genetec AutoVu),
  consumer/commercial **cameras** (Wyze, Hikvision, Amazon, Arlo, Blink, Tuya, Verkada, Avigilon,
  Axis), **Ring**, BLE **trackers** (Tile, Samsung SmartTag, Google Find My), **Ray-Ban Meta** +
  **Snap Spectacles** camera glasses, **Flipper Zero / Wi-Fi Pineapple / deauther**, **OpenDroneID**
  (Remote-ID) + **Raven** gunshot-detector BLE UUIDs, plus a few Flock BLE/module signals.
  No C++ logic was copied — only the fact tables were extracted and re-expressed as `sigdb` rules.
- **Where it landed:** `src/cyd/sigdb.cpp` (the compiled seed / `kSeedCsv`), self-seeded to the SD
  card as `/signatures.csv`. An attribution comment sits directly above the added rules.
- **Source:** SquachWatch-CYD by *skizzophrenic*, `src/signatures.cpp` @ commit `f49ecbe`
  (https://github.com/skizzophrenic/SquachWatch-CYD).
- **License:** GPL-3.0 — compatible, as cyd-scanner is now GPL-3.0 (`/LICENSE`).
- **Upstream credits carried forward:** Flock OUIs via `colonelpanichacks/flock-you` (MIT) + the
  DeFlock community field list; Ring/Verkada/Avigilon/Axis/Motorola OUIs read from the public
  **IEEE MA-L registry**; Flipper constants verified against IEEE + Bluetooth SIG registries.
- **Deliberately NOT ported:** BT-Classic skimmer signatures (scanner build is BLE-only, so
  unobservable); and the behavioral/payload matchers (iBeacon, AirTag raw-advert payload,
  Pwnagotchi beacon-IE, evil-twin, deauth-burst, full ASTM F3411 Remote-ID decode) — these aren't
  identifier lookups and would need dedicated detector code (candidate future C5-side work).

### Camera OUIs read directly from the IEEE MA-L registry (2026-10-10)

- **What was used:** MAC OUI prefixes for camera/doorbell brands the roster lacked, read straight
  from the **public IEEE MA-L registry** (standards-oui.ieee.org) — not copied from any project:
  **Nest Labs** `64:16:66` / `18:B4:30`, **Reolink** `EC:71:DB`, **SimpliSafe** `F8:51:28`. Also
  re-weighted the existing Arlo/Blink OUIs (15→40) so they stand alone as suspect.
- **Where it landed:** `src/cyd/sigdb.cpp` seed (`kSeedCsv`), `W` srcmask, label → `CAT_CAM`.
- **Why these are detectable now:** paired with the C5 associated-client capture (uplink data-frame
  addr2, 2026-10-10), an *installed* camera that neither beacons nor probes now surfaces as a Wi-Fi
  client and these OUI rules fire. (Eufy/Anker were researched but left out — no clean, verified
  brand OUI; their registrations are fragmented.)

*When further code/data from a cataloged project is adapted, record it here too: project, what was
used, where it landed (file/function), the license, and the attribution added.*

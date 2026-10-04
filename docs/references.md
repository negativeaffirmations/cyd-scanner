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
- **License:** ⚠️ **not yet verified** — check the repo before using any code (see directive above).
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
- **License:** **GPL-3.0 (copyleft).** ⚠️ This project ships no `LICENSE`; **do NOT copy/port its
  code** — doing so would force cyd-scanner to become GPL-3.0. Read it for *ideas* and reimplement
  cleanly; curated signature lists are their work, so attribute if any are reused (and prefer
  re-deriving OUIs from the IEEE registry directly). Transmit note: its only active TX is the
  **"squad" mesh, which rides BLE advertising** (not ESP-NOW) — manufacturer data, company ID
  0xFFFF, encrypted chat via AES-128-CCM keyed by a shared 5-word phrase — plus Wi-Fi for OTA/NTP.
  Its **LoRa feature is receive-only** (a passive sniffer/decoder for Meshtastic/MeshCore/LoRaWAN/
  APRS/FANET, CrowPanel-7 only). So only the BLE squad beacon is outside this project's strict
  passive/receive-only charter; the LoRa sniffing is compatible with it.
- **Status:** reference only; no code used yet.

## Code actually used

*(none yet)* — when code from a cataloged project is adapted into cyd-scanner, record it here:
project, what was used, where it landed (file/function), the license, and the attribution added.

# fleet-sniff: on-air check of the fleet link (bench tool)

Shows what the fleet link really transmits, so its privacy claims are measured, not assumed: only Probe, Hello and
Join are cleartext, no unit ID, name or fixed MAC appears in any of them, and the fresh-per-session fields really are
fresh. Results: [docs/fleet-mode.md](../../docs/fleet-mode.md) §17.2.

It is its own PlatformIO project, kept out of the scanner build.

## Receive-only

The `fleetsniff` firmware never transmits. It starts the Wi-Fi STA only so promiscuous mode works, and never scans or connects.

It keeps action frames (ESP-NOW is a vendor action frame) and frames from MACs you list at runtime. Everything else,
including other people's beacons and probes, is dropped in the RX callback. `analyze.py` then ignores any action frame
that doesn't involve a fleet MAC.

**Never commit a capture or an identities file.** They name real units. Keep both outside the repo.

## Use

Any ESP32-C5 works. A unit's own C5 can stand in, because the fleet runs on the host radio: flash this, capture, then
reflash `c5` / `xiao_c5`. That unit's scanning stops until you do.

```bash
pio run -d tools/fleet-sniff -t upload --upload-port COMxx
```

Serial commands, newline terminated. Open the port with DTR on (the C5 console needs it):

| Command | Does |
|---|---|
| `c <1-13>` | set the channel (default 1, the fleet default) |
| `w <12 hex>` | add a MAC to the watch list (up to 24) |
| `s` | print frame counters |

Each frame is one line: `<A|M> <ms> <rssi> <ch> <fc> <addr1> <addr2> <len> <first 128 bytes, hex>`.
Save that output to a file. While capturing, reset a client at least twice: Hello and Join are only sent on a join,
and comparing joins needs more than one.

```bash
python tools/fleet-sniff/analyze.py capture.txt identities.json
```

The format of `identities.json` is in the header of `analyze.py`. List each unit's name, base MAC and radios.

## Checking a BLE scanner's address

A `SCAN_REQ` can't be seen from Wi-Fi. Env `scanreq` makes the C5 a scannable beacon that prints every scanner asking
it for a scan response, as `S <ms> <address> <0 public | 1 random>`:

```bash
pio run -d tools/fleet-sniff -e scanreq -t upload --upload-port COMxx
```

It transmits that advert and the scan responses, and nothing else. Nearby phones can show up too; the unit under test
is the busiest address, and it should change when that C5 reboots.

## What a PASS does and doesn't cover

- Cleartext frames are checked whole. Encrypted frames are only searched in their first 128 bytes.
- It fails if a capture holds no join, because that would prove little.
- A repeated `Hello` or `Join` value is normally an 802.11 retransmission (retry bit set, identical bytes).
- It doesn't replay frames. Replay protection is a separate test that needs a transmitter.

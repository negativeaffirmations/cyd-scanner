#!/usr/bin/env python3
"""Check a fleet-sniff capture against what the fleet link promises (docs/fleet-mode.md section 17).

usage: analyze.py <capture.txt> <identities.json>

identities.json stays LOCAL (never commit it, or a capture):
  {"names": ["unit name", ...],
   "ids":   {"label": "12 hex sid or base MAC", ...},     searched in both byte orders, raw and as text
   "host_radios": {"label": "12 hex", ...},               must never send a fleet frame
   "scan_radios": {"label": "12 hex", ...}}               C5s: their own scanning is reported, not failed

Exit code 0 = every check passed.
"""
import collections
import json
import sys

ESPNOW_OUI = bytes.fromhex("18fe34")
CLEAR_OK = {1: "Probe", 2: "Hello", 3: "Join"}
NAMES = {4: "JoinAck", 0x10: "Sync", 0x11: "Plan", 0x12: "Ctrl", 0x13: "Irks", 0x20: "MStat", 0x21: "DetBatch", 0x22: "RidBatch",
         0x23: "LogRows", 0x24: "LogAck", 0xF0: "DbgPing", 0xF1: "DbgPong", **CLEAR_OK}
BODY = 24 + 8 + 7   # MAC header, then category + OUI + random, then the vendor IE header


def needles(ident):
    out = {}
    for n in ident.get("names", []):
        out[n.encode()] = "name " + n
    for group in ("ids", "host_radios", "scan_radios"):
        for label, h in ident.get(group, {}).items():
            raw = bytes.fromhex(h)
            for form in (raw, raw[::-1]):
                out[form] = label
                out[form.hex().encode()] = label + " (text)"
                out[form.hex().upper().encode()] = label + " (text)"
    return out


def is_clear_fleet(fc, d):
    return not fc & 0x4000 and len(d) > BODY + 2 and d[24] == 127 and d[25:28] == ESPNOW_OUI


def load(path):
    frames = []
    for line in open(path, errors="replace"):
        p = line.split()
        if len(p) != 9 or p[0] not in ("A", "M"):
            continue
        try:
            frames.append((p[0], int(p[4], 16), p[5], p[6], int(p[7]), bytes.fromhex(p[8])))
        except ValueError:
            continue
    return frames


def main():
    frames = load(sys.argv[1])
    ident = json.load(open(sys.argv[2]))
    search = needles(ident)
    hosts = {v: k for k, v in ident.get("host_radios", {}).items()}
    scans = {v: k for k, v in ident.get("scan_radios", {}).items()}
    fails = []
    clear = collections.Counter()
    sizes = collections.defaultdict(set)
    prot = collections.Counter()
    watched = collections.Counter()
    seen = collections.defaultdict(collections.Counter)   # field name -> value -> count
    other_action = 0
    total = 0

    # Fleet MACs are the ones that send cleartext fleet frames, plus whoever those frames are addressed to.
    fleet = set()
    for kind, fc, a1, a2, ln, d in frames:
        if kind == "A" and is_clear_fleet(fc, d):
            fleet.add(a2)
            if a1 != "ffffffffffff":
                fleet.add(a1)

    for kind, fc, a1, a2, ln, d in frames:
        if kind == "M":
            watched[a2] += 1
            continue
        if a2 not in fleet and a1 not in fleet:
            other_action += 1
            continue
        total += 1
        for mac in (a1, a2):
            if mac in hosts:
                fails.append(f"fleet frame uses fixed radio {hosts[mac]}")
        for needle, label in search.items():
            if needle in d[24:]:
                where = "protected" if fc & 0x4000 else "CLEARTEXT"
                fails.append(f"{label} found in a {where} frame body from {a2}")
        if fc & 0x4000:
            prot[(a2, a1)] += 1
            sizes["protected"].add(ln)
            continue
        if not is_clear_fleet(fc, d):
            fails.append(f"unprotected non-ESP-NOW action frame from {a2}")
            continue
        typ = d[BODY + 1]
        name = NAMES.get(typ, hex(typ))
        clear[name] += 1
        sizes[name].add(ln)
        if typ not in CLEAR_OK:
            fails.append(f"{name} sent in the CLEAR from {a2}")
            continue
        # The 16-byte fields must be fresh per session: a stable value would be a tracking handle.
        b = d[BODY:]
        if typ == 1:
            seen["Probe.nl"][b[8:24]] += 1
        elif typ == 2:
            seen["Hello.nl"][b[8:24]] += 1
            seen["Hello.nc"][b[24:40]] += 1
            seen["Hello.tag"][b[40:56]] += 1
        elif typ == 3:
            seen["Join.nc"][b[24:40]] += 1
            seen["Join.proof"][b[44:60]] += 1

    print(f"fleet frames {total}, cleartext {sum(clear.values())}, protected {sum(prot.values())}")
    print(f"fleet MACs seen: {len(fleet)}  (lengths include the 4-byte FCS)")
    for n, c in clear.most_common():
        print(f"  clear {n:8} x{c}  lengths {sorted(sizes[n])}")
    for (src, dst), c in prot.most_common():
        print(f"  protected {src} -> {dst} x{c}")
    ps = sorted(sizes["protected"])
    print(f"  protected lengths: {ps[:12]}{' ...' if len(ps) > 12 else ''}")
    print(f"other action frames ignored: {other_action}")
    for field, values in sorted(seen.items()):
        print(f"  {field}: {sum(values.values())} frames, {len(values)} distinct, most repeated x{max(values.values())}")
    for mac, c in watched.most_common():
        print(f"  other mgmt frames from watched {hosts.get(mac) or scans.get(mac) or mac}: x{c}")
        if mac in hosts:
            fails.append(f"fixed radio {hosts[mac]} sent {c} management frames")
    if not clear.get("Hello") or not clear.get("Join"):
        fails.append("no Hello/Join captured: the capture never saw a join, so it proves little")
    if not prot:
        fails.append("no protected frames captured")
    print("RESULT:", "PASS" if not fails else "FAIL")
    for f in sorted(set(fails)):
        print("  -", f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

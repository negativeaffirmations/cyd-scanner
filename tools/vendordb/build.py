#!/usr/bin/env python3
"""Build the web app's maker lookup files (phone analysis, docs/phone-analysis.md increment 3d).

  webapp/data/oui.tsv  "AABBCC<TAB>Maker"   IEEE MA-L (24-bit) assignments
  webapp/data/cid.tsv  "004C<TAB>Company"   Bluetooth SIG company identifiers

Sources (public registries):
  - Wireshark "manuf" (https://www.wireshark.org/download/automated/data/manuf), generated from the IEEE
    registration authority's public OUI listings. Wireshark is GPL-2.0-or-later; this project is GPL-3.0.
  - Bluetooth SIG assigned numbers, company_identifiers.yaml
    (https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml).

usage: python tools/vendordb/build.py [manuf-file company_identifiers.yaml]   (downloads both when no files are given)
"""
import os, re, sys, urllib.request

MANUF_URL = "https://www.wireshark.org/download/automated/data/manuf"
CID_URL = "https://bitbucket.org/bluetooth-SIG/public/raw/main/assigned_numbers/company_identifiers/company_identifiers.yaml"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "webapp", "data")
MAXLEN = 48


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "cyd-scanner vendordb build"})
    return urllib.request.urlopen(req, timeout=120).read().decode("utf-8", "replace")


def clean(s):
    s = re.sub(r"\s+", " ", s.replace("\t", " ")).strip()
    return s[:MAXLEN].rstrip()


def build_oui(text):
    out = {}
    for ln in text.splitlines():
        if not ln or ln.startswith("#"):
            continue
        f = ln.split("\t")
        if len(f) < 2:
            continue
        pfx = f[0].strip()
        if "/" in pfx or not re.fullmatch(r"[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}", pfx):
            continue   # MA-M / MA-S sub-ranges and odd entries: MA-L only
        name = clean(f[2] if len(f) > 2 and f[2].strip() else f[1])
        if name:
            out[pfx.replace(":", "").upper()] = name
    return out


def build_cid(text):
    out = {}
    for m in re.finditer(r"value:\s*0x([0-9A-Fa-f]{1,4})\s*\n\s*name:\s*['\"](.*?)['\"]\s*$", text, re.M):
        out[m.group(1).upper().zfill(4)] = clean(m.group(2).replace("''", "'"))
    return out


def write(path, d):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for k in sorted(d):
            f.write(k + "\t" + d[k] + "\n")
    print(path, len(d), "entries", os.path.getsize(path), "bytes")


def main():
    if len(sys.argv) == 3:
        manuf = open(sys.argv[1], encoding="utf-8", errors="replace").read()
        cid = open(sys.argv[2], encoding="utf-8", errors="replace").read()
    else:
        manuf, cid = fetch(MANUF_URL), fetch(CID_URL)
    os.makedirs(OUT, exist_ok=True)
    o, c = build_oui(manuf), build_cid(cid)
    if len(o) < 10000 or len(c) < 1000:
        sys.exit("suspiciously few entries (%d OUI, %d company IDs): not written" % (len(o), len(c)))
    write(os.path.join(OUT, "oui.tsv"), o)
    write(os.path.join(OUT, "cid.tsv"), c)


if __name__ == "__main__":
    main()

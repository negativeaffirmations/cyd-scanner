# Label printer (Supvan / Katasymbol T50M Pro, USB)

Prints a scanner's device label: its pairing QR on top, the unit name and scanner id below, in portrait.

```bash
cd tools/label-printer
py -3.14 label.py status                                                  # printer status + loaded tape
py -3.14 label.py preview ../../qr-codes/CFLO-MINI-SOPAFE-pairing-qr.svg    # PNG in out/, no printer
py -3.14 label.py print   ../../qr-codes/CFLO-MINI-SOPAFE-pairing-qr.svg    # preview, then print
```

- Needs Python with Pillow (`py -3.14` here) and the printer on USB (VID `1820`, PID `207F`). Close the Katasymbol
  Windows app / `Supvan_T50_Service` first if one is running.
- Tape size comes from the printer's tape chip, else `--tape 40x30` (mm across the head x mm along the feed).
  `--density 0-15` (default 3), `--copies N`, `--name` / `--sid` override the SVG caption. The image is flipped across
  the head before printing (the T50M Pro printed it mirrored otherwise); `--mirror` skips that flip.
- **The QR holds the scanner's secret phone key.** It is copied from the SVG's module grid (never decoded to text),
  `qr-codes/` and `out/` are git-ignored, and a label on a device lets anyone holding it pair with it. Fine for
  private units; for public ones print name + id only.
- The CYDs have no QR file (their QR is on screen: Settings -> Phone key).
- `t50/` is vendored unchanged from [zpan45/t50-windows-driver](https://github.com/zpan45/t50-windows-driver)
  (MIT, `t50/LICENSE`), see docs/references.md.

"""Print a scanner's device label on a Supvan / Katasymbol T50M Pro (USB) — cyd-scanner tool.

Layout (portrait): the scanner's pairing QR on top, its name and scanner id stacked below.
The QR comes from a pairing-QR SVG in qr-codes/ (git-ignored: it holds the device's secret key);
the module grid is read straight from the SVG, so the key is never decoded to text here.
The name + scanner id come from the SVG's caption unless --name / --sid are given.

    py -3.14 label.py preview ../../qr-codes/CFLO-MINI-SOPAFE-pairing-qr.svg   # PNG only, no printer
    py -3.14 label.py status                                                 # printer status + tape
    py -3.14 label.py print   ../../qr-codes/CFLO-MINI-SOPAFE-pairing-qr.svg   # preview + print

The USB printer code in t50/ is vendored unchanged from zpan45/t50-windows-driver (MIT, see
t50/LICENSE), whose protocol work credits heeen/supvan-cups (MIT). This file is cyd-scanner's own.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from t50 import DOTS_PER_MM, PRINTHEAD_WIDTH_DOTS  # noqa: E402

OUT_DIR = os.path.join(HERE, "out")       # git-ignored: previews show the secret-key QR
DEFAULT_TAPE = (40, 30)                    # mm across the head x mm along the feed (the owner's roll)
SAFE_MM = 1.5                              # blank border kept clear on every side
# Fonts: only ones that ship with Windows (same path on every PC). A font installed by hand would go in a project
# folder next to this file instead, never a path under a user profile.
FONT_NAME = ["C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeuib.ttf"]
FONT_ID = ["C:/Windows/Fonts/consolab.ttf", "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf"]


# ---------- the QR, read from the pairing SVG ----------
def read_qr_svg(path: str) -> tuple[list[list[bool]], str]:
    """Return (module grid, caption). The SVG draws each dark module as `M<x> <y>h<m>v<m>h-<m>z`."""
    with open(path, encoding="utf-8") as f:
        svg = f.read()
    cells = [(int(x), int(y)) for x, y in re.findall(r"M(\d+) (\d+)h\d+v\d+h-\d+z", svg)]
    m = re.search(r"M\d+ \d+h(\d+)v", svg)
    if not cells or not m:
        raise SystemExit(f"{path}: no QR modules found (not a pairing-QR SVG from qr-codes/?)")
    step = int(m.group(1))
    x0 = min(x for x, _ in cells)
    y0 = min(y for _, y in cells)
    n = (max(x for x, _ in cells) - x0) // step + 1
    if n != (max(y for _, y in cells) - y0) // step + 1 or not 21 <= n <= 177 or (n - 17) % 4:
        raise SystemExit(f"{path}: QR grid is not square / not a valid QR size ({n} modules)")
    grid = [[False] * n for _ in range(n)]
    for x, y in cells:
        grid[(y - y0) // step][(x - x0) // step] = True
    cap = re.search(r"<text[^>]*>([^<]*)</text>", svg)
    return grid, (cap.group(1).strip() if cap else "")


def split_caption(cap: str) -> tuple[str, str]:
    """'CFLO-MINI-SOPAFE · 2008e91db410' -> (name, sid)."""
    parts = [p.strip() for p in re.split(r"\s+[·|]\s+", cap) if p.strip()]
    name = parts[0] if parts else ""
    sid = parts[1] if len(parts) > 1 else ""
    return name, sid


# ---------- layout ----------
def load_font(cands: list[str], size: int) -> ImageFont.FreeTypeFont:
    for p in cands:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default(size)


def fit_font(draw: ImageDraw.ImageDraw, text: str, cands: list[str], max_w: int, max_h: int) -> ImageFont.FreeTypeFont:
    for size in range(max_h, 7, -1):
        f = load_font(cands, size)
        l, t, r, b = draw.textbbox((0, 0), text, font=f)
        if r - l <= max_w and b - t <= max_h:
            return f
    return load_font(cands, 8)


def render_portrait(grid: list[list[bool]], name: str, sid: str, w: int, h: int) -> Image.Image:
    """Portrait label w x h dots: QR on top (largest whole-dot module that fits), name + sid below."""
    img = Image.new("1", (w, h), 1)
    d = ImageDraw.Draw(img)
    safe = round(SAFE_MM * DOTS_PER_MM)
    inner_w, inner_h = w - 2 * safe, h - 2 * safe
    n = len(grid)
    quiet = 2                                            # modules of white around the code (plus the label border)
    text_h = max(int(inner_h * 0.30), 2 * 18)            # room kept for the two text lines
    mod = max(1, min(inner_w // (n + 2 * quiet), (inner_h - text_h) // (n + 2 * quiet)))
    size = n * mod
    qx = (w - size) // 2
    qy = safe + quiet * mod
    for r, row in enumerate(grid):
        for c, dark in enumerate(row):
            if dark:
                d.rectangle([qx + c * mod, qy + r * mod, qx + (c + 1) * mod - 1, qy + (r + 1) * mod - 1], fill=0)
    top = qy + size + quiet * mod
    avail = h - safe - top
    lines = [(name, FONT_NAME, 0.56), (sid, FONT_ID, 0.44)]
    lines = [ln for ln in lines if ln[0]]
    if lines:
        gap = 2
        boxes = []
        cap_h = None                                     # the id line is never taller than the name above it
        for text, cands, share in lines:
            max_h = int((avail - gap * (len(lines) - 1)) * share / sum(s for _, _, s in lines))
            if cap_h:
                max_h = min(max_h, cap_h)
            f = fit_font(d, text, cands, inner_w, max_h)
            if cap_h is None:
                l0, t0, r0, b0 = d.textbbox((0, 0), text, font=f)
                cap_h = max(8, b0 - t0)
            l, t, r, b = d.textbbox((0, 0), text, font=f)
            boxes.append((text, f, l, t, r - l, b - t))
        total = sum(bh for *_, bh in boxes) + gap * (len(boxes) - 1)
        y = top + (avail - total) // 2
        for text, f, l, t, bw, bh in boxes:
            d.text(((w - bw) // 2 - l, y - t), text, font=f, fill=0)
            y += bh + gap
    return img


def page_for_tape(portrait: Image.Image, across_dots: int, feed_dots: int) -> Image.Image:
    """Orient the portrait label for the head: image width = across the head, height = along the feed."""
    page = portrait if (portrait.width, portrait.height) == (across_dots, feed_dots) \
        else portrait.rotate(90, expand=True)            # portrait's tall side lies across the head
    # The T50M Pro lays the head's dots out opposite to the driver's packing: printed as-is the label came out
    # mirrored (owner, 2026-11-22). Flip across the head so it reads right; --mirror prints it the old way.
    return page if MIRROR else page.transpose(Image.FLIP_LEFT_RIGHT)


MIRROR = False                                           # set by --mirror


def build(svg: str, name: str | None, sid: str | None, tape: tuple[int, int]) -> tuple[Image.Image, Image.Image, str, str]:
    grid, cap = read_qr_svg(svg)
    cname, csid = split_caption(cap)
    name = name if name is not None else cname
    sid = sid if sid is not None else csid
    across, feed = tape[0] * DOTS_PER_MM, tape[1] * DOTS_PER_MM
    if across > PRINTHEAD_WIDTH_DOTS:
        raise SystemExit(f"tape {tape[0]} mm is wider than the {PRINTHEAD_WIDTH_DOTS // DOTS_PER_MM} mm head")
    pw, ph = min(across, feed), max(across, feed)        # portrait = the short side is the width
    portrait = render_portrait(grid, name, sid, pw, ph)
    return portrait, page_for_tape(portrait, across, feed), name, sid


def save_preview(portrait: Image.Image, name: str) -> str:
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, f"{name or 'label'}-preview.png")
    portrait.convert("L").resize((portrait.width * 3, portrait.height * 3), Image.NEAREST).save(path)
    return path


def to_row_major_msb(img: Image.Image) -> bytes:
    """1 = black, MSB first (what t50.bitmap.pack_page takes)."""
    w, h = img.size
    bpl = (w + 7) // 8
    out = bytearray(bpl * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] == 0:
                out[y * bpl + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


# ---------- CLI ----------
def parse_tape(s: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d+)x(\d+)", s)
    if not m:
        raise argparse.ArgumentTypeError("tape is WxH in mm, e.g. 40x30 (W = across the head)")
    return int(m.group(1)), int(m.group(2))


def main() -> None:
    ap = argparse.ArgumentParser(description="cyd-scanner device labels on a T50M Pro (USB)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status", help="printer status + loaded tape")
    for c in ("preview", "print"):
        p = sub.add_parser(c, help="render a label PNG" if c == "preview" else "render, then print")
        p.add_argument("svg", help="pairing-QR SVG from qr-codes/")
        p.add_argument("--name", help="override the caption's unit name")
        p.add_argument("--sid", help="override the caption's scanner id")
        p.add_argument("--tape", type=parse_tape, help="WxH mm, W across the head (default: what the printer reports, else 40x30)")
        if c == "print":
            p.add_argument("--density", type=int, default=3, help="0-15 (default 3)")
            p.add_argument("--copies", type=int, default=1)
            p.add_argument("--mirror", action="store_true", help="skip the across-head flip (if a label prints mirrored)")
    a = ap.parse_args()

    if a.cmd == "status":
        from t50.printer import Printer
        with Printer.open() as p:
            print("status:", p.query_status())
            print("tape:  ", p.query_material())
        return

    global MIRROR
    MIRROR = bool(getattr(a, "mirror", False))
    tape = a.tape
    printer = None
    if a.cmd == "print":
        from t50.printer import Printer
        printer = Printer.open()
        if tape is None:
            mat = printer.query_material()
            if mat.width_mm and mat.height_mm:
                tape = (mat.width_mm, mat.height_mm)
                print(f"tape from printer: {mat}")
    tape = tape or DEFAULT_TAPE
    portrait, page, name, sid = build(a.svg, a.name, a.sid, tape)
    print(f"label: {name or '-'} / {sid or '-'}  tape {tape[0]}x{tape[1]} mm  page {page.width}x{page.height} dots")
    print("preview:", save_preview(portrait, name))
    if printer is None:
        return
    try:
        data = to_row_major_msb(page)
        for i in range(max(1, a.copies)):
            printer.print_page(data, page.width, page.height, density=max(0, min(15, a.density)))
            print(f"printed {i + 1}/{a.copies}")
    finally:
        printer.close()


if __name__ == "__main__":
    main()

# Builds the Android launcher icons from the COINTEL-FLO wordmark (resources/logo/logo.png):
# the wordmark's "C" and "F" glyphs, pixel-scaled, magenta on black.
# Run with any Python that has Pillow:  python android-app/scripts/make-icons.py
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
RES = ROOT / "android-app/android/app/src/main/res"
logo = Image.open(ROOT / "resources/logo/logo.png").convert("RGBA")
W, H = logo.size

def lit(x, y):
    return sum(logo.getpixel((x, y))[:3]) > 60

def glyph(x0, x1):
    """Crop one letter column range to its lit rows."""
    rows = [y for y in range(H) if any(lit(x, y) for x in range(x0, x1))]
    return logo.crop((x0, rows[0], x1, rows[-1] + 1))

# Letter column ranges in the wordmark: C = first letter, F = first letter of "FLO".
c, f = glyph(11, 90), glyph(593, 673)
gap = 8
pair = Image.new("RGBA", (c.width + gap + f.width, max(c.height, f.height)), (0, 0, 0, 0))
pair.paste(c, (0, 0)); pair.paste(f, (c.width + gap, 0))

def placed(size, frac):
    """The glyph pair centred on a black square, its width `frac` of the square."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 255))
    scale = size * frac / pair.width
    g = pair.resize((max(1, round(pair.width * scale)), max(1, round(pair.height * scale))), Image.LANCZOS)
    img.alpha_composite(g, ((size - g.width) // 2, (size - g.height) // 2))
    return img

DENS = {"mdpi": 1, "hdpi": 1.5, "xhdpi": 2, "xxhdpi": 3, "xxxhdpi": 4}
for d, k in DENS.items():
    out = RES / f"mipmap-{d}"
    # Adaptive foreground (108 dp): keep the art inside the 66 dp safe zone.
    placed(round(108 * k), 0.58).save(out / "ic_launcher_foreground.png")
    legacy = placed(round(48 * k), 0.80)
    legacy.convert("RGB").save(out / "ic_launcher.png")
    mask = Image.new("L", legacy.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, legacy.width - 1, legacy.height - 1), fill=255)
    rnd = Image.new("RGBA", legacy.size, (0, 0, 0, 0)); rnd.paste(legacy, (0, 0), mask)
    rnd.save(out / "ic_launcher_round.png")
(RES / "values/ic_launcher_background.xml").write_text(
    '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="ic_launcher_background">#000000</color>\n</resources>\n')
print("icons written to", RES)

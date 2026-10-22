#!/usr/bin/env python3
"""Draw the Spectra 6 test cards, for show_image.py.

    python3 esp32/testcard.py testcard.png                 # harmonies (v4)
    python3 esp32/testcard.py --card mixing testcard.png   # ink mixing (v3)
    python3 esp32/testcard.py --card calibrate out.png     # inks, text grounds, patterns
    python3 esp32/testcard.py --card type testcard.png     # typography
    python3 esp32/testcard.py --card type2 --trmnl DIR out.png  # pixel fonts vs today
    python3 esp32/testcard.py --card type3 out.png         # Arial, Verdana, Archivo weights
    python3 esp32/testcard.py --card type4 --free DIR out.png   # their free equivalents
    python3 esp32/testcard.py --card fonts-microsoft out.png    # Regular/Bold, 8-48 px
    python3 esp32/testcard.py --card fonts-apple out.png
    python3 esp32/testcard.py --card fonts-open --open DIR out.png
    python3 esp32/show_image.py testcard.png

"harmonies" puts the wignet-visuals skill's guidance on the panel: five
palettes as small screens, the smooth mixes beside grainy ones, and text
contrast pairings to use and to avoid. "calibrate" checks the measured
palette (PHOTO_INKS) on our own panel: the inks solid, black and white text
on every ink and every ink as text on white, and six 50/50 mixes laid out
five ways (checkerboard, rows, columns, diffused, photo), since the
arrangement alone changes the colour. "mixing" is the raw material:

A pixel on the panel holds one ink; "mixing" two inks means alternating
them in a fine pattern that the eye blends from a distance. Every pixel here
is drawn in a converter palette value (wgn6.RGB_PALETTE) at the panel's size,
so the converter changes none of them: the panel shows exactly these
patterns.

1. The six inks, solid.
2. Every pair of inks: above the diagonal 50/50 (a pixel checkerboard),
   below it 3/4 of the row's ink and 1/4 of the column's.
3. Tint ramps in five steps (0, 25, 50, 75, 100 %), laid out with a 4x4
   ordered pattern: oranges, pinks, light blues, blue-yellow, greys.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
from backoffice import wgn6  # noqa: E402

INKS = [("Black", wgn6.BLACK), ("White", wgn6.WHITE), ("Red", wgn6.RED),
        ("Yellow", wgn6.YELLOW), ("Green", wgn6.GREEN), ("Blue", wgn6.BLUE)]
RAMPS = [("Red > Yellow", wgn6.RED, wgn6.YELLOW), ("Red > White", wgn6.RED, wgn6.WHITE),
         ("Blue > White", wgn6.BLUE, wgn6.WHITE), ("Blue > Yellow", wgn6.BLUE, wgn6.YELLOW),
         ("Black > White", wgn6.BLACK, wgn6.WHITE)]
STEPS = (0, 4, 8, 12, 16)  # sixteenths of the second ink: 0, 25, 50, 75, 100 %
# 4x4 Bayer matrix: the first n cells in this order hold the second ink, so
# n = 8 is a checkerboard and n = 4 spreads a quarter evenly.
BAYER = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
INK_RGB = wgn6.RGB_PALETTE
TEXT = INK_RGB[wgn6.BLACK]


def font(size, bold=False):
    names = ["Arial Bold.ttf" if bold else "Arial.ttf",
             "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"]
    for folder in ("/System/Library/Fonts/Supplemental", "/usr/share/fonts/truetype/dejavu"):
        for name in names:
            try:
                return ImageFont.truetype(os.path.join(folder, name), size)
            except OSError:
                pass
    return ImageFont.load_default()


def mix(image, box, first, second, sixteenths):
    """Fill box with `sixteenths`/16 of ink `second`, the rest `first`, pixel-exact."""
    x0, y0, x1, y1 = box
    pixels = image.load()
    for y in range(y0, y1):
        for x in range(x0, x1):
            ink = second if BAYER[y % 4][x % 4] < sixteenths else first
            pixels[x, y] = INK_RGB[ink]


def draw(width=800, height=480):
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    # No anti-aliasing: grey text edges would be dithered, and this card must
    # reach the panel pixel for pixel.
    d.fontmode = "1"
    small, bold = font(12), font(13, True)

    # 1. Solid inks.
    d.text((4, 1), "1  The six inks, solid", font=small, fill=TEXT)
    column = width // len(INKS)
    for i, (name, code) in enumerate(INKS):
        box = (i * column, 16, (i + 1) * column - 2, 58)
        d.rectangle(box, fill=INK_RGB[code], outline=TEXT)
        label = TEXT if code in (wgn6.WHITE, wgn6.YELLOW) else INK_RGB[wgn6.WHITE]
        d.text((box[0] + 5, 20), name, font=bold, fill=label)

    # 2. Every pair: 50/50 above the diagonal, 75/25 below.
    top, label_w, cell_h = 66, 64, 36
    cell_w = (width - label_w) // len(INKS)
    d.text((4, top), "2  Pairs: above the diagonal 50/50, below 3/4 row ink + 1/4 column ink",
           font=small, fill=TEXT)
    grid_top = top + 30
    for j, (name, _) in enumerate(INKS):
        d.text((label_w + j * cell_w + 4, grid_top - 14), name, font=bold, fill=TEXT)
    for i, (row_name, row_ink) in enumerate(INKS):
        y0 = grid_top + i * cell_h
        d.text((4, y0 + 11), row_name, font=bold, fill=TEXT)
        for j, (_, col_ink) in enumerate(INKS):
            x0 = label_w + j * cell_w
            box = (x0, y0, x0 + cell_w - 3, y0 + cell_h - 3)
            share = 16 if i == j else (8 if j > i else 4)
            mix(image, box, row_ink, col_ink, 0 if i == j else share)
            d.rectangle(box, outline=TEXT)

    # 3. Tint ramps.
    ramps_top = grid_top + len(INKS) * cell_h + 8
    d.text((4, ramps_top), "3  Ramps: 0, 25, 50, 75, 100 % of the second ink", font=small, fill=TEXT)
    ramp_h = (height - ramps_top - 18) // len(RAMPS)
    step_w = (width - 110) // len(STEPS)
    for r, (name, first, second) in enumerate(RAMPS):
        y0 = ramps_top + 16 + r * ramp_h
        d.text((4, y0 + 2), name, font=small, fill=TEXT)
        for s, sixteenths in enumerate(STEPS):
            x0 = 110 + s * step_w
            box = (x0, y0, x0 + step_w - 2, y0 + ramp_h - 2)
            mix(image, box, first, second, sixteenths)
    return image


def swatch(image, d, box, spec):
    """Fill box with a solid ink ("red") or a mix (("green", "blue", 8))."""
    if isinstance(spec, str):
        d.rectangle(box, fill=INK_RGB[CODE[spec]])
    else:
        first, second, share = spec
        mix(image, (box[0], box[1], box[2] + 1, box[3] + 1), CODE[first], CODE[second], share)


CODE = {name.lower(): code for name, code in INKS}
TEAL, CREAM, BROWN, PLUM = ("green", "blue", 8), ("white", "yellow", 8), ("red", "green", 8), ("red", "blue", 8)


def draw_harmonies(width=800, height=480):
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    d.fontmode = "1"
    tiny, small, bold, big = font(11), font(12), font(13, True), font(34, True)
    ink = lambda name: INK_RGB[CODE[name]]
    d.text((4, 1), "Harmonies: five palettes, smooth vs grainy mixes, text contrast", font=small, fill=TEXT)

    # 1. Five palettes as small screens.
    tile_w, top, tile_h = width // 5, 18, 196
    names = ["Ink & accent", "Primaries", "Warm morning", "Cool calm", "Earth"]
    for t, name in enumerate(names):
        x0, x1, y1 = t * tile_w + 3, (t + 1) * tile_w - 4, top + tile_h
        if t == 0:  # white, black, one red accent
            d.rectangle((x0, top, x1, y1), fill=ink("white"))
            d.rectangle((x0, top, x1, top + 8), fill=ink("red"))
            d.text((x0 + 10, top + 18), "21°", font=big, fill=ink("black"))
            for b, h in enumerate((40, 70, 55, 90, 30)):
                d.rectangle((x0 + 12 + b * 26, y1 - 12 - h, x0 + 30 + b * 26, y1 - 12),
                            fill=ink("red") if b == 3 else ink("black"))
        elif t == 1:  # primaries on white with black lines
            d.rectangle((x0, top, x1, y1), fill=ink("white"))
            mid_x, mid_y = x0 + 60, top + 110
            d.rectangle((x0, top, mid_x, mid_y), fill=ink("red"))
            d.rectangle((mid_x, mid_y, x1, y1), fill=ink("blue"))
            d.rectangle((x0, mid_y + 50, mid_x, y1), fill=ink("yellow"))
            for line in ((mid_x, top, mid_x, y1), (x0, mid_y, x1, mid_y), (x0, mid_y + 50, mid_x, mid_y + 50)):
                d.line(line, fill=ink("black"), width=5)
        elif t == 2:  # cream ground, red sun, black text
            swatch(image, d, (x0, top, x1, y1), CREAM)
            d.ellipse((x0 + 40, top + 22, x0 + 110, top + 92), fill=ink("red"))
            d.text((x0 + 22, top + 110), "7:30", font=big, fill=ink("black"))
            d.text((x0 + 24, top + 152), "Wake up", font=bold, fill=ink("black"))
        elif t == 3:  # white, blue, green, teal
            d.rectangle((x0, top, x1, y1), fill=ink("white"))
            d.text((x0 + 10, top + 12), "1.2 m", font=big, fill=ink("black"))
            d.text((x0 + 12, top + 52), "Surf, north-west", font=small, fill=ink("black"))
            swatch(image, d, (x0, top + 96, x1, top + 128), "blue")
            swatch(image, d, (x0, top + 132, x1, top + 160), TEAL)
            swatch(image, d, (x0, top + 164, x1, y1), "green")
        else:  # cream, green, brown, red
            swatch(image, d, (x0, top, x1, y1), CREAM)
            swatch(image, d, (x0 + 68, top + 100, x0 + 86, y1 - 10), BROWN)
            d.ellipse((x0 + 22, top + 24, x0 + 132, top + 116), fill=ink("green"))
            for cx, cy in ((x0 + 50, top + 60), (x0 + 96, top + 48), (x0 + 84, top + 88)):
                d.ellipse((cx - 7, cy - 7, cx + 7, cy + 7), fill=ink("red"))
        d.rectangle((x0, top, x1, y1), outline=ink("black"))
        d.text((x0 + 2, y1 + 3), name, font=bold, fill=TEXT)

    # 2. Mixes: smooth pairs (close lightness) beside grainy ones.
    mixes_top = top + tile_h + 26
    d.text((4, mixes_top), "Smooth mixes (inks of close lightness)", font=small, fill=TEXT)
    d.text((width // 2 + 4, mixes_top), "Grainy mixes (big lightness gap): distance only",
           font=small, fill=TEXT)
    smooth = [("teal", TEAL), ("cream", CREAM), ("brown", BROWN), ("plum", PLUM)]
    grainy = [("orange", ("red", "yellow", 8)), ("pink", ("red", "white", 8)),
              ("light blue", ("blue", "white", 8)), ("olive", ("black", "yellow", 8))]
    sw = (width // 2 - 8) // 4
    for k, (label, spec) in enumerate(smooth + grainy):
        x0 = 4 + k * sw + (8 if k >= 4 else 0)
        box = (x0, mixes_top + 16, x0 + sw - 8, mixes_top + 66)
        swatch(image, d, box, spec)
        d.rectangle(box, outline=ink("black"))
        d.text((x0, mixes_top + 69), label, font=tiny, fill=TEXT)

    # 3. Text contrast: pairings to use, and to avoid.
    text_top = mixes_top + 88
    d.text((4, text_top), "Text: use", font=small, fill=TEXT)
    pairs = [("black", "white", True), ("black", "yellow", True), ("white", "red", True),
             ("white", "green", True), ("white", "blue", True),
             ("white", "yellow", False), ("black", "red", False), ("black", "blue", False)]
    pw = (width - 8) // len(pairs)
    first_bad = next(k for k, pair in enumerate(pairs) if not pair[2])
    d.text((4 + first_bad * pw, text_top), "avoid", font=small, fill=TEXT)
    for k, (fg, bg, good) in enumerate(pairs):
        x0 = 4 + k * pw
        box = (x0, text_top + 16, x0 + pw - 6, height - 6)
        d.rectangle(box, fill=ink(bg), outline=ink("black"))
        d.text((x0 + 8, text_top + 22), "Aa", font=big, fill=ink(fg))
        d.text((x0 + 8, text_top + 62), f"{fg} on", font=tiny, fill=ink(fg))
        d.text((x0 + 8, text_top + 74), bg, font=tiny, fill=ink(fg))
    return image


ARCHIVO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..",
                                       "agent_skill", "skills", "wignet", "fonts"))


def archivo(size, black=False):
    """Archivo, the font the wignet skills ship (SIL OFL)."""
    name = "ArchivoBlack-Regular.ttf" if black else "Archivo-Medium.ttf"
    return ImageFont.truetype(os.path.join(ARCHIVO, name), size)


def draw_type(width=800, height=480):
    """Typography: sizes to judge from 1, 2 and 3 m, weights, colour text, crisp vs smoothed."""
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    d.fontmode = "1"
    ink = lambda name: INK_RGB[CODE[name]]
    label = archivo(13)
    d.text((16, 6), "Typography (Archivo): stand at 1, 2 and 3 m; note the smallest size you can read",
           font=label, fill=ink("black"))

    # 1. Size ladder, Archivo Medium, with the size and the distance it suits.
    y = 30
    for size, note in ((16, "1 m, just"), (20, "detail, 1 m"), (24, "detail, 2 m"),
                       (32, "support"), (40, "support, 2 m"), (48, "support, 3 m"),
                       (64, "glance, 2 m")):
        d.text((16, y + size * 0.25), f"{size}", font=label, fill=ink("black"))
        d.text((48, y), "Réveil 7:30", font=archivo(size), fill=ink("black"))
        d.text((48 + d.textlength("Réveil 7:30", font=archivo(size)) + 10, y + size * 0.25),
               note, font=label, fill=ink("black"))
        y += int(size * 1.22)

    # 2. The glance headline, and the two weights side by side.
    d.text((480, 22), "21°", font=archivo(110, black=True), fill=ink("black"))
    d.text((690, 64), "110 px", font=label, fill=ink("black"))
    d.text((690, 80), "glance, 3 m", font=label, fill=ink("black"))
    d.text((480, 162), "Medium 32", font=archivo(32), fill=ink("black"))
    d.text((480, 202), "Black 32", font=archivo(32, black=True), fill=ink("black"))
    d.text((480, 250), "Medium 20 · Samedi 14", font=archivo(20), fill=ink("black"))
    d.text((480, 276), "Black 20 · Samedi 14", font=archivo(20, black=True), fill=ink("black"))

    # 3. Colour text at 40 px (headline size): pairings and the one to avoid.
    top = 326
    d.text((16, top - 18), "Colour text, 40 px: headlines only, except black", font=label, fill=ink("black"))
    pairs = [("red", "white"), ("blue", "white"), ("green", "white"), ("black", "green"),
             ("black", "blue"), ("black", "yellow"), ("yellow", "white")]
    box_w = (width - 32) // len(pairs)
    for k, (fg, bg) in enumerate(pairs):
        x0 = 16 + k * box_w
        d.rectangle((x0, top, x0 + box_w - 6, top + 58), fill=ink(bg), outline=ink("black"))
        d.text((x0 + 8, top + 4), "Aa7", font=archivo(40, black=True), fill=ink(fg))
        tag = "avoid" if (fg, bg) == ("yellow", "white") else f"{fg} / {bg}"
        d.text((x0 + 2, top + 61), tag, font=archivo(11), fill=ink("black"))

    # 4. Crisp against smoothed: the smoothed half is left grey on purpose,
    #    so the converter dithers its edges as it would any anti-aliased text.
    top = 412
    d.text((16, top), "Crisp (fontmode 1)", font=label, fill=ink("black"))
    d.text((16, top + 18), "Lever du jour 7:12", font=archivo(24), fill=ink("black"))
    d.text((16, top + 46), "Coucher 20:41", font=archivo(16), fill=ink("red"))
    d.fontmode = "L"
    d.text((420, top), "Smoothed (anti-aliased)", font=label, fill=ink("black"))
    d.text((420, top + 18), "Lever du jour 7:12", font=archivo(24), fill=ink("black"))
    d.text((420, top + 46), "Coucher 20:41", font=archivo(16), fill=ink("red"))
    d.fontmode = "1"
    return image


TRMNL_DIR = None  # set by --trmnl: the folder holding TRMNL*.ttf (OFL, usetrmnl/trmnl-framework)
FONTS_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "fonts"))


def trmnl(native, scale, bold=False):
    """A TRMNL pixel font at an exact multiple of its native size: every stroke even."""
    name = f"TRMNL{native}-{'Bold' if bold else 'Regular'}.ttf"
    return ImageFont.truetype(os.path.join(TRMNL_DIR, name), native * scale)


def thresholded_text(image, xy, text, face, fill):
    """Text as the platform's message board draws it today: smoothed, then cut at 50 %."""
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).text(xy, text, font=face, fill=255)
    image.paste(fill, mask=mask.point(lambda v: 255 if v >= 128 else 0))


def draw_type2(width=800, height=480):
    """TRMNL pixel fonts against today's platform text, and fonts a sandbox has."""
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    d.fontmode = "1"
    black, red = INK_RGB[wgn6.BLACK], INK_RGB[wgn6.RED]
    label = trmnl(16, 1)
    samples = [("Réveil 7:30", 42, True), ("Samedi 14 sept.", 32, False), ("Pluie à 16 h, 12 °C", 24, False)]

    # Left: the platform today (Roboto, smoothed then thresholded).
    d.text((12, 6), "TODAY: Roboto, smoothed then cut", font=label, fill=red)
    y = 30
    for text, size, bold in samples:
        face = ImageFont.truetype(os.path.join(FONTS_DIR, "Roboto-Black.ttf" if bold else "Roboto-Bold.ttf"), size)
        thresholded_text(image, (12, y), text, face, black)
        d.text((12 + 370, y + 4), f"{size}", font=label, fill=black)
        y += int(size * 1.35)

    # Right: TRMNL at x2 (TRMNL21 Bold 42, TRMNL16 32, TRMNL12 24).
    d.text((412, 6), "TRMNL pixel fonts x2, bold", font=label, fill=red)
    y = 30
    for (text, size, bold), (native, scale) in zip(samples, ((21, 2), (16, 2), (12, 2))):
        d.text((412, y), text, font=trmnl(native, scale, bold=True), fill=black)
        d.text((770, y + 4), f"{size}", font=label, fill=black)
        y += int(size * 1.35)

    # Middle: x3 body and bold, and the headline question.
    d.line((12, 172, width - 12, 172), fill=black, width=2)
    d.text((12, 180), "TRMNL16 x3 = 48", font=label, fill=black)
    d.text((12, 198), "Lever 7:12", font=trmnl(16, 3), fill=black)
    d.text((12, 250), "Coucher 20:41", font=trmnl(16, 3, bold=True), fill=black)
    d.text((412, 180), "Archivo Black 96 | TRMNL21 Bold x4", font=label, fill=black)
    d.text((412, 196), "21°", font=archivo(96, black=True), fill=black)
    d.text((600, 206), "21°", font=trmnl(21, 4, bold=True), fill=black)

    # Bottom: fonts an agent's sandbox has, at sizes where they render evenly.
    d.line((12, 318, width - 12, 318), fill=black, width=2)
    d.text((12, 326), "In a sandbox: Pillow's default (Aileron), Arial, Verdana, crisp, at even sizes", font=label, fill=black)
    faces = [("Aileron 32, Pillow default: no à!", ImageFont.load_default(size=32)),
             ("Arial 24", ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 24)),
             ("Verdana Bold 20", ImageFont.truetype("/System/Library/Fonts/Supplemental/Verdana Bold.ttf", 20))]
    y = 350
    for name, face in faces:
        d.text((12, y), "Pluie à 16 h, 12 °C", font=face, fill=black)
        d.text((500, y + 6), name, font=label, fill=black)
        y += int(face.size * 1.45)
    return image


SYSTEM_FONTS = "/System/Library/Fonts/Supplemental"


def stems_even(face, size):
    """True if every vertical stem of "lIlIl nmhu" renders at one width, crisp."""
    from collections import Counter
    img = Image.new("1", (size * 12, size * 2), 1)
    draw = ImageDraw.Draw(img)
    draw.fontmode = "1"
    draw.text((2, 2), "lIlIl nmhu", font=face, fill=0)
    y, runs, run = int(2 + size * 0.62), Counter(), 0
    for x in range(img.width):
        if img.getpixel((x, y)) == 0:
            run += 1
        elif run:
            runs[run] += 1
            run = 0
    stems = [w for w in runs if w <= max(2, size * 0.3)]
    return len(stems) <= 1


def font_grid(rows, columns, title, row_h=56, width=800, height=480):
    """One row per font, one crisp sample per size, each marked = (even stems) or x.

    rows: [(name, size -> ImageFont)]; columns: [(size, sample text)].
    """
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    d.fontmode = "1"
    black, red = INK_RGB[wgn6.BLACK], INK_RGB[wgn6.RED]
    label = ImageFont.truetype(os.path.join(SYSTEM_FONTS, "Verdana.ttf"), 12)
    mark = ImageFont.truetype(os.path.join(SYSTEM_FONTS, "Verdana Bold.ttf"), 14)
    tallest = max(size for size, _ in columns)
    # Each column as wide as its widest sample, plus room for the mark.
    widths = [max(int(d.textlength(text, font=make(size))) for _, make in rows) + 22
              for size, text in columns]
    d.text((6, 4), title + "   = every stem the same width,  x stems mixed (measured)",
           font=label, fill=black)
    top, name_w = 24, 136
    for r, (name, make) in enumerate(rows):
        y = top + r * row_h
        d.text((6, y + row_h // 2 - 8), name, font=label, fill=black)
        x = name_w
        for (size, text), column_w in zip(columns, widths):
            face = make(size)
            d.text((x, y + (tallest - size)), text, font=face, fill=black)
            even = stems_even(face, size)
            d.text((x + column_w - 18, y + tallest - 14), "=" if even else "x", font=mark,
                   fill=black if even else red)
            x += column_w
        if r:
            d.line((6, y - 3, width - 6, y - 3), fill=black)
    return image


def _file_font(path):
    return lambda size: ImageFont.truetype(path, size)


def draw_type3(width=800, height=480):
    """Arial, Verdana and Archivo weights at 20, 24, 32 and 48 px, crisp."""
    rows = [(name, _file_font(path)) for name, path in (
        ("Arial", os.path.join(SYSTEM_FONTS, "Arial.ttf")),
        ("Arial Bold", os.path.join(SYSTEM_FONTS, "Arial Bold.ttf")),
        ("Verdana", os.path.join(SYSTEM_FONTS, "Verdana.ttf")),
        ("Verdana Bold", os.path.join(SYSTEM_FONTS, "Verdana Bold.ttf")),
        ("Archivo Medium", os.path.join(FONTS_DIR, "Archivo-Medium.ttf")),
        ("Archivo Bold", os.path.join(FONTS_DIR, "Archivo-Bold.ttf")),
        ("Archivo ExtraBold", os.path.join(FONTS_DIR, "Archivo-ExtraBold.ttf")),
        ("Archivo Black", os.path.join(FONTS_DIR, "ArchivoBlack-Regular.ttf")))]
    columns = [(20, "Pluie à 16 h"), (24, "Pluie à 16 h"), (32, "Samedi 14"), (48, "7:30")]
    return font_grid(rows, columns, "Crisp at 20, 24, 32, 48 px.", 56, width, height)


FREE_DIR = None  # set by --free: Liberation, Arimo and DejaVu fonts (open licences)


def draw_type4(width=800, height=480):
    """Free equivalents under the originals: Arimo and Liberation for Arial, DejaVu for Verdana."""
    def arimo(bold):
        def make(size):
            face = ImageFont.truetype(os.path.join(FREE_DIR, "Arimo[wght].ttf"), size)
            face.set_variation_by_name("Bold" if bold else "Regular")
            return face
        return make
    liberation = os.path.join(FREE_DIR, "liberation-fonts-ttf-2.1.5")
    rows = [("Arial", _file_font(os.path.join(SYSTEM_FONTS, "Arial.ttf"))),
            ("  Arimo", arimo(False)),
            ("  Liberation Sans", _file_font(os.path.join(liberation, "LiberationSans-Regular.ttf"))),
            ("Arial Bold", _file_font(os.path.join(SYSTEM_FONTS, "Arial Bold.ttf"))),
            ("  Arimo Bold", arimo(True)),
            ("  Liberation Bold", _file_font(os.path.join(liberation, "LiberationSans-Bold.ttf"))),
            ("Verdana", _file_font(os.path.join(SYSTEM_FONTS, "Verdana.ttf"))),
            ("  DejaVu Sans", _file_font(os.path.join(FREE_DIR, "dejavu-sans-ttf-2.37", "ttf", "DejaVuSans.ttf"))),
            ("Verdana Bold", _file_font(os.path.join(SYSTEM_FONTS, "Verdana Bold.ttf"))),
            ("  DejaVu Sans Bold", _file_font(os.path.join(FREE_DIR, "dejavu-fonts-ttf-2.37", "ttf", "DejaVuSans-Bold.ttf")))]
    columns = [(12, "Pluie à 16 h, 12 °C"), (20, "Pluie à 16 h"), (24, "Pluie à 16 h"), (32, "Mardi 14")]
    return font_grid(rows, columns, "Originals, then free equivalents (indented).", 45, width, height)


BOARD_SIZES = (8, 12, 16, 24, 32, 48)
BOARD_SAMPLE = "août 12"  # an accent, rounds, digits; lowercase, like body text
OPEN_DIR = None  # set by --open: Ubuntu, Bitstream Vera, Selawik, Open Sans, PT Sans


def loader(path, index=0, variation=None):
    """size -> ImageFont, for a file, a face of a .ttc, or a named instance of a variable font."""
    def make(size):
        face = ImageFont.truetype(path, size, index=index)
        if variation:
            face.set_variation_by_name(variation)
        return face
    return make


def font_board(title, families, width=800, height=480):
    """Each family in Regular and Bold at BOARD_SIZES, crisp, on one baseline per row.

    families: [(name, regular loader, bold loader or None)].
    """
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    d.fontmode = "1"
    black = INK_RGB[wgn6.BLACK]
    label = ImageFont.truetype(os.path.join(SYSTEM_FONTS, "Verdana.ttf"), 12)
    rows = []
    for name, regular, bold in families:
        rows.append((name, regular))
        rows.append(("   bold", bold))
    name_w = 6 + 16 + max(int(d.textlength(name, font=label)) for name, _ in rows)
    widths = [max(int(d.textlength(BOARD_SAMPLE, font=make(size))) for _, make in rows if make) + 10
              for size in BOARD_SIZES]
    d.text((6, 4), title, font=label, fill=black)
    x = name_w
    for size, column_w in zip(BOARD_SIZES, widths):
        d.text((x, 22), f"{size} px", font=label, fill=black)
        x += column_w
    top, row_h = 40, (height - 44) // len(rows)
    for r, (name, make) in enumerate(rows):
        baseline = top + r * row_h + row_h - 6
        d.text((6, baseline), name, font=label, fill=black, anchor="ls")
        if make is None:
            d.text((name_w, baseline), "(no bold in this family)", font=label, fill=black, anchor="ls")
            continue
        x = name_w
        for size, column_w in zip(BOARD_SIZES, widths):
            d.text((x, baseline), BOARD_SAMPLE, font=make(size), fill=black, anchor="ls")
            x += column_w
    return image


def draw_fonts_microsoft(width=800, height=480):
    f = lambda name: os.path.join(SYSTEM_FONTS, name)
    return font_board("Microsoft screen fonts (hand-hinted for 1990s black-and-white screens)", [
        ("Arial", loader(f("Arial.ttf")), loader(f("Arial Bold.ttf"))),
        ("Verdana", loader(f("Verdana.ttf")), loader(f("Verdana Bold.ttf"))),
        ("Tahoma", loader(f("Tahoma.ttf")), loader(f("Tahoma Bold.ttf"))),
        ("Trebuchet MS", loader(f("Trebuchet MS.ttf")), loader(f("Trebuchet MS Bold.ttf"))),
        ("Georgia", loader(f("Georgia.ttf")), loader(f("Georgia Bold.ttf"))),
    ], width, height)


def draw_fonts_apple(width=800, height=480):
    f = lambda name: os.path.join("/System/Library/Fonts", name)
    return font_board("Apple system fonts", [
        ("Lucida Grande", loader(f("LucidaGrande.ttc"), 0), loader(f("LucidaGrande.ttc"), 1)),
        ("Geneva", loader(f("Geneva.ttf")), None),
        ("Helvetica", loader(f("Helvetica.ttc"), 0), loader(f("Helvetica.ttc"), 1)),
        ("Helvetica Neue", loader(f("HelveticaNeue.ttc"), 0), loader(f("HelveticaNeue.ttc"), 1)),
        ("San Francisco", loader(f("SFNS.ttf"), variation="Regular"), loader(f("SFNS.ttf"), variation="Bold")),
    ], width, height)


def draw_fonts_open(width=800, height=480):
    f = lambda *parts: os.path.join(OPEN_DIR, *parts)
    return font_board("Open-source fonts known for screen hinting", [
        ("Ubuntu", loader(f("Ubuntu-Regular.ttf")), loader(f("Ubuntu-Bold.ttf"))),
        ("Bitstream Vera", loader(f("ttf-bitstream-vera-1.10", "Vera.ttf")),
         loader(f("ttf-bitstream-vera-1.10", "VeraBd.ttf"))),
        ("Selawik", loader(f("selawik", "selawk.ttf")), loader(f("selawik", "selawkb.ttf"))),
        ("Open Sans", loader(f("OpenSans[wdth,wght].ttf"), variation="Regular"),
         loader(f("OpenSans[wdth,wght].ttf"), variation="Bold")),
        ("PT Sans", loader(f("PT_Sans-Web-Regular.ttf")), loader(f("PT_Sans-Web-Bold.ttf"))),
    ], width, height)


MIX_ROWS = [("orange", "red", "yellow"), ("cream", "white", "yellow"), ("pink", "white", "red"),
            ("light blue", "white", "blue"), ("teal", "green", "blue"), ("grey", "black", "white")]
PATTERNS = ("checker", "rows", "columns", "diffused", "photo")


def pattern(image, box, first, second, kind):
    """Fill box with a 50/50 mix of two inks, laid out as `kind`.

    checker: a pixel checkerboard (fill_mix at 8); rows and columns: one-pixel
    stripes, the same count of each ink in another arrangement; diffused: the
    average of the two pure values, as Floyd-Steinberg gives a flat colour an
    agent draws; photo: the average of the two inks as measured, through the
    photo converter.
    """
    x0, y0, x1, y1 = box
    a, b = CODE[first], CODE[second]
    if kind in ("diffused", "photo"):
        inks = wgn6.RGB_PALETTE if kind == "diffused" else wgn6.PHOTO_INKS
        target = tuple((inks[a][c] + inks[b][c]) // 2 for c in range(3))
        flat = Image.new("RGB", (x1 - x0, y1 - y0), target)
        quantized = (wgn6.quantize_image(flat, dither=True) if kind == "diffused"
                     else wgn6.quantize_photo(flat))
        image.paste(quantized.convert("RGB"), (x0, y0))
        return
    pixels = image.load()
    for y in range(y0, y1):
        for x in range(x0, x1):
            if kind == "checker":
                second_here = (x + y) % 2
            else:
                second_here = (y if kind == "rows" else x) % 2
            pixels[x, y] = INK_RGB[b if second_here else a]


def draw_calibrate(width=800, height=480):
    """The six inks solid, every ink as a text ground, and six mixes laid out
    five ways: what to look at (or measure) before trusting PHOTO_INKS."""
    image = Image.new("RGB", (width, height), INK_RGB[wgn6.WHITE])
    d = ImageDraw.Draw(image)
    d.fontmode = "1"
    small, tiny = font(12), font(11)
    ink = lambda name: INK_RGB[CODE[name]]
    names = [name.lower() for name, _ in INKS]
    column = width // len(names)

    # 1. Solid patches, no outline, large enough to measure or photograph.
    d.text((4, 1), "1  The six inks, solid (measure the middle of each patch)", font=small, fill=TEXT)
    for i, name in enumerate(names):
        box = (i * column, 16, (i + 1) * column - 3, 86)
        d.rectangle(box, fill=ink(name))
        label = "white" if name in ("black", "red", "green", "blue") else "black"
        d.text((box[0] + 5, box[3] - 15), name, font=tiny, fill=ink(label))

    # 2. Text on each ink at the skill's sizes: 40 px headline (Archivo
    # Black), 24 and 16 px (Archivo Medium). Rows: black text, white text,
    # the column's ink as text on white.
    top, row_h = 92, 56
    d.text((4, top), "2  Text: black on each ink, white on each ink, each ink on white (40, 24, 16 px)",
           font=small, fill=TEXT)
    for r, (fg_of, bg_of) in enumerate(((lambda n: "black", lambda n: n),
                                        (lambda n: "white", lambda n: n),
                                        (lambda n: n, lambda n: "white"))):
        y0 = top + 16 + r * row_h
        for i, name in enumerate(names):
            fg, bg = fg_of(name), bg_of(name)
            x0 = i * column
            box = (x0, y0, x0 + column - 3, y0 + row_h - 4)
            d.rectangle(box, fill=ink(bg))
            if fg == bg:
                continue
            d.text((x0 + 6, y0 + 2), "Aa", font=archivo(40, black=True), fill=ink(fg))
            d.text((x0 + 68, y0 + 4), "7:30", font=archivo(24), fill=ink(fg))
            d.text((x0 + 68, y0 + 32), "Réveil", font=archivo(16), fill=ink(fg))

    # 3. Mixes, the same two inks half and half, laid out five ways: the
    # arrangement alone can change the colour.
    top = top + 16 + 3 * row_h + 2
    label_w = 80
    cell_w = (width - label_w) // len(PATTERNS)
    d.text((4, top), "3  50/50", font=small, fill=TEXT)
    for k, kind in enumerate(PATTERNS):
        d.text((label_w + k * cell_w + 2, top), kind, font=small, fill=TEXT)
    row_h = (height - top - 16) // len(MIX_ROWS)
    for r, (label, first, second) in enumerate(MIX_ROWS):
        y0 = top + 16 + r * row_h
        d.text((4, y0 + row_h // 2 - 7), label, font=small, fill=TEXT)
        for k, kind in enumerate(PATTERNS):
            x0 = label_w + k * cell_w
            pattern(image, (x0, y0, x0 + cell_w - 3, y0 + row_h - 3), first, second, kind)
    return image


CARDS = {"harmonies": draw_harmonies, "calibrate": draw_calibrate, "mixing": draw, "type": draw_type, "type2": draw_type2,
         "type3": draw_type3, "type4": draw_type4, "fonts-microsoft": draw_fonts_microsoft,
         "fonts-apple": draw_fonts_apple, "fonts-open": draw_fonts_open}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", nargs="?", default="testcard.png")
    parser.add_argument("--card", choices=list(CARDS), default="harmonies")
    parser.add_argument("--trmnl", help="folder with the TRMNL*.ttf pixel fonts (for type2)")
    parser.add_argument("--free", help="folder with Arimo, Liberation and DejaVu (for type4)")
    parser.add_argument("--open", help="folder with the open-source screen fonts (for fonts-open)")
    args = parser.parse_args()
    TRMNL_DIR = args.trmnl
    FREE_DIR = args.free
    OPEN_DIR = args.open
    CARDS[args.card]().save(args.output)
    print(args.output)

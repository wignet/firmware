#!/usr/bin/env python3
"""LVGL spike: the test scene's pictures, in LVGL v9's own image formats.

Writes, next to this file:
- background.bin: 320x240 RGB565, little-endian, stride 640 (LV_COLOR_FORMAT_RGB565).
- walker.bin: ten 48x48 frames, each RGB565A8 (LV_COLOR_FORMAT_RGB565A8): the
  RGB565 plane (stride 96) then the A8 plane (stride 48), 6912 bytes a frame.
  Frames 0-3 walk right, 4-5 happy, 6-9 walk left (0-3 mirrored).
- with --preview DIR: preview_*.ppm in DIR, to look at them on the Mac.

Stands in for what the server would do (draw, calibrate nothing, convert to
the device's native format); the firmware embeds the .bin files as they are
(board_build.embed_files). No dependency beyond the standard library.
"""
from __future__ import annotations

import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SS = 4  # supersampling per axis: soft edges, which the A8 plane keeps

CREAM = (0xF7, 0xF2, 0xE8)
INK = (0x33, 0x20, 0x21)
ROSE = (0xD5, 0x66, 0x68)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rgb565(c):
    r, g, b = (max(0, min(255, round(v))) for v in c)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


# ---------------------------------------------------------------- sprite --
# A shape is (inside(x, y) -> bool, colour); later shapes paint over earlier.

def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0


def arc(cx, cy, r, width, upward=True):
    """A ^ (upward) or a smile: half a ring."""
    def inside(x, y):
        d = math.hypot(x - cx, y - cy)
        return abs(d - r) <= width / 2 and ((y < cy) if upward else (y > cy))
    return inside


def character(frame: str, step: int):
    """Shapes for one frame: 'walk' (step 0-3) or 'happy' (step 0-1)."""
    bob = (0, -1, 0, -1)[step] if frame == "walk" else (-4, -7)[step]
    cy = 27 + bob
    shapes = []
    # Feet: walking swaps them forward and back.
    if frame == "walk":
        lead = (4, 1, -4, 1)[step]
        shapes += [(ellipse(18 + lead, 43, 5.5, 3.2), INK),
                   (ellipse(30 - lead, 43, 5.5, 3.2), INK)]
    else:
        shapes += [(ellipse(17, 43 + bob // 2, 5, 3), INK),
                   (ellipse(31, 43 + bob // 2, 5, 3), INK)]
    # Arms: up when happy.
    if frame == "happy":
        wave = (0, 2)[step]
        shapes += [(ellipse(8, cy - 12 - wave, 3.5, 6.5), ROSE),
                   (ellipse(40, cy - 12 + wave, 3.5, 6.5), ROSE)]
    shapes += [(ellipse(24, cy, 17, 16), INK),          # outline
               (ellipse(24, cy, 15.5, 14.5), ROSE),     # body
               (ellipse(24, cy + 5, 9, 7), CREAM)]      # belly
    if frame == "walk":
        shapes += [(ellipse(29, cy - 5, 2.4, 3.4), INK),  # eyes, looking right
                   (ellipse(36, cy - 5, 2.4, 3.4), INK)]
    else:
        shapes += [(arc(22, cy - 4, 3.2, 1.6), INK),      # ^ ^
                   (arc(31, cy - 4, 3.2, 1.6), INK),
                   (ellipse(17, cy + 0, 2.6, 1.6), (0xF0, 0x9A, 0x9A)),
                   (ellipse(36, cy + 0, 2.6, 1.6), (0xF0, 0x9A, 0x9A)),
                   (arc(26.5, cy + 1, 3, 1.4, upward=False), INK)]
    return shapes


def render_frame(shapes, mirror=False, size=48):
    """Premultiplied supersampling: returns rows of (rgb, alpha 0-255)."""
    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0]
            covered = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    if mirror:
                        px = size - px
                    colour = None
                    for inside, c in shapes:
                        if inside(px, py):
                            colour = c
                    if colour is not None:
                        covered += 1
                        for i in range(3):
                            acc[i] += colour[i]
            if covered:
                rgb = tuple(v / covered for v in acc)
            else:
                rgb = (0, 0, 0)
            row.append((rgb, round(255 * covered / (SS * SS))))
        pixels.append(row)
    return pixels


def rgb565a8(pixels):
    colour = b"".join(struct.pack("<H", rgb565(rgb)) for row in pixels for rgb, _ in row)
    alpha = bytes(a for row in pixels for _, a in row)
    return colour + alpha


# ------------------------------------------------------------ background --

def background(w=320, h=240):
    sky_top, sky_low = (0xE9, 0xF0, 0xF2), CREAM
    hill, hill_far = (0x9C, 0xB8, 0x8E), (0xC4, 0xD4, 0xB4)
    ground, ground_dark = (0xD8, 0xC7, 0xA6), (0xC9, 0xB5, 0x90)
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            c = mix(sky_top, sky_low, y / 150)
            if math.hypot(x - (w - 58), y - 48) <= 22:                # sun
                c = (0xF2, 0xC4, 0x6B)
            if y > 118 + 14 * math.sin(x / 41.0 + 1.3):               # far hills
                c = hill_far
            if y > 132 + 10 * math.sin(x / 27.0):                     # near hills
                c = hill
            if y >= 150:                                              # ground
                c = mix(ground, ground_dark, (y - 150) / 90)
                if (x // 8 + y // 6) % 9 == 0 and y % 6 == 0:         # pebbles
                    c = ground_dark
            row.append(c)
        rows.append(row)
    return rows


def write_ppm(path, rows, alpha_over=None):
    h, w = len(rows), len(rows[0])
    out = bytearray(f"P6 {w} {h} 255\n".encode())
    for row in rows:
        for px in row:
            if alpha_over is not None:
                rgb, a = px
                c = mix(alpha_over, rgb, a / 255)
            else:
                c = px
            out += bytes(max(0, min(255, round(v))) for v in c)
    path.write_bytes(bytes(out))


def main():
    preview = Path(sys.argv[sys.argv.index("--preview") + 1]) if "--preview" in sys.argv else None
    bg = background()
    (HERE / "background.bin").write_bytes(
        b"".join(struct.pack("<H", rgb565(c)) for row in bg for c in row))
    if preview:
        write_ppm(preview / "preview_background.ppm", bg)
    # The 1.54" board (240x240, env waveshare_esp32s3_lcd_154_lvgl).
    square = background(240, 240)
    (HERE / "background_240.bin").write_bytes(
        b"".join(struct.pack("<H", rgb565(c)) for row in square for c in row))

    frames = [render_frame(character("walk", s)) for s in range(4)]
    frames += [render_frame(character("happy", s)) for s in range(2)]
    frames += [render_frame(character("walk", s), mirror=True) for s in range(4)]
    sheet = b"".join(rgb565a8(f) for f in frames)
    assert len(sheet) == 10 * 48 * 48 * 3
    (HERE / "walker.bin").write_bytes(sheet)
    if preview:
        strip = [sum((f[y] for f in frames), []) for y in range(48)]
        write_ppm(preview / "preview_walker.ppm", strip, alpha_over=(0xE0, 0xE0, 0xE0))
    print(f"background.bin {320 * 240 * 2} bytes, walker.bin {len(sheet)} bytes")


if __name__ == "__main__":
    main()

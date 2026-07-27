#!/usr/bin/env python3
"""Generate the Magic Dingus Box app icons.

Run from anywhere:  python3 generate_icons.py

WHY A RASTER CROP AND NOT THE SVG
The mark exists as clean vector art (MDB elements/Magic Dingus Box Face
Graphic.svg), but no SVG rasteriser is installed on the dev Mac or the Pi
— rsvg-convert, inkscape, imagemagick and cairosvg are all absent. Rather
than add a build dependency for an asset that changes approximately never,
the mark is cropped out of the shipped banner raster, which needs only
Pillow. The outputs are committed; this script exists so they can be
regenerated deterministically, not so they can be built on demand.

WHERE THE MARK IS
The wordmark logo.png reads "agic Dingus Box" — there is no letter M,
because the M *is* the isometric cube, drawn as a separate glyph at the
left. A column-occupancy scan of the 32768x4734 banner finds the first
content run at x[416, 4592], i.e. 4176x4734 => aspect 0.882. That is the
cube, and it is near-square, so it centres in a square canvas with even
padding and no cropping.

SIZING RULES (these are not arbitrary)
- iOS apple-touch-icon: square, fully opaque, corners NOT rounded. iOS
  applies its own squircle mask; baking rounded corners in leaves black
  wedges outside the mask.
- "any" icons: mark at 72% of canvas height — visually balanced full-bleed.
- "maskable" icons: mark at 60%. Android's safe zone is a circle of 80%
  diameter; at 0.882 aspect a 72% mark clips its own left/right frame
  corners under that mask, and 60% is the largest that stays inside.
"""
import os
import sys

from PIL import Image

Image.MAX_IMAGE_PIXELS = None

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "images", "logo.png"))

# Theme background — matches theme.cpp and the existing manifest's
# background_color/theme_color, so the icon sits on the product's own dark.
BG = (0x1F, 0x19, 0x1F, 255)

# Cube glyph within the banner, measured not guessed (see module docstring).
CUBE_BOX = (416, 0, 4592, 4734)


def build(size: int, mark_frac: float, out_name: str) -> None:
    src = Image.open(SRC).convert("RGBA")
    mark = src.crop(CUBE_BOX)

    # Trim any fully-transparent margin so mark_frac describes the visible
    # artwork rather than whatever padding the crop happened to include.
    bbox = mark.getbbox()
    if bbox:
        mark = mark.crop(bbox)

    target_h = int(round(size * mark_frac))
    scale = target_h / mark.height
    target_w = max(1, int(round(mark.width * scale)))
    mark = mark.resize((target_w, target_h), Image.LANCZOS)

    canvas = Image.new("RGBA", (size, size), BG)
    canvas.alpha_composite(mark, ((size - target_w) // 2, (size - target_h) // 2))

    # Flatten: iOS refuses transparency in apple-touch-icon and renders it
    # black anyway, so bake the background in for every output.
    out = Image.new("RGB", (size, size), BG[:3])
    out.paste(canvas, mask=canvas.split()[3])
    path = os.path.join(HERE, out_name)
    out.save(path, "PNG", optimize=True)
    print("  %-26s %4dpx  mark %d%%" % (out_name, size, round(mark_frac * 100)))


def main() -> int:
    if not os.path.exists(SRC):
        print("source not found: %s" % SRC, file=sys.stderr)
        return 1
    print("generating icons from %s" % os.path.basename(SRC))
    # iOS home screen. 180 is the size current iPhones actually request.
    build(180, 0.72, "icon-180.png")
    # Web app manifest, purpose "any".
    build(192, 0.72, "icon-192.png")
    build(512, 0.72, "icon-512.png")
    # Web app manifest, purpose "maskable" — smaller mark, see docstring.
    build(192, 0.60, "icon-maskable-192.png")
    build(512, 0.60, "icon-maskable-512.png")
    # Favicon. The frame detail degrades below ~48px; acceptable for a tab.
    build(32, 0.80, "icon-32.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

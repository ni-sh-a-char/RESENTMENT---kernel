#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render the project's social preview images into media/.

Kept in the tree rather than done by hand in an image editor, for the same
reason the documentation site is generated: an asset nobody can rebuild is an
asset that goes stale the first time the version number changes.

The palette and the geometry are the site's, so the card and the page look like
the same project. Two accents on a near-black ground: brass for time, because
Kaalka reads the angles between the hands of a clock, and cyan for the digest,
because every object in the kernel is a hash. They never appear on the same
element.

  python tools/mksocial.py                 # all sizes, into media/
  python tools/mksocial.py --fonts DIR     # use TTFs from DIR instead of fetching

Fonts are Inter and JetBrains Mono, both SIL Open Font Licence, which is what
makes it legitimate to bake them into a distributable image. They are fetched
once and cached; pass --fonts to work offline.
"""

import argparse
import os
import re
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError as exc:
    # Print what actually went wrong. "install pillow" is unhelpful advice when
    # pillow is installed and something inside it failed to import.
    print("cannot import Pillow: %s" % exc)
    print("if it is not installed:  python -m pip install pillow")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "media")
CACHE = os.path.join(ROOT, "build", "fonts")

BG        = (8, 9, 12)
GRID      = (28, 33, 43)
FG        = (230, 233, 239)
MUTED     = (154, 164, 182)
FAINT     = (107, 116, 136)
BRASS     = (224, 165, 69)
CYAN      = (79, 214, 210)
GREEN     = (100, 214, 138)
PANEL     = (16, 19, 25)
LINE      = (42, 49, 63)

FONTS = {
    "Inter-Regular.ttf":         "Inter:wght@400",
    "Inter-SemiBold.ttf":        "Inter:wght@600",
    "Inter-Bold.ttf":            "Inter:wght@700",
    "JetBrainsMono-Regular.ttf": "JetBrains+Mono:wght@400",
}


# ------------------------------------------------------------------- fonts

def ensure_fonts(explicit):
    if explicit:
        return explicit

    os.makedirs(CACHE, exist_ok=True)
    missing = [f for f in FONTS if not os.path.exists(os.path.join(CACHE, f))]
    if not missing:
        return CACHE

    import urllib.request
    ua = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
    for name in missing:
        url = "https://fonts.googleapis.com/css2?family=" + FONTS[name]
        css = urllib.request.urlopen(
            urllib.request.Request(url, headers=ua), timeout=30).read().decode()
        m = re.search(r"url\((https://[^)]+\.ttf)\)", css)
        if not m:
            print("could not find a ttf for %s" % name)
            sys.exit(1)
        data = urllib.request.urlopen(
            urllib.request.Request(m.group(1), headers=ua), timeout=30).read()
        with open(os.path.join(CACHE, name), "wb") as fh:
            fh.write(data)
        print("  fetched %s" % name)
    return CACHE


def load(fdir, name, size):
    return ImageFont.truetype(os.path.join(fdir, name), size)


def fit(fdir, name, lines, max_w, start):
    """The largest size at which every line fits the column.

    Measured rather than guessed, because the column narrows when the card
    carries a terminal panel and a headline chosen for the wide layout then
    runs underneath it - which is exactly what happened."""
    size = start
    while size > 12:
        f = load(fdir, name, size)
        if max(f.getlength(t) for t in lines) <= max_w:
            return f
        size -= 2
    return load(fdir, name, 12)


# ------------------------------------------------------------------ pieces

def grid(img, step, fade_from):
    """The faint lattice, fading out downwards. It is the kernel's own idea:
    everything is a node on a graph whose edges you can hash."""
    w, h = img.size
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for x in range(0, w, step):
        d.line([(x, 0), (x, h)], fill=GRID + (255,), width=1)
    for y in range(0, h, step):
        d.line([(0, y), (w, y)], fill=GRID + (255,), width=1)

    mask = Image.new("L", img.size, 0)
    md = ImageDraw.Draw(mask)
    for y in range(h):
        t = max(0.0, 1.0 - max(0.0, y - fade_from) / float(h - fade_from))
        md.line([(0, y), (w, y)], fill=int(150 * t))
    layer.putalpha(mask)
    img.alpha_composite(layer)


def glow(img, cx, cy, radius, colour, strength):
    """A soft radial wash. Drawn as concentric circles because a real blur over
    an image this size costs more than it is worth here."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    steps = 60
    for i in range(steps, 0, -1):
        r = radius * i / steps
        a = int(strength * (1.0 - i / float(steps)) ** 2)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=colour + (a,))
    img.alpha_composite(layer)


def gradient_text(img, xy, text, font, c0, c1):
    """Pillow cannot fill text with a gradient, so the text becomes a mask and
    the gradient is pasted through it."""
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).text(xy, text, font=font, fill=255)

    box = mask.getbbox()
    if not box:
        return
    grad = Image.new("RGBA", img.size, c1 + (255,))
    gd = ImageDraw.Draw(grad)
    x0, x1 = box[0], box[2]
    for x in range(x0, x1 + 1):
        t = (x - x0) / float(max(1, x1 - x0))
        gd.line([(x, 0), (x, img.size[1])],
                fill=tuple(int(a + (b - a) * t) for a, b in zip(c0, c1)) + (255,))
    img.paste(grad, (0, 0), mask)


def clock_mark(d, cx, cy, r):
    """The logo: a dial with an hour hand in brass and a minute hand in cyan.
    Not decoration - it is the one image that says what the kernel is about."""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=LINE, width=max(2, r // 12))
    d.line([cx, cy, cx, cy - r * 0.62], fill=BRASS, width=max(2, r // 9))
    d.line([cx, cy, cx + r * 0.52, cy + r * 0.34], fill=CYAN, width=max(2, r // 9))
    k = max(2, r // 8)
    d.ellipse([cx - k, cy - k, cx + k, cy + k], fill=FG)


def big_dial(img, cx, cy, r):
    """A dial large enough to read as texture rather than as a logo, bleeding
    off the right edge. It fills what would otherwise be dead space with the
    one image that means something here: Kaalka keys from the angles between
    the hands, so the angles are what the card shows."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=LINE + (150,),
              width=max(2, int(r * 0.008)))
    rr = r * 0.82
    d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=LINE + (90,),
              width=max(1, int(r * 0.004)))

    import math
    for i in range(12):
        a = math.radians(i * 30 - 90)
        outer, inner = r * 0.94, r * (0.86 if i % 3 else 0.80)
        d.line([cx + math.cos(a) * inner, cy + math.sin(a) * inner,
                cx + math.cos(a) * outer, cy + math.sin(a) * outer],
               fill=(LINE if i % 3 else FAINT) + (190,),
               width=max(2, int(r * 0.006)))

    # 10:09, the angle every watch advertisement uses, because it is the one
    # that looks deliberate rather than accidental.
    for ang, colour, length, wid in ((-60.0, BRASS, 0.52, 0.020),
                                     (54.0,  CYAN,  0.74, 0.014)):
        a = math.radians(ang - 90)
        d.line([cx, cy, cx + math.cos(a) * r * length,
                cy + math.sin(a) * r * length],
               fill=colour + (205,), width=max(3, int(r * wid)))

    k = max(3, int(r * 0.030))
    d.ellipse([cx - k, cy - k, cx + k, cy + k], fill=BG + (255,),
              outline=FAINT + (220,), width=max(2, int(r * 0.007)))
    img.alpha_composite(layer)


def chip(d, x, y, text, font, colour):
    """A pill with a hairline border, matching the version badge on the site."""
    tw = d.textlength(text, font=font)
    th = font.size
    pad_x, pad_y = int(th * 0.7), int(th * 0.45)
    box = [x, y, x + tw + pad_x * 2, y + th + pad_y * 2]
    d.rounded_rectangle(box, radius=(th + pad_y * 2) // 2,
                        outline=colour, width=2)
    d.text((x + pad_x, y + pad_y - 1), text, font=font, fill=colour)
    return box[2] - box[0]


# ------------------------------------------------------------------- cards

def card(size, fdir, version, terminal=True):
    w, h = size
    s = w / 1280.0                       # everything scales off the 1280 design

    img = Image.new("RGBA", size, BG + (255,))
    grid(img, int(56 * s), int(h * 0.34))
    glow(img, w * 0.34, -h * 0.22, w * 0.60, BRASS, 46)
    glow(img, w * 1.05, h * 1.02, w * 0.46, CYAN, 30)

    if not (terminal and w >= 1500):
        big_dial(img, w * 0.885, h * 0.52, h * 0.46)

    d = ImageDraw.Draw(img)

    has_term = terminal and w >= 1500
    term_w   = int(w * 0.32)
    m0       = int(84 * s)
    # The text column stops short of the panel when there is one.
    col_w    = (w - term_w - m0 * 2 - int(56 * s)) if has_term else int(w * 0.62)

    head_lines = ["A capability-secure,", "AI-native kernel."]
    sub_lines  = ["Authority expires.  The system is a graph you can hash.",
                  "Inference is a scheduling class."]

    f_brand   = load(fdir, "Inter-SemiBold.ttf", int(30 * s))
    f_head    = fit(fdir, "Inter-Bold.ttf",    head_lines, col_w, int(78 * s))
    f_sub     = fit(fdir, "Inter-Regular.ttf", sub_lines,  col_w, int(26 * s))
    f_mono    = load(fdir, "JetBrainsMono-Regular.ttf", int(19 * s))
    f_chip    = load(fdir, "JetBrainsMono-Regular.ttf", int(17 * s))

    m = int(84 * s)                       # margin
    y = int(74 * s)

    # ---- brand row
    r = int(26 * s)
    clock_mark(d, m + r, y + r, r)
    d.text((m + r * 2 + int(22 * s), y + int(6 * s)), "RESENTMENT",
           font=f_brand, fill=FG)
    bw = d.textlength("RESENTMENT", font=f_brand)
    chip(d, m + r * 2 + int(22 * s) + bw + int(18 * s), y + int(6 * s),
         version, f_chip, BRASS)

    # ---- headline
    y = int(h * 0.27)
    d.text((m, y), head_lines[0], font=f_head, fill=FG)
    y += int(f_head.size * 1.18)
    gradient_text(img, (m, y), head_lines[1], f_head, BRASS, CYAN)
    d = ImageDraw.Draw(img)

    # ---- subline
    y += int(f_head.size * 1.42)
    d.text((m, y), sub_lines[0], font=f_sub, fill=MUTED)
    d.text((m, y + int(f_sub.size * 1.4)), sub_lines[1], font=f_sub, fill=MUTED)
    y += int(f_sub.size * 1.4)

    # The address, because a card that makes someone want the thing and then
    # does not say where it is has done half a job.
    d.text((m, y + int(f_sub.size * 2.0)),
           "github.com/ni-sh-a-char/RESENTMENT---kernel",
           font=f_mono, fill=FAINT)

    # ---- the strip along the bottom
    d.line([m, h - int(104 * s), w - m, h - int(104 * s)], fill=LINE)
    y = h - int(74 * s)
    items = [("x86_64", FAINT), ("aarch64", FAINT), ("riscv64", FAINT),
             ("SMP", CYAN), ("ring 3", CYAN),
             ("1440 + 168 tests", GREEN), ("0 dependencies", FAINT)]
    x = m
    for i, (text, colour) in enumerate(items):
        if i:
            d.text((x, y), "·", font=f_mono, fill=LINE)
            x += d.textlength("·", font=f_mono) + int(18 * s)
        d.text((x, y), text, font=f_mono, fill=colour)
        x += d.textlength(text, font=f_mono) + int(18 * s)

    # ---- a real boot transcript, on the wide cards only
    if has_term:
        tw, th = term_w, int(h * 0.50)
        tx, ty = w - tw - m, int(h * 0.25)
        d.rounded_rectangle([tx, ty, tx + tw, ty + th], radius=int(14 * s),
                            fill=PANEL, outline=LINE, width=2)
        for i, cx in enumerate((0.30, 0.55, 0.80)):
            c = [(224, 85, 97), (217, 164, 65), (79, 181, 115)][i]
            r2 = int(6 * s)
            px = tx + int(22 * s) + i * int(20 * s)
            py = ty + int(22 * s)
            d.ellipse([px - r2, py - r2, px + r2, py + r2], fill=c)
        d.line([tx, ty + int(44 * s), tx + tw, ty + int(44 * s)], fill=LINE)

        lines = [
            ("smp      4 of 4 processors started", FAINT),
            ("selftest all 7 self-tests passed", GREEN),
            ("boot     complete in 119 ms", GREEN),
            ("", FG),
            ("resentment> .exec /boot/bin/init", FG),
            ("  hello from ring 3.", CYAN),
            ("", FG),
            ("resentment> .digest", FG),
            ("7d4a1f0e83c25b9a6f1e0d4c", BRASS),
        ]
        ly = ty + int(62 * s)
        f_t = load(fdir, "JetBrainsMono-Regular.ttf", int(15 * s))
        for text, colour in lines:
            d.text((tx + int(20 * s), ly), text, font=f_t, fill=colour)
            ly += int(24 * s)

    return img.convert("RGB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts", default=None,
                    help="directory of TTFs, instead of fetching them")
    ap.add_argument("--version", default="2.0.0")
    args = ap.parse_args()

    fdir = ensure_fonts(args.fonts)
    os.makedirs(OUT, exist_ok=True)

    sizes = [
        ("social-preview.png", (1280, 640), "GitHub repository social preview"),
        ("social-wide.png",    (1600, 900), "X, and anything 16:9"),
        ("social-linkedin.png", (1200, 627), "LinkedIn and Facebook"),
    ]
    for name, size, what in sizes:
        img = card(size, fdir, args.version)
        path = os.path.join(OUT, name)
        img.save(path, "PNG", optimize=True)
        print("  %-22s %4dx%-4d  %-36s %6.1f KiB"
              % (name, size[0], size[1], what,
                 os.path.getsize(path) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())

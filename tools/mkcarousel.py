#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render the Instagram carousel into media/carousel/.

Ten slides at 1080x1350. Instagram crops anything taller and wastes the feed on
anything squarer, and 4:5 is the tallest it will show uncropped.

The palette, the fonts, the dial and the grid are imported from mksocial rather
than restated, so the carousel and the preview cards are visibly the same
project. A second set of constants here would drift from the first within one
version bump.

Every number on these slides is one the suite actually produces. Nothing is
rounded up for the caption.

  python tools/mkcarousel.py                 # ten slides into media/carousel/
  python tools/mkcarousel.py --fonts DIR     # use TTFs from DIR instead of fetching
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import mksocial as ms                                    # noqa: E402
from PIL import Image, ImageDraw                         # noqa: E402

ROOT = ms.ROOT
OUT = os.path.join(ROOT, "media", "carousel")

W, H = 1080, 1350
MARGIN = 88

# The two halves of the verification claim, and a total that is derived. Same
# reasoning as tools/mkdocx.py: a headline figure that disagrees with its own
# parts three slides later is worse than no figure.
HOST_CHECKS = 1440
QEMU_CHECKS = 180
TOTAL_CHECKS = HOST_CHECKS + QEMU_CHECKS

REPO = "github.com/ni-sh-a-char/RESENTMENT---kernel"


# ------------------------------------------------------------------ slides
#
# eyebrow  small label above the headline, in an accent
# head     the statement, one entry per line, sized to fit
# body     supporting prose, one entry per line
# panel    optional monospace block: (line, colour) pairs
# stats    optional row of big figures: (number, label) pairs
# accent   which accent this slide leans on

SLIDES = [
    dict(
        cover=True,
        eyebrow="AN OPERATING SYSTEM KERNEL",
        head=["Authority", "that expires."],
        body=["A capability-secure, AI-native kernel",
              "for x86_64, ARM64 and RISC-V."],
        accent=ms.BRASS,
    ),
    dict(
        eyebrow="THE PROBLEM",
        head=["Your kernel", "trusts everything."],
        body=["On a normal system every process starts",
              "with the authority to try anything, and is",
              "stopped only by a check that remembers",
              "to say no.",
              "",
              "That is ambient authority. It is the default,",
              "and it is why one compromised process is",
              "usually the whole machine."],
        accent=ms.BRASS,
    ),
    dict(
        eyebrow="CAPABILITIES",
        head=["No ambient", "authority."],
        body=["Nothing here can be reached by naming it.",
              "You hold a capability, or you cannot act.",
              "",
              "Every access is checked four ways:"],
        panel=[("  type        is this the kind of object", ms.MUTED),
               ("              you think it is?", ms.FAINT),
               ("  rights      may you do this to it?", ms.MUTED),
               ("  generation  has it been recycled", ms.MUTED),
               ("              underneath you?", ms.FAINT),
               ("  seal        has your authority", ms.MUTED),
               ("              simply run out?", ms.BRASS)],
        accent=ms.CYAN,
    ),
    dict(
        eyebrow="KAALKA",
        head=["Time is", "the key."],
        body=["Every capability carries a cryptographic",
              "seal with its own deadline inside it.",
              "",
              "Forget to revoke a permission and it stops",
              "working anyway. Revocation is the default",
              "state, not an action someone has to",
              "remember to take.",
              "",
              "Q32.32 fixed point, byte-identical to the",
              "reference implementation."],
        accent=ms.BRASS,
    ),
    dict(
        eyebrow="THE RUNTIME GRAPH",
        head=["The whole", "machine has", "one hash."],
        body=["Every object is a node in a Merkle DAG.",
              "The digest excludes timestamps, ids and",
              "pointers, so it describes the state and",
              "not the run.",
              "",
              "Two machines in the same state produce",
              "the same digest. Snapshot it, replay it,",
              "diff it against yesterday."],
        panel=[("resentment> .digest", ms.FG),
               ("7d4a1f0e83c25b9a6f1e0d4c", ms.BRASS)],
        accent=ms.CYAN,
    ),
    dict(
        eyebrow="THE AI SUBSYSTEM",
        head=["Inference is a", "scheduling class."],
        body=["Not a background thread that starves the",
              "moment the machine is busy.",
              "",
              "SCHED_INFERENCE is earliest-deadline-first",
              "with a per-period budget and real admission",
              "control: a model that will not meet its",
              "deadline is refused at admission rather",
              "than accepted and missed.",
              "",
              "A transformer forward pass runs in-kernel",
              "on all three architectures."],
        accent=ms.CYAN,
    ),
    dict(
        eyebrow="PORTABILITY",
        head=["Three", "architectures.", "Every core."],
        body=["Not a port and two aspirations. The same",
              "features are built and tested on all three.",
              "",
              "SMP everywhere: ACPI MADT and a real-mode",
              "trampoline, PSCI CPU_ON, SBI HSM hart_start.",
              "Ring 3, demand paging and copy-on-write on",
              "each of them."],
        stats=[("x86_64", "SMP, ring 3"),
               ("ARM64", "SMP, ring 3"),
               ("RISC-V", "Sv39, ring 3")],
        accent=ms.BRASS,
    ),
    dict(
        eyebrow="VERIFICATION",
        head=["%s automated" % "{:,}".format(TOTAL_CHECKS), "checks."],
        body=["The host suite compiles the real kernel",
              "sources, not a mock of them.",
              "",
              "The QEMU suite drives the kernel's own",
              "shell across six targets: three",
              "architectures, uniprocessor and SMP."],
        stats=[("{:,}".format(HOST_CHECKS), "on the host"),
               (str(QEMU_CHECKS), "through the shell"),
               ("6", "QEMU targets")],
        accent=ms.GREEN,
    ),
    dict(
        eyebrow="BUILD IT",
        head=["Three", "commands.", "No dependencies."],
        body=["A compiler, make and QEMU. Nothing else",
              "to install, nothing to vendor."],
        panel=[("$ git clone %s" % REPO.replace("github.com/", "https://github.com/"),
                ms.MUTED),
               ("$ make", ms.MUTED),
               ("$ make qemu", ms.MUTED),
               ("", ms.FG),
               ("smp      4 of 4 processors started", ms.FAINT),
               ("selftest all 7 self-tests passed", ms.GREEN),
               ("boot     complete in 119 ms", ms.GREEN),
               ("", ms.FG),
               ("resentment> .exec /boot/bin/init", ms.FG),
               ("  hello from ring 3.", ms.CYAN)],
        accent=ms.GREEN,
    ),
    dict(
        eyebrow="OPEN SOURCE, APACHE 2.0",
        head=["Come and", "break it."],
        body=["The tracker has real work in it, scoped and",
              "pointing at the file and line it concerns.",
              "Four are labelled good first issue.",
              "",
              "Two confirmed defects are written up in",
              "full, because a project that hides its bugs",
              "is not one you should build on."],
        link=True,
        accent=ms.BRASS,
    ),
]


# ------------------------------------------------------------------- draw

def overflow(font, text, limit, what):
    """Refuse to render a line that does not fit its column.

    The headline is measured and shrunk by mksocial.fit(), but body text and
    panel lines are set at a fixed size and are broken by hand in SLIDES. This
    is what catches a hand-broken line that got one word too long."""
    w = font.getlength(text)
    if w > limit:
        raise SystemExit(
            "%s overruns its column by %dpx (%dpx of %dpx):\n  %r\n"
            "break the line in SLIDES, or shorten it."
            % (what, w - limit, w, limit, text))


def ground(img, accent, cover):
    """Grid, and two washes of light. The cover gets the dial as texture."""
    ms.grid(img, 48, int(H * 0.30))
    ms.glow(img, W * 0.28, -H * 0.10, W * 0.85, accent, 40)
    ms.glow(img, W * 1.08, H * 1.04, W * 0.55,
            ms.CYAN if accent is not ms.CYAN else ms.BRASS, 26)
    if cover:
        ms.big_dial(img, W * 0.80, H * 0.84, H * 0.28)


def brand_row(img, d, fdir, index, total, version):
    f_brand = ms.load(fdir, "Inter-SemiBold.ttf", 27)
    f_chip = ms.load(fdir, "JetBrainsMono-Regular.ttf", 15)
    f_num = ms.load(fdir, "JetBrainsMono-Regular.ttf", 17)

    r = 22
    y = 78
    ms.clock_mark(d, MARGIN + r, y + r, r)
    x = MARGIN + r * 2 + 18
    d.text((x, y + 6), "RESENTMENT", font=f_brand, fill=ms.FG)
    bw = d.textlength("RESENTMENT", font=f_brand)
    ms.chip(d, x + bw + 16, y + 5, version, f_chip, ms.BRASS)

    label = "%02d / %02d" % (index, total)
    d.text((W - MARGIN - d.textlength(label, font=f_num), y + 12),
           label, font=f_num, fill=ms.FAINT)


def footer(img, d, fdir, last):
    f = ms.load(fdir, "JetBrainsMono-Regular.ttf", 18)
    d.line([MARGIN, H - 132, W - MARGIN, H - 132], fill=ms.LINE)
    d.text((MARGIN, H - 104), REPO, font=f,
           fill=ms.BRASS if last else ms.FAINT)
    if not last:
        tag = "swipe"
        d.text((W - MARGIN - d.textlength(tag, font=f), H - 104),
               tag, font=f, fill=ms.FAINT)


def panel_block(img, d, fdir, lines, y):
    f = ms.load(fdir, "JetBrainsMono-Regular.ttf", 20)
    pad = 26
    lh = 32
    inner = W - MARGIN * 2 - pad * 2
    for text, _ in lines:
        # Monospace does not reflow, so an over-long line runs out through the
        # side of the panel and looks like a rendering bug rather than a typo.
        # Fail loudly at build time instead of shipping it to a feed.
        overflow(f, text, inner, "panel line")

    box_h = len(lines) * lh + pad * 2
    d.rounded_rectangle([MARGIN, y, W - MARGIN, y + box_h], radius=14,
                        fill=ms.PANEL, outline=ms.LINE, width=2)
    ly = y + pad
    for text, colour in lines:
        d.text((MARGIN + pad, ly), text, font=f, fill=colour)
        ly += lh
    return y + box_h


def stats_row(img, d, fdir, stats, y):
    f_n = ms.load(fdir, "Inter-Bold.ttf", 52)
    f_l = ms.load(fdir, "Inter-Regular.ttf", 20)
    col = (W - MARGIN * 2) / float(len(stats))
    for i, (num, label) in enumerate(stats):
        x = MARGIN + col * i
        if i:
            d.line([x - 22, y + 8, x - 22, y + 78], fill=ms.LINE)
        # A long word gets its own smaller size rather than overrunning the
        # column, which is what happened to "x86_64" at 52px.
        f = f_n
        while f.getlength(num) > col - 40 and f.size > 24:
            f = ms.load(fdir, "Inter-Bold.ttf", f.size - 3)
        d.text((x, y), num, font=f, fill=ms.FG)
        d.text((x, y + 66), label, font=f_l, fill=ms.FAINT)
    return y + 100


def slide(spec, fdir, index, total, version):
    cover = spec.get("cover", False)
    accent = spec.get("accent", ms.BRASS)

    img = Image.new("RGBA", (W, H), ms.BG + (255,))
    ground(img, accent, cover)
    d = ImageDraw.Draw(img)

    brand_row(img, d, fdir, index, total, version)

    # The body is drawn onto its own layer from the top, then composited
    # centred in the band between the brand row and the footer. Measuring the
    # block by rendering it is exact; deriving its height a second time from
    # font metrics would be a second implementation to keep in step, and the
    # slides carry between two and ten body lines each.
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    y = 0

    # ---- eyebrow
    f_eye = ms.load(fdir, "JetBrainsMono-Regular.ttf", 19)
    eye = spec["eyebrow"]
    ld.text((MARGIN, y), eye, font=f_eye, fill=accent)
    # a short rule under it, the width of the text
    ew = ld.textlength(eye, font=f_eye)
    ld.line([MARGIN, y + 34, MARGIN + ew, y + 34], fill=accent)
    y += 78

    # ---- headline
    head = spec["head"]
    f_head = ms.fit(fdir, "Inter-Bold.ttf", head, W - MARGIN * 2,
                    92 if cover else 76)
    for i, line in enumerate(head):
        # On the cover the last line carries the brass-to-cyan gradient, the
        # same one the preview card uses for its second headline line.
        if cover and i == len(head) - 1:
            ms.gradient_text(layer, (MARGIN, y), line, f_head, ms.BRASS, ms.CYAN)
            ld = ImageDraw.Draw(layer)
        else:
            ld.text((MARGIN, y), line, font=f_head, fill=ms.FG)
        y += int(f_head.size * 1.12)

    y += 34

    # ---- body
    f_body = ms.load(fdir, "Inter-Regular.ttf", 27)
    for line in spec.get("body", []):
        if line:
            overflow(f_body, line, W - MARGIN * 2, "body line")
            ld.text((MARGIN, y), line, font=f_body, fill=ms.MUTED)
        y += 40

    # ---- panel or stats
    if spec.get("panel"):
        y = panel_block(layer, ld, fdir, spec["panel"], y + 18)
    if spec.get("stats"):
        y = stats_row(layer, ld, fdir, spec["stats"], y + 24)

    # ---- the address, spelled out on the last slide
    if spec.get("link"):
        f_l = ms.load(fdir, "Inter-SemiBold.ttf", 30)
        ld.text((MARGIN, y + 26), "Star it, file an issue, send a patch.",
                font=f_l, fill=ms.FG)
        f_m = ms.load(fdir, "JetBrainsMono-Regular.ttf", 22)
        ld.text((MARGIN, y + 78), REPO, font=f_m, fill=ms.CYAN)
        ld.text((MARGIN, y + 116), "buymeacoffee.com/piyushmishra00",
                font=f_m, fill=ms.BRASS)

    compose(img, layer)
    footer(img, d, fdir, last=(index == total))
    return img.convert("RGB")


def compose(img, layer):
    """Centre the drawn block in the band between brand row and footer.

    Top-aligning left a third of every slide empty below the text, which on a
    feed reads as a template someone did not finish filling in."""
    box = layer.getbbox()
    if not box:
        return
    top, bottom = 214, H - 174
    dy = top + ((bottom - top) - (box[3] - box[1])) // 2 - box[1]
    dy = max(dy, top - box[1])                 # never ride up into the brand row

    shifted = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    src = layer.crop((0, box[1], W, min(H, box[3])))
    shifted.paste(src, (0, max(0, box[1] + dy)))
    img.alpha_composite(shifted)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fonts", default=None,
                    help="directory of TTFs, instead of fetching them")
    ap.add_argument("--version", default="2.0.0")
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()

    fdir = ms.ensure_fonts(args.fonts)
    os.makedirs(args.out, exist_ok=True)

    total = len(SLIDES)
    for i, spec in enumerate(SLIDES, 1):
        img = slide(spec, fdir, i, total, args.version)
        name = "slide-%02d.png" % i
        path = os.path.join(args.out, name)
        img.save(path, "PNG", optimize=True)
        print("  %-14s %4dx%-5d %-34s %6.1f KiB"
              % (name, W, H, spec["eyebrow"].lower(),
                 os.path.getsize(path) / 1024.0))

    write_alt_text(args.out, total)
    print("  %d slides in %s" % (total, args.out))
    return 0


def write_alt_text(out, total):
    """Alt text per slide, derived from the slide rather than written twice.

    Instagram takes alt text per image and almost nobody supplies it. These are
    images that are mostly words, so without it the post says nothing at all to
    anyone using a screen reader."""
    lines = ["RESENTMENT 2.0.0 - Instagram carousel",
             "",
             "%d slides, 1080x1350, post in numbered order." % total,
             "The caption and the rest of the platform copy are in",
             "media/RESENTMENT-2.0.0-social-kit.docx - kept there so there is",
             "one home for wording rather than two that disagree.",
             "",
             "Alt text, per slide:",
             ""]
    for i, spec in enumerate(SLIDES, 1):
        head = " ".join(spec["head"])
        body = next((b for b in spec.get("body", []) if b), "")
        lines.append("slide-%02d.png" % i)
        lines.append("  Dark slide headed \"%s\" under the label %s. %s"
                     % (head, spec["eyebrow"].title(), body))
        lines.append("")

    path = os.path.join(out, "alt-text.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))
    print("  %-14s %s" % ("alt-text.txt", "alt text for all %d slides" % total))


if __name__ == "__main__":
    sys.exit(main())

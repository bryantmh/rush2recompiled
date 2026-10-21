"""Builds the track select banners of the SF Rush and Rush 2049 tracks from 128x32 PNGs:
tools/rush1/banners/trackN_<name>.png (SF Rush track 1-7, src/rush1/track1_convert.cpp build_logo) and
tools/rush2049/banners/trackN_<name>.png (Rush 2049 race track 1-6, src/rush2049/track2049_art.cpp build_logo). A track without
a PNG gets its name drawn in its game's banner style with the banner font (tools/banner_font.py); the names come from
track_names in include/track1.h and include/track2049.h. Rush 2049's stunt arenas, obstacle course and battle arenas
follow its race tracks as banners 7-19 (STUNT 1-4, OBSTACLE, BATTLE 1-8), always drawn with the font.

Each banner becomes the format of Rush 2's track logos: 128x32 CI8 texels in rows top-down (build_logo flips them to
the bottom-up order the game stores) and a 256-entry RGBA5551 palette whose entry 0 is transparent. Pixels with alpha
below 128 are transparent; the rest are reduced to 5 bits a channel and quantized to the 255 remaining entries. The
game draws the banner with its exact colors instead (the full-color image, also in the header, replaces the CI8 one
in the renderer: rush2::upscale::add_exact_image), so the quantizing only shows if that replacement is missing.

  python tools/build_banners.py [--preview DIR]     (--preview also saves every banner as DIR/<game>_trackN.png)
  python tools/build_banners.py --arcade             (only the arcade tracks' pictures, below)

--arcade writes the pictures the game's Rush 2049 arcade track mod ships (assets/arcade2049/, src/arcade2049,
docs/arcade2049.md):
  - <id>_logo.png for the arcade versions of the six race tracks: each the N64 track's own banner with " A" after it.
    A name drawn with the font is drawn again as "<NAME> A"; a picture banner (tools/rush2049/banners/trackN_*.png) is
    kept, narrowed by the columns the "A" of "<NAME> A" takes (as many columns dropped evenly), with that "A" put
    after it.
  - alcatraz_logo.png: the plain name in Rush 2049's lettering (banner_font.render_fit: a wider canvas, not smaller
    letters).
  - <id>_preview.png: each tools/arcade2049/previews/<id>.png (in-game screenshots) cut round (round_preview).

Writes include/track1_banners.h and include/track2049_banners.h.
"""

import os
import re
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import banner_font  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 128, 32

ARENAS_2049 = [f"Stunt {n}" for n in range(1, 5)] + ["Obstacle"] + [f"Battle {n}" for n in range(1, 9)]

# (source folder, header, C++ namespace, track count, header with track_names, banner font style, names of the banners
# after the tracks').
GAMES = [
    ("tools/rush1/banners", "include/track1_banners.h", "rush2::track1::banners", 7, "include/track1.h", "sfrush", []),
    ("tools/rush2049/banners", "include/track2049_banners.h", "rush2::track2049::banners", 6, "include/track2049.h",
     "rush2049", ARENAS_2049),
]


def track_names(header):
    text = open(os.path.join(ROOT, header)).read()
    m = re.search(r"track_names\[track_count\]\s*=\s*\{([^}]*)\}", text)
    return re.findall(r'"([^"]*)"', m.group(1))


def convert(im):
    """(CI8 texels, RGBA5551 palette, exact RGBA pixels as 0xRRGGBBAA), all top-down."""
    im = im.convert("RGBA")
    if im.size != (W, H):
        raise SystemExit(f"banner is {im.size}, expected {W}x{H}")
    px = list(im.getdata())
    exact = [(r << 24) | (g << 16) | (b << 8) | 255 if a >= 128 else 0 for (r, g, b, a) in px]
    opaque = [a >= 128 for (_, _, _, a) in px]
    rgb = Image.new("RGB", (W, H))
    rgb.putdata([((r >> 3) << 3, (g >> 3) << 3, (b >> 3) << 3) for (r, g, b, _) in px])
    q = rgb.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[: 255 * 3]
    idx = list(q.getdata())
    texels = [i + 1 if o else 0 for i, o in zip(idx, opaque)]
    palette = [0] * 256
    for i in range(len(pal) // 3):
        r, g, b = pal[i * 3 : i * 3 + 3]
        palette[i + 1] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1
    return texels, palette, exact


def build(src, header, namespace, tracks, names_header, style, extra, preview):
    src_dir = os.path.join(ROOT, src)
    images = {}
    for name in sorted(os.listdir(src_dir)):
        m = re.fullmatch(r"track(\d)_\w+\.png", name)
        if m and 1 <= int(m.group(1)) <= tracks:
            images[int(m.group(1))] = Image.open(os.path.join(src_dir, name))
    names = track_names(names_header) + extra
    tracks += len(extra)
    for t in range(1, tracks + 1):
        if t not in images:
            images[t] = banner_font.render(names[t - 1], style, (W, H))
    banners = {}
    for t, im in sorted(images.items()):
        banners[t] = convert(im)
        if preview:
            os.makedirs(preview, exist_ok=True)
            im.convert("RGBA").save(os.path.join(preview, f"{style}_track{t}.png"))

    out = [
        f"// Generated by tools/build_banners.py from {src}/*.png. Do not edit.",
        "// Track select banners: 128x32 CI8 texels (rows top-down) and RGBA5551 palettes, entry 0 transparent, and the",
        "// exact images (0xRRGGBBAA, rows top-down) the renderer draws in their place.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        f"namespace {namespace} {{",
    ]
    for t, (texels, palette, exact) in sorted(banners.items()):
        out.append(f"    inline constexpr uint8_t track{t}_texels[{W * H}] = {{")
        for k in range(0, len(texels), 32):
            out.append("        " + ", ".join(str(v) for v in texels[k : k + 32]) + ",")
        out.append("    };")
        out.append(f"    inline constexpr uint16_t track{t}_palette[256] = {{")
        for k in range(0, 256, 16):
            out.append("        " + ", ".join(f"0x{v:04X}" for v in palette[k : k + 16]) + ",")
        out.append("    };")
        out.append(f"    inline constexpr uint32_t track{t}_rgba[{W * H}] = {{")
        for k in range(0, len(exact), 16):
            out.append("        " + ", ".join(f"0x{v:08X}" for v in exact[k : k + 16]) + ",")
        out.append("    };")
    out.append("")
    out.append(f"    // Per track (0-{tracks - 1}).")
    for kind, ctype in (("texels", "uint8_t"), ("palette", "uint16_t"), ("rgba", "uint32_t")):
        names_list = ", ".join(f"track{t + 1}_{kind}" for t in range(tracks))
        plural = kind + ("s" if kind == "palette" else "")
        out.append(f"    inline constexpr const {ctype}* {plural}[{tracks}] = {{ {names_list} }};")
    out.append("}")
    path = os.path.join(ROOT, header)
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"Wrote {header} ({len(banners)} banners)")


ARCADE_IDS = ["marina_arcade", "haight_arcade", "civic_arcade", "metro_arcade", "mission_arcade", "presidio_arcade"]


def arcade_logos():
    src_dir = os.path.join(ROOT, "tools", "rush2049", "banners")
    names = track_names("include/track2049.h")
    for t, track_id in enumerate(ARCADE_IDS, 1):
        name = names[t - 1]
        text = banner_font.render(name + " A", "2049", (W, H))
        art = [n for n in os.listdir(src_dir) if re.fullmatch(rf"track{t}_\w+\.png", n)]
        if art:
            # The "A": the columns of "<NAME> A" after the last fully clear column before its right end.
            px = text.load()
            right = text.getbbox()[2]
            a0 = right - 1
            while a0 > 0 and any(px[a0 - 1, y][3] for y in range(H)):
                a0 -= 1
            a_w = right - a0
            gap = 2
            pic = Image.open(os.path.join(src_dir, art[0])).convert("RGBA")
            keep_w = W - a_w - gap - 1
            # Drop columns evenly (no resampling: the picture is pixel art).
            cols = [round(i * (W - 1) / (keep_w - 1)) for i in range(keep_w)]
            im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            for x, c in enumerate(cols):
                im.paste(pic.crop((c, 0, c + 1, H)), (x, 0))
            im.alpha_composite(text.crop((a0, 0, right, H)), (keep_w + gap, 0))
        else:
            im = text
        out = os.path.join(ROOT, "assets", "arcade2049", track_id + "_logo.png")
        im.save(out)
        print("Wrote", os.path.relpath(out, ROOT))
    out = os.path.join(ROOT, "assets", "arcade2049", "alcatraz_logo.png")
    banner_font.render_fit("Alcatraz", "rush2049").convert("RGBA").save(out)
    print("Wrote", os.path.relpath(out, ROOT))
    previews = os.path.join(ROOT, "tools", "arcade2049", "previews")
    for name in sorted(os.listdir(previews)):
        if name.endswith(".png"):
            out = os.path.join(ROOT, "assets", "arcade2049", name[:-4] + "_preview.png")
            round_preview(os.path.join(previews, name)).save(out)
            print("Wrote", os.path.relpath(out, ROOT))


def round_preview(path):
    """A track select picture cut like Rush 2049's own (its TPIC thumbnails): the middle square of the image at
    128 x 128, inside a disc of radius 62 centered on (63.5, 64.5) with hard edges, clear outside. That disc matches
    every 2049 TPIC's alpha exactly."""
    im = Image.open(path).convert("RGBA")
    s = min(im.size)
    x0, y0 = (im.width - s) // 2, (im.height - s) // 2
    im = im.crop((x0, y0, x0 + s, y0 + s)).resize((128, 128), Image.LANCZOS)
    alpha = Image.new("L", (128, 128))
    alpha.putdata([255 if (x - 63.5) ** 2 + (y - 64.5) ** 2 <= 62 ** 2 else 0 for y in range(128) for x in range(128)])
    im.putalpha(alpha)
    return im


if __name__ == "__main__":
    if "--arcade" in sys.argv:
        arcade_logos()
        sys.exit(0)
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None
    for game in GAMES:
        build(*game, preview)

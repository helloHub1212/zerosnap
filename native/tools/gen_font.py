#!/usr/bin/env python3
"""Generate src/text_data.{h,c} for the native camera UI.

The target is a single-core ARMv6 with no font engine: every label is
pre-rendered to an 8-bit alpha mask on the build host.  This script needs
Pillow and is never shipped to the device.

    /tmp/opencode/venv/bin/python tools/gen_font.py

Fonts
-----
* assets/DroidSansFallbackFull.ttf -- CJK + CJK punctuation.
* assets/Piboto-Regular.ttf        -- Latin/ASCII (the CJK fallback font has
  no Basic Latin glyphs, so ASCII comes from the Pi's UI font).

Every glyph is drawn with the anchor "ls" (left/baseline) on a shared line
box, so CJK and Latin baselines line up and text_draw() can place a sprite
with a single (x + bx, y + by) offset.

Output is deterministic: same Pillow/font versions produce byte-identical
files.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - build-host diagnostic
    sys.exit("Pillow is required: /tmp/opencode/venv/bin/python tools/gen_font.py")

ROOT = Path(__file__).resolve().parent.parent
SIZES = (12, 16)
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
#: Padding around the drawn text so negative bearings are never clipped.
PAD = 6


def load_strings(path: Path) -> list[str]:
    strings: list[str] = []
    seen: set[str] = set()
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line in seen:
            continue
        seen.add(line)
        strings.append(line)
    return strings


def font_for(ch: str, cjk: ImageFont.FreeTypeFont, latin: ImageFont.FreeTypeFont):
    return latin if ASCII_FIRST <= ord(ch) <= ASCII_LAST else cjk


def render(
    text: str,
    cjk: ImageFont.FreeTypeFont,
    latin: ImageFont.FreeTypeFont,
    line_ascent: int,
    line_height: int,
) -> dict:
    """Render one string as a cropped 8-bit alpha mask plus metrics."""
    advances: list[tuple[str, object, int]] = []
    total = 0
    for ch in text:
        f = font_for(ch, cjk, latin)
        a = int(round(f.getlength(ch)))
        if a < 0:
            a = 0
        advances.append((ch, f, a))
        total += a

    width = max(1, total + 2 * PAD)
    height = max(1, line_height + 2 * PAD)
    canvas = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(canvas)
    baseline = PAD + line_ascent
    pen = PAD
    for ch, f, a in advances:
        draw.text((pen, baseline), ch, font=f, fill=255, anchor="ls")
        pen += a

    bbox = canvas.getbbox()
    if bbox is None:  # space / no ink
        return {"w": 0, "h": 0, "advance": total, "bx": 0, "by": 0, "mask": b""}

    x0, y0, x1, y1 = bbox
    return {
        "w": x1 - x0,
        "h": y1 - y0,
        "advance": total,
        "bx": x0 - PAD,
        "by": y0 - PAD,
        "mask": canvas.crop(bbox).tobytes(),
    }


def c_string(text: str) -> str:
    """Emit a C string literal; non-ASCII as octal to keep the file ASCII."""
    out: list[str] = []
    for b in text.encode("utf-8"):
        if b == 0x22:
            out.append('\\"')
        elif b == 0x5C:
            out.append("\\\\")
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


class MaskPool:
    def __init__(self) -> None:
        self.names: dict[bytes, str] = {}
        self.order: list[tuple[str, bytes]] = []
        self.bytes = 0

    def add(self, data: bytes) -> str:
        if not data:
            return "kMaskEmpty"
        name = self.names.get(data)
        if name is None:
            name = "kMask_%d" % len(self.order)
            self.names[data] = name
            self.order.append((name, data))
            self.bytes += len(data)
        return name

    def emit(self, out: list[str]) -> None:
        out.append("static const unsigned char kMaskEmpty[1] = { 0 };")
        out.append("")
        for name, data in self.order:
            out.append("static const unsigned char %s[%d] = {" % (name, len(data)))
            for i in range(0, len(data), 12):
                chunk = ", ".join("0x%02x" % b for b in data[i : i + 12])
                tail = "," if i + 12 < len(data) else ""
                out.append("    %s%s" % (chunk, tail))
            out.append("};")
        out.append("")


def build(strings: list[str], cjk_path: Path, latin_path: Path):
    fonts: dict[int, tuple[ImageFont.FreeTypeFont, ImageFont.FreeTypeFont]] = {}
    metrics: dict[int, tuple[int, int]] = {}
    for px in SIZES:
        cjk = ImageFont.truetype(str(cjk_path), px)
        latin = ImageFont.truetype(str(latin_path), px)
        fonts[px] = (cjk, latin)
        ca, cd = cjk.getmetrics()
        la, ld = latin.getmetrics()
        metrics[px] = (max(ca, la), max(cd, ld))

    pool = MaskPool()
    sprites: list[dict] = []
    for text in strings:
        for px in SIZES:
            cjk, latin = fonts[px]
            ascent, descent = metrics[px]
            glyph = render(text, cjk, latin, ascent, ascent + descent)
            glyph.update(text=text, px=px, maskname=pool.add(glyph["mask"]))
            sprites.append(glyph)
    sprites.sort(key=lambda g: (g["px"], g["text"].encode("utf-8")))

    ascii_sprites: dict[int, list[dict]] = {}
    for px in SIZES:
        cjk, latin = fonts[px]
        ascent, descent = metrics[px]
        row = []
        for cp in range(ASCII_FIRST, ASCII_LAST + 1):
            glyph = render(chr(cp), cjk, latin, ascent, ascent + descent)
            glyph.update(text=chr(cp), px=px, maskname=pool.add(glyph["mask"]))
            row.append(glyph)
        ascii_sprites[px] = row

    return sprites, ascii_sprites, metrics, pool


def gen_header(line_heights: dict[int, int]) -> str:
    return """/* Generated by tools/gen_font.py -- do not edit. */
#ifndef TEXT_DATA_H
#define TEXT_DATA_H

#include <stdint.h>

#include "text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *utf8;
    int px;
    int w, h, advance, bx, by;
    const unsigned char *mask;
} TextSpriteData;

/* Pre-rendered string from tools/strings.txt, or NULL.  Sizes other than
 * 12/16 select the nearest of the two. */
const TextSpriteData *text_data_find(const char *utf8, int px);

/* ASCII glyph 0x20..0x7E at 12/16 px, or NULL outside that range. */
const TextSpriteData *text_data_ascii(int codepoint, int px);

/* Has the same lifetime as the static table (no copying). */
const TextSprite *text_data_sprite(const char *utf8, int px);

/* Line-box height in pixels; 12 px -> %d, 16 px -> %d. */
int text_data_line_height(int px);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_DATA_H */
""" % (line_heights[12], line_heights[16])


def gen_source(
    sprites: list[dict],
    ascii_sprites: dict[int, list[dict]],
    line_heights: dict[int, int],
    pool: MaskPool,
) -> str:
    out: list[str] = []
    out.append("/* Generated by tools/gen_font.py -- do not edit. */")
    out.append("#include <string.h>")
    out.append("")
    out.append('#include "text_data.h"')
    out.append("")

    pool.emit(out)

    out.append("static const TextSpriteData kSpr[] = {")
    for g in sprites:
        out.append(
            "    {%s, %d, %d, %d, %d, %d, %d, %s},"
            % (
                c_string(g["text"]),
                g["px"],
                g["w"],
                g["h"],
                g["advance"],
                g["bx"],
                g["by"],
                g["maskname"],
            )
        )
    out.append("};")
    out.append("")

    out.append("/* Plain TextSprite view of kSpr, for text.h's text_lookup(). */")
    out.append("static const TextSprite kSprView[] = {")
    for g in sprites:
        out.append(
            "    {%d, %d, %d, %d, %d, %s},"
            % (g["w"], g["h"], g["advance"], g["bx"], g["by"], g["maskname"])
        )
    out.append("};")
    out.append("")

    out.append("static const TextSpriteData kAscii[2][%d] = {" % (ASCII_LAST - ASCII_FIRST + 1))
    for idx, px in enumerate(SIZES):
        out.append("    { /* %d px */" % px)
        for cp, g in enumerate(ascii_sprites[px], ASCII_FIRST):
            out.append(
                "        {%s, %d, %d, %d, %d, %d, %d, %s},"
                % (
                    c_string(chr(cp)),
                    px,
                    g["w"],
                    g["h"],
                    g["advance"],
                    g["bx"],
                    g["by"],
                    g["maskname"],
                )
            )
        out.append("    },")
    out.append("};")
    out.append("")

    out.append("static int size_index(int px)")
    out.append("{")
    out.append("    return px >= 14 ? 1 : 0;")
    out.append("}")
    out.append("")

    out.append("const TextSpriteData *text_data_find(const char *utf8, int px)")
    out.append("{")
    out.append("    size_t i;")
    out.append("    int want;")
    out.append("")
    out.append("    if (utf8 == NULL)")
    out.append("        return NULL;")
    out.append("    want = size_index(px) ? 16 : 12;")
    out.append("    for (i = 0; i < sizeof(kSpr) / sizeof(kSpr[0]); i++) {")
    out.append("        if (kSpr[i].px == want && strcmp(kSpr[i].utf8, utf8) == 0)")
    out.append("            return &kSpr[i];")
    out.append("    }")
    out.append("    return NULL;")
    out.append("}")
    out.append("")

    out.append("const TextSprite *text_data_sprite(const char *utf8, int px)")
    out.append("{")
    out.append("    const TextSpriteData *d = text_data_find(utf8, px);")
    out.append("")
    out.append("    if (d == NULL)")
    out.append("        return NULL;")
    out.append("    return &kSprView[d - kSpr];")
    out.append("}")
    out.append("")

    out.append("const TextSpriteData *text_data_ascii(int codepoint, int px)")
    out.append("{")
    out.append("    if (codepoint < 0x%02X || codepoint > 0x%02X)" % (ASCII_FIRST, ASCII_LAST))
    out.append("        return NULL;")
    out.append("    return &kAscii[size_index(px)][codepoint - 0x%02X];" % ASCII_FIRST)
    out.append("}")
    out.append("")

    out.append("int text_data_line_height(int px)")
    out.append("{")
    out.append("    return size_index(px) ? %d : %d;" % (line_heights[16], line_heights[12]))
    out.append("}")
    out.append("")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--strings", type=Path, default=ROOT / "tools" / "strings.txt")
    ap.add_argument("--cjk-font", type=Path, default=ROOT / "assets" / "DroidSansFallbackFull.ttf",
                    help="CJK TTF (default: assets/DroidSansFallbackFull.ttf)")
    ap.add_argument("--latin-font", type=Path, default=ROOT / "assets" / "Piboto-Regular.ttf",
                    help="Latin TTF (default: assets/Piboto-Regular.ttf)")
    ap.add_argument("--header", type=Path, default=ROOT / "src" / "text_data.h")
    ap.add_argument("--source", type=Path, default=ROOT / "src" / "text_data.c")
    args = ap.parse_args(argv)

    for p in (args.strings, args.cjk_font, args.latin_font):
        if not p.is_file():
            ap.error("missing input: %s" % p)

    strings = load_strings(args.strings)
    sprites, ascii_sprites, metrics, pool = build(strings, args.cjk_font, args.latin_font)

    line_heights = {px: metrics[px][0] + metrics[px][1] for px in SIZES}

    header = gen_header(line_heights)
    source = gen_source(sprites, ascii_sprites, line_heights, pool)

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.source.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(header, encoding="ascii")
    args.source.write_text(source, encoding="ascii", newline="\n")

    print("gen_font: %d strings x %d sizes = %d sprites" % (len(strings), len(SIZES), len(sprites)))
    print(
        "gen_font: %d ASCII glyphs (%d px + %d px), %d unique masks, %d mask bytes"
        % (
            (ASCII_LAST - ASCII_FIRST + 1) * len(SIZES),
            ASCII_LAST - ASCII_FIRST + 1,
            ASCII_LAST - ASCII_FIRST + 1,
            len(pool.order),
            pool.bytes,
        )
    )
    print(
        "gen_font: line heights %s, %s = %d B, %s = %d B"
        % (
            line_heights,
            args.header,
            args.header.stat().st_size,
            args.source,
            args.source.stat().st_size,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

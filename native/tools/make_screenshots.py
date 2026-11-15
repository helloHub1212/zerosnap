#!/usr/bin/env python3
"""Render the README screenshots from the real native UI.

This is a build-host tool, never shipped to the Pi.  It needs Pillow, the same
dependency as tools/gen_font.py.  The scenes are synthetic landscapes: no
third-party photographs are redistributed with the project.

    python3 tools/make_screenshots.py

The script builds a throwaway media directory with twelve generated photos and
their 320x240 RGB565 previews, then asks zerosnap-ui for a one-frame PPM of
each view.  The PPM is doubled with nearest-neighbour scaling so the 320x240
pixel grid stays crisp in the README.
"""
from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:  # pragma: no cover - build-host diagnostic
    sys.exit("make_screenshots: Pillow is required (pip install Pillow)")

W, H = 320, 240
PHOTO_COUNT = 12


# --------------------------------------------------------------- synthetic art

PALETTES = [
    # golden hour, teal shadows
    dict(top=(24, 33, 62), mid=(96, 108, 148), low=(255, 173, 105),
         sun=(255, 232, 178), glow=1.0, haze=(236, 170, 130),
         ridges=[(74, 84, 112), (48, 60, 84), (28, 38, 54)],
         water=(34, 42, 60), fg=(14, 20, 28)),
    # blue hour
    dict(top=(14, 22, 48), mid=(48, 66, 112), low=(186, 142, 160),
         sun=(240, 200, 190), glow=0.55, haze=(160, 140, 170),
         ridges=[(56, 66, 96), (36, 46, 72), (20, 28, 44)],
         water=(24, 32, 50), fg=(10, 14, 22)),
    # dawn
    dict(top=(54, 62, 110), mid=(172, 136, 172), low=(255, 168, 140),
         sun=(255, 224, 196), glow=0.9, haze=(236, 160, 150),
         ridges=[(92, 92, 126), (62, 64, 96), (36, 40, 64)],
         water=(42, 46, 68), fg=(18, 20, 32)),
    # alpine day
    dict(top=(28, 82, 138), mid=(108, 162, 202), low=(214, 228, 226),
         sun=(255, 250, 220), glow=0.7, haze=(200, 218, 226),
         ridges=[(92, 120, 146), (60, 88, 114), (36, 58, 80)],
         water=(40, 72, 92), fg=(18, 34, 40)),
    # emerald
    dict(top=(16, 44, 60), mid=(62, 112, 110), low=(210, 218, 170),
         sun=(255, 245, 196), glow=0.8, haze=(190, 204, 168),
         ridges=[(60, 92, 84), (40, 68, 64), (22, 42, 42)],
         water=(28, 56, 54), fg=(12, 26, 26)),
    # storm
    dict(top=(40, 46, 58), mid=(92, 98, 110), low=(196, 186, 176),
         sun=(240, 230, 210), glow=0.35, haze=(176, 172, 168),
         ridges=[(78, 82, 94), (52, 56, 68), (30, 34, 44)],
         water=(38, 42, 52), fg=(16, 18, 24)),
]


def clamp(value: float) -> int:
    return 0 if value < 0 else (255 if value > 255 else int(value))


def mix(a, b, t):
    return tuple(clamp(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render_scene(index: int) -> Image.Image:
    """A deterministic, poster-like landscape, 320x240 RGB."""
    p = PALETTES[index % len(PALETTES)]
    variant = index // len(PALETTES)
    horizon = int(H * (0.60 + 0.03 * (variant % 2)))
    sun_x = int(W * (0.34 if variant == 0 else 0.66))
    sun_y = horizon - 16
    px = bytearray(W * H * 3)

    # Sky, with a warm glow around the hidden sun at the horizon.
    for y in range(horizon):
        t = y / max(1, horizon - 1)
        if t < 0.62:
            c = mix(p["top"], p["mid"], t / 0.62)
        else:
            c = mix(p["mid"], p["low"], (t - 0.62) / 0.38)
        for x in range(W):
            d = math.hypot((x - sun_x) * 0.9, (y - sun_y) * 1.25)
            glow = math.exp(-((d / 78.0) ** 2)) * p["glow"]
            if d < 11:
                glow += 1.2
            r = clamp(c[0] + p["sun"][0] * glow * 0.85)
            g = clamp(c[1] + p["sun"][1] * glow * 0.80)
            b = clamp(c[2] + p["sun"][2] * glow * 0.62)
            o = (y * W + x) * 3
            px[o], px[o + 1], px[o + 2] = r, g, b

    # Soft clouds, brighter where the sun is behind them.
    cloud = Image.new("L", (W, H), 0)
    cd = ImageDraw.Draw(cloud)
    for i, (cx, cy, cw, ch) in enumerate([
        (sun_x - 70, horizon - 92, 95, 12),
        (sun_x + 28, horizon - 74, 120, 10),
        (sun_x - 10, horizon - 116, 70, 8),
        (sun_x - 120, horizon - 60, 64, 7),
    ]):
        cd.ellipse([cx - cw // 2, cy - ch // 2, cx + cw // 2, cy + ch // 2],
                   fill=70 if i % 2 else 100)
    cloud = cloud.filter(ImageFilter.GaussianBlur(7))
    for y in range(horizon):
        for x in range(W):
            a = cloud.getpixel((x, y)) / 255.0 * (0.30 + 0.25 * (1 - y / horizon))
            if a <= 0.01:
                continue
            o = (y * W + x) * 3
            tint = p["sun"] if x > sun_x - 40 else p["low"]
            px[o] = clamp(px[o] * (1 - a) + tint[0] * a)
            px[o + 1] = clamp(px[o + 1] * (1 - a) + tint[1] * a)
            px[o + 2] = clamp(px[o + 2] * (1 - a) + tint[2] * a)

    im = Image.frombytes("RGB", (W, H), bytes(px))
    draw = ImageDraw.Draw(im)

    # Three ridges, far to near: distant layers stay pale behind the shore.
    bases = [horizon - 6, horizon - 26, horizon - 52]
    for layer in (2, 1, 0):
        base = bases[layer]
        amp1 = 18 + layer * 5
        amp2 = 7 + layer * 2
        f1 = 0.017 + layer * 0.004
        f2 = 0.041 + variant * 0.008 + layer * 0.006
        ph1 = 0.8 + layer * 1.7 + variant * 0.9
        ph2 = 2.1 + layer * 0.6 + index * 0.37
        pts = [(0, H)]
        for x in range(W + 1):
            y = base - amp1 * math.sin(f1 * x + ph1) \
                - amp2 * math.sin(f2 * x + ph2)
            pts.append((x, int(y)))
        pts.append((W, H))
        draw.polygon(pts, fill=p["ridges"][2 - layer])

    # Haze hugging the horizon, before the water is painted over it.
    haze = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    hd = ImageDraw.Draw(haze)
    for y in range(max(0, horizon - 60), horizon):
        t = (y - (horizon - 60)) / 60.0
        hd.line([(0, y), (W, y)], fill=(*p["haze"], int(70 * t * t)))
    im = Image.alpha_composite(im.convert("RGBA"), haze).convert("RGB")

    # Water: a vertical gradient plus a soft, rippled sun column.
    px = bytearray(W * H * 3)
    for y in range(horizon, H):
        t = (y - horizon) / max(1, H - horizon - 1)
        c = mix(mix(p["low"], p["water"], 0.55), (10, 14, 20), t * 0.75)
        for x in range(W):
            ripple = 0.94 + 0.06 * math.sin(
                (y - horizon) * 0.55 + x * 0.16 + index * 1.7)
            dx = abs(x - sun_x)
            refl = math.exp(-((dx / 46.0) ** 2)) * (1 - t) * p["glow"] * ripple
            r = clamp(c[0] + p["sun"][0] * refl * 0.75)
            g = clamp(c[1] + p["sun"][1] * refl * 0.62)
            b = clamp(c[2] + p["sun"][2] * refl * 0.38)
            o = (y * W + x) * 3
            px[o], px[o + 1], px[o + 2] = r, g, b
    water = Image.frombytes("RGB", (W, H), bytes(px))
    im.paste(water.crop((0, horizon, W, H)), (0, horizon))

    # Glitter on the water, fading towards the viewer.
    draw = ImageDraw.Draw(im, "RGBA")
    for i in range(26):
        yy = horizon + 3 + (i * 37) % (H - horizon - 8)
        t = (yy - horizon) / (H - horizon)
        half = int(2 + 24 * (1 - t) * (0.4 + 0.6 * ((i * 7) % 5) / 4))
        xx = sun_x + int(6 * math.sin(i * 2.1)) - half
        draw.line([(xx, yy), (xx + 2 * half, yy)],
                  fill=(*p["sun"], int(44 * (1 - t) * p["glow"])))
        if half > 4:
            draw.line([(xx + 2, yy + 1), (xx + 2 * half - 2, yy + 1)],
                      fill=(*p["sun"], int(20 * (1 - t))))

    # Foreground shore and a few reeds.
    draw = ImageDraw.Draw(im)
    pts = [(0, H)]
    for x in range(W + 1):
        y = H - 12 - 7 * math.sin(x * 0.021 + index * 0.8) \
            - 3 * math.sin(x * 0.09 + index)
        pts.append((x, int(y)))
    pts.append((W, H))
    draw.polygon(pts, fill=p["fg"])

    draw = ImageDraw.Draw(im)
    for i in range(11):
        bx = 8 + i * 7 + (index * 3) % 5
        by = H - 14 - int(5 * math.sin(i * 1.7 + index))
        ln = 5 + (i * 5) % 9
        draw.line([(bx, by), (bx - 2 - i % 3, by - ln)], fill=p["fg"], width=1)
        draw.line([(bx + 2, by), (bx + 4 + i % 3, by - ln + 2)],
                  fill=p["fg"], width=1)

    # Vignette and a little fixed-pattern grain so flat areas do not band.
    px = bytearray(im.tobytes())
    for y in range(H):
        ny = (y - H / 2) / (H / 2)
        for x in range(W):
            nx = (x - W / 2) / (W / 2)
            d = math.hypot(nx, ny)
            f = 1.0 - 0.20 * max(0.0, d - 0.45) ** 1.4
            n = ((x * 374761393 + y * 668265263 + index * 69069)
                 ^ 0x5BF03635) & 255
            grain = (n % 7 - 3) * 0.6
            o = (y * W + x) * 3
            for k in range(3):
                px[o + k] = clamp(px[o + k] * f + grain)
    return Image.frombytes("RGB", (W, H), bytes(px))


# ------------------------------------------------------------------- encoding

def write_ppm(path: Path, im: Image.Image) -> None:
    path.write_bytes(b"P6\n%d %d\n255\n" % (im.width, im.height)
                     + im.convert("RGB").tobytes())


def write_rgb565(path: Path, im: Image.Image) -> None:
    """Little-endian RGB565, the format media.c blits directly."""
    out = bytearray()
    data = im.convert("RGB").tobytes()
    for i in range(0, len(data), 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += value.to_bytes(2, "little")
    path.write_bytes(out)


def build_media(root: Path) -> dict[str, Path]:
    """Fake FAT32 camera folder: twelve names plus their .565 previews."""
    media = root / "media"
    previews = media / ".preview"
    previews.mkdir(parents=True)
    start = 1_700_000_000
    scenes: dict[str, Path] = {}

    for i in range(PHOTO_COUNT):
        name = f"IMG_{i + 1:04d}"
        # The UI only stats the media file; the pixels live in .preview.
        photo = media / f"{name}.jpg"
        photo.write_bytes(b"")
        os.utime(photo, (start + (PHOTO_COUNT - i), start + (PHOTO_COUNT - i)))

        scene = render_scene(i)
        write_rgb565(previews / f"{name}.565", scene)
        if i == 0:
            ppm = root / "scene0.ppm"
            write_ppm(ppm, scene)
            scenes["preview"] = ppm
    return scenes


# -------------------------------------------------------------------- capture

def capture(ui: Path, work: Path, name: str, view: str, media: Path,
            preview: Path | None = None, calibrate: bool = False) -> Path:
    ppm = work / f"{name}.ppm"
    cmd = [str(ui), "--fb", str(work / "fb.img"), "--screenshot", str(ppm),
           "--view", view]
    if preview is not None:
        cmd += ["--preview", str(preview)]
    if calibrate:
        cmd += ["--calibrate"]

    env = dict(os.environ)
    env["CAMERA_MEDIA_DIR"] = str(media)
    subprocess.run(cmd, check=True, env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return ppm


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    repo = root.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path,
                        default=repo / "docs" / "screenshots",
                        help="output directory (default: docs/screenshots)")
    parser.add_argument("--scale", type=int, default=2,
                        help="integer upscale for the PNGs (default: 2)")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary PPMs and media folder")
    parser.add_argument("--ui", type=Path, default=root / "zerosnap-ui",
                        help="path to a built zerosnap-ui binary")
    args = parser.parse_args()

    if args.scale < 1:
        parser.error("--scale must be at least 1")
    if not args.ui.exists():
        print(f"make_screenshots: {args.ui} is missing; run make first",
              file=sys.stderr)
        return 1

    args.out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="zerosnap-shots-"))
    try:
        scenes = build_media(work)
        media = work / "media"
        shots = [
            ("viewfinder", "viewfinder", scenes["preview"], False),
            ("gallery", "gallery", None, False),
            ("photo", "photo", None, False),
            ("settings", "settings", None, False),
            ("calibration", "viewfinder", None, True),
        ]
        for name, view, preview, calibrate in shots:
            ppm = capture(args.ui, work, name, view, media, preview, calibrate)
            im = Image.open(ppm)
            if args.scale != 1:
                im = im.resize((im.width * args.scale, im.height * args.scale),
                               Image.Resampling.NEAREST)
            out = args.out / f"{name}.png"
            im.save(out, optimize=True)
            print(f"wrote {out}")
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

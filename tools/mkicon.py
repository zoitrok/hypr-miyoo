#!/usr/bin/env python3
"""Generate the OnionOS launcher icons.

Kept as a script rather than committed pixels so the icon is reviewable and
tweakable: it is the same vaporwave scene the app itself draws -- gradient sky,
scanline sun, perspective grid -- rendered small.

  python3 tools/mkicon.py
"""

import math
import pathlib

from PIL import Image, ImageDraw

SIZE = 120  # OnionOS scales icons down; rendering large keeps edges clean.

# The palette the renderer will use, kept in one place here so the icon and the
# app cannot drift apart visually.
SKY_TOP = (26, 8, 56)
SKY_BOT = (94, 22, 108)
SUN_TOP = (255, 214, 92)
SUN_BOT = (255, 45, 122)
GRID = (0, 240, 232)
HORIZON = 0.60  # fraction of height


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def render(selected: bool) -> Image.Image:
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    horizon = int(SIZE * HORIZON)

    # Sky gradient.
    for y in range(horizon):
        d.line([(0, y), (SIZE, y)], fill=lerp(SKY_TOP, SKY_BOT, y / horizon))

    # Ground is darker so the grid reads against it.
    for y in range(horizon, SIZE):
        t = (y - horizon) / max(SIZE - horizon, 1)
        d.line([(0, y), (SIZE, y)], fill=lerp((40, 6, 62), (12, 2, 28), t))

    # Sun: a disc clipped to the sky, with horizontal gaps widening downward --
    # the detail that makes the shape read as 80s rather than as a circle.
    cx, cy, r = SIZE / 2, horizon * 0.98, SIZE * 0.27
    for y in range(int(cy - r), min(int(cy + r) + 1, horizon)):
        dy = y - cy
        half = math.sqrt(max(r * r - dy * dy, 0))
        if half <= 0:
            continue
        band = (y - (cy - r)) / (2 * r)
        if band > 0.45:
            # Scanline gaps: period and thickness both grow toward the horizon.
            period = 3 + int((band - 0.45) * 14)
            if (y - int(cy - r)) % period < max(1, period // 2):
                continue
        d.line([(cx - half, y), (cx + half, y)],
               fill=lerp(SUN_TOP, SUN_BOT, band))

    # Perspective grid: verticals converge on the vanishing point, horizontals
    # accelerate toward the viewer.
    grid = GRID if not selected else (255, 90, 200)
    for i in range(-6, 7):
        x_far = cx + i * 5
        x_near = cx + i * (SIZE / 3.2)
        d.line([(x_far, horizon), (x_near, SIZE)], fill=grid, width=1)

    z = 0.0
    while z < 1.0:
        y = horizon + (SIZE - horizon) * (z * z)
        d.line([(0, y), (SIZE, y)], fill=grid, width=1)
        z += 0.085

    # Horizon glow.
    d.line([(0, horizon), (SIZE, horizon)], fill=(255, 240, 255), width=1)

    # Rounded-corner mask, so the icon does not sit as a hard square.
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, SIZE - 1, SIZE - 1],
                                           radius=int(SIZE * 0.18), fill=255)
    img.putalpha(mask)

    if selected:
        # The selected variant gets a bright border; MainUI shows it on focus.
        ImageDraw.Draw(img).rounded_rectangle(
            [0, 0, SIZE - 1, SIZE - 1], radius=int(SIZE * 0.18),
            outline=(255, 255, 255, 255), width=3)

    return img


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "package" / "App"
    for app in ("Hypr", "HyprProbe"):
        d = out / app
        d.mkdir(parents=True, exist_ok=True)
        render(False).save(d / "icon.png")
        render(True).save(d / "iconsel.png")
        print(f"wrote {d/'icon.png'} and {d/'iconsel.png'}")


if __name__ == "__main__":
    main()

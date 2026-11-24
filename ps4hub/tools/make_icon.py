"""Generates sce_sys/icon0.png (512x512) for PS4Hub.

Navy gradient, a translucent glass card and three cyan slider tracks.
Drawn at 4x and downsampled for clean anti-aliasing. Requires Pillow.

usage: python tools/make_icon.py sce_sys/icon0.png
"""
import sys

from PIL import Image, ImageDraw

SIZE = 512
SS = 4
S = SIZE * SS

TOP = (0x00, 0x11, 0x2C)
BOTTOM = (0x00, 0x2F, 0x6C)
ACCENT = (0x00, 0x70, 0xD1)
BRIGHT = (0x00, 0xA2, 0xFF)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def main(out_path):
    img = Image.new("RGB", (S, S))
    px = ImageDraw.Draw(img)
    for y in range(S):
        px.line([(0, y), (S, y)], fill=lerp(TOP, BOTTOM, y / (S - 1)))

    overlay = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)

    # Glass card.
    m = 72 * SS
    d.rounded_rectangle([m, m, S - m, S - m], radius=56 * SS, fill=(15, 25, 45, 179),
                        outline=(120, 170, 255, 70), width=3 * SS)

    # Three slider tracks with knobs at different positions.
    left, right = 132 * SS, S - 132 * SS
    knobs = [0.30, 0.72, 0.50]
    for i, k in enumerate(knobs):
        y = (182 + i * 74) * SS
        h = 10 * SS
        d.rounded_rectangle([left, y - h // 2, right, y + h // 2], radius=h // 2, fill=(255, 255, 255, 45))
        kx = left + (right - left) * k
        d.rounded_rectangle([left, y - h // 2, kx, y + h // 2], radius=h // 2, fill=ACCENT + (255,))
        r = 24 * SS
        d.ellipse([kx - r - 8 * SS, y - r - 8 * SS, kx + r + 8 * SS, y + r + 8 * SS], fill=BRIGHT + (60,))
        d.ellipse([kx - r, y - r, kx + r, y + r], fill=BRIGHT + (255,), outline=(255, 255, 255, 230), width=4 * SS)

    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save(out_path, "PNG", optimize=True)
    print("wrote", out_path)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "sce_sys/icon0.png")

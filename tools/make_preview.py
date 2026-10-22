#!/usr/bin/env python3
"""Render saturn frames with --dump and assemble an animated GIF (needs Pillow).

    python3 tools/make_preview.py                 # assets/preview.gif, one orbit sped up 5x
    python3 tools/make_preview.py --still 0 out.png
"""
import argparse, re, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

FONTS = ["/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
         "/System/Library/Fonts/Menlo.ttc", "C:/Windows/Fonts/consola.ttf"]
TOKEN = re.compile(r"\x1b\[([0-9;]*)m|(.)")
BG = (10, 11, 16)


def load_font(size):
    for f in FONTS:
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def frame(binary, cols, rows, t, font, cw, ch):
    out = subprocess.run([binary, "--colors", "truecolor", "--dump", f"{cols}x{rows}", str(t)],
                         capture_output=True, text=True, check=True).stdout
    img = Image.new("RGB", (cols * cw, rows * ch), BG)
    d = ImageDraw.Draw(img)
    for y, line in enumerate(out.split("\n")[:rows]):
        x, col = 0, (200, 200, 200)
        for m in TOKEN.finditer(line):
            if m.group(1) is not None:
                p = m.group(1).split(";")
                if len(p) == 5:
                    col = tuple(map(int, p[2:]))
                continue
            if m.group(2) != " ":
                d.text((x * cw, y * ch), m.group(2), font=font, fill=col)
            x += 1
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./saturn")
    ap.add_argument("--cols", type=int, default=110)
    ap.add_argument("--rows", type=int, default=32)
    ap.add_argument("--orbit", type=float, default=60.0, help="seconds of animation to cover")
    ap.add_argument("--speedup", type=float, default=5.0)
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--still", nargs=2, metavar=("SECONDS", "PNG"))
    ap.add_argument("out", nargs="?", default="assets/preview.gif")
    a = ap.parse_args()
    font, cw, ch = load_font(13), 8, 16
    if a.still:
        frame(a.binary, a.cols, a.rows, float(a.still[0]), font, cw, ch).save(a.still[1])
        return
    n = int(a.orbit / a.speedup * a.fps)
    frames = []
    for i in range(n):
        frames.append(frame(a.binary, a.cols, a.rows, i * a.speedup / a.fps, font, cw, ch)
                      .quantize(colors=96, method=Image.Quantize.MEDIANCUT))
        print(f"\rframe {i + 1}/{n}", end="", file=sys.stderr)
    print(file=sys.stderr)
    frames[0].save(a.out, save_all=True, append_images=frames[1:], duration=int(1000 / a.fps),
                   loop=0, optimize=True, disposal=1)


if __name__ == "__main__":
    main()

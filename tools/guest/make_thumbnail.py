#!/usr/bin/env python3
"""A guest course's picture for the course select's GUEST COURSES page
(src/game_menus.cpp), from a screenshot -- an F2 mark's, say.

    python tools/guest/make_thumbnail.py <image> <out folder> [left,top,right,bottom]

writes thumbnail.bin there: 284 by 210 (twice the page's square, 142 by
105), RGBA 5551 big-endian, as the N64 reads it. The image is cut to the
square's shape, from the given box (in the image's own pixels) or else its
middle, the black bars of a window's screenshot cut off first. Like the
pack, it is made on the player's own machine (a screenshot of another
game's place is not this repository's to hold).
"""

import os
import struct
import sys

from PIL import Image

WIDTH, HEIGHT = 284, 210


def content_box(im):
    """The image without the black bars round a window's picture."""
    grey = im.convert('L')
    w, h = grey.size
    rows = [y for y in range(h) if max(grey.crop((0, y, w, y + 1)).getdata()) > 12]
    cols = [x for x in range(w) if max(grey.crop((x, 0, x + 1, h)).getdata()) > 12]
    return (cols[0], rows[0], cols[-1] + 1, rows[-1] + 1) if rows and cols else (0, 0, w, h)


def main():
    im = Image.open(sys.argv[1]).convert('RGB')
    out_dir = sys.argv[2]
    if len(sys.argv) > 3:
        box = tuple(int(v) for v in sys.argv[3].split(','))
    else:
        l, t, r, b = content_box(im)
        # The middle, the square's shape.
        w, h = r - l, b - t
        if w / h > WIDTH / HEIGHT:
            cut = int(h * WIDTH / HEIGHT)
            l, r = l + (w - cut) // 2, l + (w - cut) // 2 + cut
        else:
            cut = int(w * HEIGHT / WIDTH)
            t, b = t + (h - cut) // 2, t + (h - cut) // 2 + cut
        box = (l, t, r, b)
    small = im.crop(box).resize((WIDTH, HEIGHT), Image.LANCZOS)
    data = bytearray()
    raw = small.tobytes()
    for i in range(0, len(raw), 3):
        r, g, b = raw[i], raw[i + 1], raw[i + 2]
        data += struct.pack('>H', ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'thumbnail.bin'), 'wb') as f:
        f.write(data)
    small.save(os.path.join(out_dir, 'thumbnail.png'))
    print(f'thumbnail.bin: {WIDTH} by {HEIGHT}, from {box} of {im.size}')


if __name__ == '__main__':
    main()

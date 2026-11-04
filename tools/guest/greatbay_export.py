#!/usr/bin/env python3
"""Great Bay Coast (Z2_30GYOSON) from the player's Majora's Mask ROM, exported
for a first look (docs/GUEST_COURSES.md, milestone 1): its collision and its
scenery as OBJ models, a top-down picture of both, and a summary.

    python tools/guest/greatbay_export.py <rom.z64> <out folder>

Nothing is written into the repository: the out folder is the player's.
"""

import json
import os
import sys

from mm_rom import Rom
from mm_scene import Scenery, commands, room_lists, scene_and_rooms

# The scene's file in the USA ROM's file table (found by its collision header
# at 0x93F4, the offset the 2Ship2Harkinian archive names it by).
GREAT_BAY_COAST = 1330


def main():
    rom = Rom(sys.argv[1])
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    scene, header, col, rooms = scene_and_rooms(rom, GREAT_BAY_COAST)

    with open(os.path.join(out, 'greatbay_collision.obj'), 'w') as f:
        f.write('# Great Bay Coast collision (Majora\'s Mask units)\n')
        for x, y, z in col.vertices:
            f.write(f'v {x} {y} {z}\n')
        for p in col.polygons:
            a, b, c = p['v']
            f.write(f'f {a + 1} {b + 1} {c + 1}\n')

    scenery = []
    for r in rooms:
        s = Scenery({2: scene, 3: r})
        for opa, xlu in room_lists(r):
            if opa:
                s.walk(opa)
            if xlu:
                s.walk(xlu)
        scenery.extend(s.triangles)
    with open(os.path.join(out, 'greatbay_scenery.obj'), 'w') as f:
        f.write('# Great Bay Coast scenery, vertex colours (Majora\'s Mask units)\n')
        n = 0
        last = None
        for verts, texture in scenery:
            if texture != last:
                f.write(f'g texture_{texture:08X}\n')
                last = texture
            for x, y, z, s_, t, r, g, b, a in verts:
                f.write(f'v {x} {y} {z} {r / 255:.3f} {g / 255:.3f} {b / 255:.3f}\n')
            f.write(f'f {n + 1} {n + 2} {n + 3}\n')
            n += 3

    summary = {
        'collision': {'vertices': len(col.vertices), 'polygons': len(col.polygons), 'min': col.min, 'max': col.max},
        'water_boxes': col.water,
        'rooms': len(rooms),
        'scenery_triangles': len(scenery),
        'scenery_textures': sorted({f'{t:08X}' for _, t in scenery}),
        'header': [(f'{c:02X}', b, f'{w:08X}') for c, b, w in header],
    }
    with open(os.path.join(out, 'greatbay_summary.json'), 'w') as f:
        json.dump(summary, f, indent=1)

    try:
        top_down(out, col, scenery)
    except ImportError:
        pass
    print(json.dumps({k: v for k, v in summary.items() if k != 'header'}, indent=1))


def top_down(out, col, scenery):
    """Collision (coloured by height: blue under the sea, green to white up
    the land; walls red) and the water box, from above, +x right, +z down."""
    from PIL import Image, ImageDraw
    x0, z0 = col.min[0], col.min[2]
    x1, z1 = col.max[0], col.max[2]
    scale = 1200 / max(x1 - x0, z1 - z0)
    img = Image.new('RGB', (int((x1 - x0) * scale) + 20, int((z1 - z0) * scale) + 20), (20, 20, 30))
    d = ImageDraw.Draw(img)
    px = lambda x, z: (10 + (x - x0) * scale, 10 + (z - z0) * scale)
    for w in col.water:
        d.rectangle([px(w['x'], w['z']), px(w['x'] + w['x_length'], w['z'] + w['z_length'])], outline=(60, 120, 255))
    polys = sorted(col.polygons, key=lambda p: max(col.vertices[i][1] for i in p['v']))
    for p in polys:
        vs = [col.vertices[i] for i in p['v']]
        y = sum(v[1] for v in vs) / 3
        ny = p['normal'][1]
        if abs(ny) < 0.3:
            colour = (200, 60, 60)
        elif y < 0:
            k = max(0.0, min(1.0, -y / 445))
            colour = (40, int(90 - 50 * k), int(200 - 100 * k))
        else:
            k = max(0.0, min(1.0, y / 1500))
            colour = (int(60 + 180 * k), int(150 + 90 * k), int(60 + 180 * k))
        d.polygon([px(v[0], v[2]) for v in vs], fill=colour, outline=(0, 0, 0))
    img.save(os.path.join(out, 'greatbay_top_down.png'))


if __name__ == '__main__':
    main()

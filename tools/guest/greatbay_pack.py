#!/usr/bin/env python3
"""Great Bay Coast for Wave Race 64's Free Roam: a pack the game loads into
memory, built from the player's own Majora's Mask (USA) ROM.

    python tools/guest/greatbay_pack.py <mm rom.z64> <out folder>

writes greatbay.pack there (and the export's pictures/models). Nothing of
Majora's Mask is in this repository (docs/GUEST_COURSES.md).

The pack is one block copied to physical kBase (src/guest_course.cpp):

    +0x000  header: magic 'WR64GBAY', version, sizes, the display list's
            address, the collision's offset and count, the start
    +0x100  the scene file, the room file and the scene texture file
            (scene_texture_05, segment 6 in Majora's Mask), as they are
    ...     the scenery's vertices, copied and re-batched
    ...     its display list, in Wave Race's microcode
    ...     the collision: triangles in Wave Race units, 3 x (x, y, z) f32

Majora's Mask's display lists are F3DEX2; Wave Race's microcode is a custom
Fast3D (F3D_OLD in the decompilation): G_VTX 0x04 with v0 * 5, G_TRI1 0xBF
with indices * 5, G_ENDDL 0xB8, G_TEXTURE 0xBB, G_SETOTHERMODE_L/H
0xB9/0xBA (shift and length rather than F3DEX2's 32 - shift - length and
length - 1), geometry mode by 0xB6 (clear) / 0xB7 (set). RDP commands
(0xE4 and up but F3DEX2's own 0xE1-0xE3) are the same in both.
"""

import math
import os

import numpy as np
import struct
import sys

from mm_rom import Rom
import mm_character
import mm_merge
from mm_scene import Scenery, room_lists, scene_and_rooms

GREAT_BAY_COAST = 1330
SCENE_TEXTURE_05 = 1118
BASE = 0x400000                  # physical; src/guest_course.cpp kBase
LIMIT = 0x4C0000                 # up to the collision (src/guest_course.cpp kPlanes)
SCALE = 0.83                     # Wave Race units to a Majora's Mask unit (Link ~50 tall, ~2.5 cm a unit)
BATCH = 24                       # vertices per load
# Where the ride starts, in Majora's Mask units: open water west of the land,
# 1800 from anything solid and well inside the collision (which ends at
# z 8960: past it the game holds the jet ski as if at a wall), facing it.
START = (-4250.0, 2250.0, 0.0)  # x, z, heading (degrees, 0 along +x)


# Its people: characters of Majora's Mask posed by the game as it goes
# (tools/guest/mm_character.py, src/guest_people.cpp). Each model: its
# object (file), skeleton (and whether its limbs are LodLimbs), scale,
# the textures segments 8 and 9 stand for (its eyes, its mouth), and its
# animations, by name, each (kind, header or offset, frames).
#   Zoras: object_zo (file 758): treading water, swimming (its run, the
#   game turning it face down), surfacing.
#   Link: object_link_child (file 654); his animations are the player's,
#   in link_animetion (file 7), each frame 22 joints and the face. (Their
#   lengths are what the data holds: read past, standing and treading water
#   threw him about, the user's second look, 2026-11-03.)
# Items a model holds: a display list in gameplay_keep (file 649), the
# limb it is held in, and its segment 8 (the rupee's colour).
GAMEPLAY_KEEP = 649
LINK_ANIMETION = 7
MODELS = {
    'zora': {'object': 758, 'skeleton': 0xD208, 'lod': False, 'scale': 0.01, 'seg8': 0x50A0, 'seg9': None,
             'anims': [('tread', 'npc', 0x4168, 0), ('swim', 'npc', 0xE400, 0), ('surface', 'npc', 0x3610, 0)], 'items': []},
    'link': {'object': 654, 'skeleton': 0x1E244, 'lod': True, 'scale': 0.01, 'seg8': 0x0, 'seg9': 0x4000,
             'anims': [('swim', 'player', 0xBC6F0, 29), ('tread', 'player', 0x13DD0, 12), ('dive', 'player', 0x11EDF0, 178),
                       ('rise', 'player', 0x181390, 32), ('stand', 'player', 0x87C0, 20), ('show', 'player', 0x11C600, 22)],
             'items': [('rupee', 0x62288, 15, 0x61FC0), ('stick', 0x3248, 15, None)]},
}
# Who is about, where they start (Majora's Mask units: x, z, yaw as a
# binary angle): the Zoras where Great Bay Coast's scene places them
# (its actors 0x224 and 0x260, in both its day setups), and Link.
PEOPLE = [('zora', -1604, 4673, 11915), ('zora', -2930, 4441, 8203), ('zora', -2441, 5334, 14987),
          ('zora', -4916, -87, 0), ('zora', -1244, -2566, 18815), ('link', 1300, -1750, -16384)]
# The open water they may swim in: WATER_CELL squares whose collision is
# below WATER_DEPTH (or none), over the whole map.
WATER_CELL = 100
WATER_DEPTH = -60
# Zora Cape (scene file 1332): its world is Great Bay Coast's moved this
# far (Majora's Mask units; found from the vertices both scenes share).
ZORA_CAPE = 1332
ZORA_CAPE_SHIFT = (0, 0, 6981)
# The coast goes on (Great Bay is the end of a land, not an island): the
# ridge along Great Bay Coast's north-east (its triangles with their middles
# east of x 500 and north of z -3000: 7119 long, its north end 2550 further
# west than its south), drawn again and solid end to end beyond the map's
# north edge, and on from Zora Cape's south-east end, far past where the
# Free Roam current turns the jet ski back (BOUNDS, Wave Race units: x
# from, to, z from, to). (The whole east side, copied first, cut through
# the land by the beach: its copies' mountains hung over the water.)
COAST_X, COAST_Z = 500, -3000
SKIRT_DEPTH = -400
COAST_STEP = (-2550, 0, -7119)
COAST_COPIES = [tuple(COAST_STEP[i] * k for i in range(3)) for k in (1, 2, 3, 4)]
# (Past Zora Cape: from where its mountains end, z 13000 -- the first copy
# began at 19000, its open end in plain sight across the gap, the user's
# second look, 2026-11-03.)
COAST_COPIES += [(3050 + 2550 * k, 0, 28600 + 7119 * k) for k in (-1, 0, 1)]
# The Free Roam current's lines: north and south where the user marked them
# (2026-11-03, F2 at each end of the seafloor).
BOUNDS = (-9500.0, 6500.0, -4737.0, 15049.0)
# Under the void past the seafloor's edge, sand: the seafloor's own rippled
# sand, a flat floor 15 m down (500 Wave Race units), out past anything the
# ride can see, in squares SAND_CELL across (Majora's Mask units).
SAND_Y = -602
SAND_CELL = 6400
SAND_AREA = (-44000, 24000, -44000, 52000)          # x from, to, z from, to
SAND_TEXTURE = ('tex', 20480)                        # scene_texture_05's rippled sand
# Collision under the seafloor's shallowest: the ride never reaches the deep
# floor, and the jet ski was brought up short over it, at its huge
# triangles' edges and corners (the user's marks 1 and 8, 2026-11-03).
DEEP = -150
# Things standing in the water drawn with nothing solid: the fish statues
# and the spikes out west (the user's marks 3 and 6) -- their own triangles
# made solid where the scenery rises through the water with no raised
# collision within SOLID_REACH.
SOLID_REACH = 400
# Rock with no way down on one side, filled in: each edge of its triangles
# that is the scenery's edge and above the water gets a face of the same
# rock down to FILL_DEPTH, solid. Each region: the scene (0 Great Bay
# Coast, 1 Zora Cape), x from, to, z from, to (Majora's Mask units, the
# world's). The user's marks, 2026-11-03 night: Zora Cape's rock cut off
# along its top (mark 3); the spit in the bay's south-west, hanging over
# the water the way to the Pirates' Fortress goes under (mark 5).
FILL_DEPTH = -460
# A region's last: whether its edges with the rock below them count too
# (the cape's rock's open side was its top's edge), or only those with the
# rock above (the channel's mouth past the spit, the ridge's arch over it
# open below, the third night's mark 3, and the mountains beyond it, a
# backdrop too, 950 up at their lowest: their peaks' edges stay as they are).
FILL_REGIONS = [(1, -2600, -200, 11800, 14400, True), (0, -4700, -1000, -5100, -2900, True),
                (0, -1000, 4500, -7500, -2400, False), (0, 2800, 5200, -2600, 2500, False)]
# The ring of six spikes out west (its centre, the pit's radius): inside it
# the floor goes down to 60 m (2000 Wave Race units), as in Majora's Mask
# (the user's mark 4).
PIT = (-9256, 28, 640)
PIT_Y = -2410
PIT_SIDES = 32


def rim(t):
    """The seafloor's rim out west, rising to the surface at the map's edge:
    left out, scenery and collision, the sand below showing instead (the
    user's mark 6)."""
    t = np.asarray(t, float)
    edge = (t[:, 0] <= -10200) | (t[:, 2] <= -6600) | (t[:, 2] >= 12100)
    if np.any(edge & (t[:, 1] > -30)) and t[:, 1].min() < -100 and t[:, 1].max() < 30:
        return True
    # (And the wide shallow plains of floor out west, 5 m and less: the
    # user's mark 2, the third night.)
    n = np.cross(t[1] - t[0], t[2] - t[0])
    area = np.linalg.norm(n) / 2
    return bool(area > 1e6 and abs(n[1]) > 0.95 * 2 * area and t[:, 1].max() > -300 and t.mean(axis=0)[0] < -5000)


def align(n, a=16):
    return (n + a - 1) // a * a


class Pack:
    def __init__(self):
        self.data = bytearray(0x100)

    def add(self, blob, a=16):
        at = align(len(self.data), a)
        self.data += b'\0' * (at - len(self.data))
        self.data += blob
        return at


class Atlas:
    """The textures (and palettes) the lists load, copied out of their files
    into one block: each load command (G_LOADBLOCK, G_LOADTILE, G_LOADTLUT)
    says how much of the image it reads, and only that much is kept. The
    files themselves -- mostly vertices, lists, collision and cutscenes --
    stay out of the pack."""

    def __init__(self, files):
        self.files = files        # key -> bytes
        self.spans = {}           # key -> [(start, end)]
        self.where = None

    def add(self, key, start, length):
        start8 = start & ~7
        end = min(len(self.files[key]), start + max(8, int(length) + 8))
        self.spans.setdefault(key, []).append((start8, (end + 7) & ~7))

    def build(self):
        blob = bytearray()
        self.where = {}
        for key, spans in self.spans.items():
            merged = []
            for a, b in sorted(spans):
                if merged and a <= merged[-1][1]:
                    merged[-1] = (merged[-1][0], max(merged[-1][1], b))
                else:
                    merged.append((a, b))
            for a, b in merged:
                self.where.setdefault(key, []).append((a, b, len(blob)))
                blob += self.files[key][a:b]
                blob += b'\0' * (align(len(blob), 8) - len(blob))
        return bytes(blob)

    def offset(self, key, at):
        for a, b, o in self.where[key]:
            if a <= at < b:
                return o + (at - a)
        raise KeyError((key, hex(at)))


TEXEL_BYTES = {0: 0.5, 1: 1, 2: 2, 3: 4}


def convert(data, names, roots, atlas, end=0xB8000000, shift=(0, 0, 0), keep=None, both_sides=None, extra=None, slot=None):
    """Display lists `roots` (segmented addresses into `data`, segment ->
    bytes) in Wave Race's microcode, their vertices copied (moved by
    `shift`): (vertices blob, display list as (w0, w1) words, triangles out).
    Textures are copied into `atlas` (`names`: segment -> the atlas's key
    for that segment's file); their addresses are ('tex', key, offset)
    until the atlas is built. `keep(positions)` leaves a triangle out when
    false; `both_sides(positions)` draws it from behind too; `extra(positions,
    records)` gives more triangles (vertex records) drawn as it is.

    A character's limb (`slot`, its matrix's): vertices stay in the limb's
    units, and each is loaded under the matrix it was loaded under in
    Majora's Mask (the limb's own, or another limb's from segment 13); the
    matrices are ('mtx', slot) until a character's are placed."""
    pending = []          # the image set and not yet loaded: (key, offset, size, width)

    def moved(record):
        if shift == (0, 0, 0):
            return record
        x, y, z = struct.unpack('>hhh', record[0:6])
        return struct.pack('>hhh', x + shift[0], y + shift[1], z + shift[2]) + record[6:]

    out = []
    vertices = bytearray()
    batch = []           # vertex records in the current load
    tris = []            # (i, j, k) into batch
    count = [0]

    def flush():
        if not tris:
            batch.clear()
            return
        if slot is not None:
            # Grouped by matrix, each group loaded under its own at its own
            # place in the vertex buffer (G_VTX v0 * 5 in bits 16-23).
            order = sorted(range(len(batch)), key=lambda i: batch[i][16])
            where = {old: new for new, old in enumerate(order)}
            start = 0
            while start < len(order):
                m = batch[order[start]][16]
                end_ = start
                while end_ < len(order) and batch[order[end_]][16] == m:
                    end_ += 1
                at = len(vertices)
                for i in order[start:end_]:
                    vertices.extend(batch[i][:16])
                n = end_ - start
                out.append((0x01020040, ('mtx', m)))
                out.append(((0x04 << 24) | ((start * 5) << 16) | (n << 9) | (n * 16 - 1), ('vtx', at)))
                start = end_
            for k_, (i, j, k, twice) in enumerate(tris):
                tris[k_] = (where[i], where[j], where[k], twice)
        else:
            at = len(vertices)
            for v in batch:
                vertices.extend(v)
            n = len(batch)
            out.append(((0x04 << 24) | (n << 9) | (n * 16 - 1), ('vtx', at)))
        for i, j, k, twice in tris:
            out.append((0xBF << 24, ((i * 5) << 16) | ((j * 5) << 8) | (k * 5)))
            count[0] += 1
            if twice:
                out.append((0xBF << 24, ((i * 5) << 16) | ((k * 5) << 8) | (j * 5)))
        batch.clear()
        tris.clear()

    vbuf = [None] * 64
    vbuf_m = [slot] * 64        # (a character's) the matrix each was loaded under
    state = {'skip': False, 'half': 0, 'lit': False, 'mtx': slot}

    def walk(a, depth=0):
        seg = (a >> 24) & 0x0F
        if seg not in data or depth > 10:
            return
        d = data[seg]
        at = a & 0xFFFFFF
        while at + 8 <= len(d):
            w0, w1 = struct.unpack('>II', d[at:at + 8])
            op = w0 >> 24
            at += 8
            if op == 0x01:
                n = (w0 >> 12) & 0xFF
                end = (w0 >> 1) & 0x7F
                vs = (w1 >> 24) & 0x0F
                if vs not in data:
                    continue
                base = w1 & 0xFFFFFF
                for i in range(n):
                    vbuf[end - n + i] = moved(bytes(data[vs][base + i * 16:base + i * 16 + 16]))
                    vbuf_m[end - n + i] = state['mtx']
            elif op in (0x05, 0x06, 0x07):
                if state['skip']:
                    continue
                groups = [((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF)]
                if op == 0x06:
                    groups.append(((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF))
                if op == 0x07:   # a quad: two triangles
                    a_, b_, c_, d_ = (w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF, w1 & 0xFF
                    groups = [(a_, b_, c_), (a_, c_, d_)]
                for g in groups:
                    recs = [vbuf[i // 2] for i in g]
                    if any(r is None for r in recs):
                        continue
                    where = [struct.unpack('>hhh', r[0:6]) for r in recs]
                    if keep is not None and not keep(where):
                        continue
                    twice = both_sides is not None and both_sides(where)
                    if state['lit']:
                        recs = [bake(r) for r in recs]
                    if slot is not None:
                        recs = [r + bytes([vbuf_m[i // 2]]) for r, i in zip(recs, g)]
                    more = [(recs, twice)]
                    if extra is not None:
                        more += [(m, True) for m in extra(where, recs)]
                    for rs, both in more:
                        if len(batch) + 3 > BATCH:
                            flush()
                        idx = []
                        for r in rs:
                            if r in batch:
                                idx.append(batch.index(r))
                            else:
                                batch.append(r)
                                idx.append(len(batch) - 1)
                        tris.append((idx[0], idx[1], idx[2], both))
            elif op == 0xE1:
                state['half'] = w1
            elif op == 0x04:     # G_BRANCH_Z: the nearer, detailed list
                flush()
                walk(state['half'], depth + 1)
                return
            elif op == 0xDE:
                flush()
                walk(w1, depth + 1)
                if (w0 >> 16) & 0xFF:
                    return
            elif op == 0xDF:
                flush()
                return
            elif op == 0xD9:     # geometry mode: whether the vertices are lit
                clear, set_ = ~w0 & 0xFFFFFF, w1
                if clear & 0x20000:
                    state['lit'] = False
                if set_ & 0x20000:
                    state['lit'] = True
            elif op == 0xDA and slot is not None:
                if (w1 >> 24) == 0x0D:
                    state['mtx'] = (w1 & 0xFFFFFF) // 0x40
            elif op in (0x00, 0x03, 0xDA, 0xD8, 0xDB, 0xDC, 0x02):
                continue         # no-op, cull, matrices, segments
            elif op == 0xD7:     # G_TEXTURE: the on bit moves
                flush()
                level, tile, on = (w0 >> 11) & 7, (w0 >> 8) & 7, (w0 >> 1) & 0x7F
                out.append(((0xBB << 24) | (level << 11) | (tile << 8) | (1 if on else 0), w1))
            elif op in (0xE2, 0xE3):
                flush()
                length = (w0 & 0xFF) + 1
                shift = 32 - ((w0 >> 8) & 0xFF) - length
                # The render mode's first cycle, Majora's Mask's fog: passed
                # through instead (Wave Race's fog range fogged it all).
                if op == 0xE2 and shift <= 3 and shift + length >= 32:
                    w1 = (w1 & ~0xCCCC0000) | (0x0C080000 & 0xCCCC0000)
                out.append((((0xB9 if op == 0xE2 else 0xBA) << 24) | (shift << 8) | length, w1))
            elif op == 0xFD:     # the texture image: ours, or none (animated)
                flush()
                seg = (w1 >> 24) & 0x0F
                state['skip'] = (w1 >> 24) >= 0x80 or seg not in names
                if not state['skip']:
                    key = names[seg]
                    for k_, a_, s_, w_, loaded in pending:
                        if not loaded:
                            atlas.add(k_, a_, 4096)     # set and never loaded: a generous share
                    pending[:] = [[key, w1 & 0xFFFFFF, (w0 >> 19) & 3, (w0 & 0xFFF) + 1, False]]
                    out.append((w0, ('tex', key, w1 & 0xFFFFFF)))
            elif op in (0xF3, 0xF0, 0xF4):   # loads: how much of the image they read
                flush()
                if pending:
                    key, image, size, width, _loaded = pending[0]
                    pending[0][4] = True
                    if op == 0xF3:
                        length = (((w1 >> 12) & 0xFFF) + 1) * TEXEL_BYTES[size]
                    elif op == 0xF0:
                        length = (((w1 >> 14) & 0x3FF) + 1) * 2
                    else:
                        lrt = (w1 & 0xFFF) >> 2
                        length = (lrt + 1) * width * TEXEL_BYTES[size]
                    atlas.add(key, image, length)
                out.append((w0, w1))
            elif op >= 0xE4:     # RDP commands, as they are
                flush()
                out.append((w0, w1))
        flush()

    for a in roots:
        state['skip'] = False
        walk(a)
    for key, at, size, width, loaded in pending:
        if not loaded:
            atlas.add(key, at, 4096)   # set and never loaded here: a generous share
    if end is not None:
        out.append((end, 0))
    return vertices, out, count[0]


# Lit vertices hold a normal (s8 x, y, z) where unlit ones hold a colour:
# their light is baked in, a sun high and to one side over an ambient, the
# way the seafloor is lit (the game's own lights at the time were nothing to
# do with it: the land came out orange and magenta).
SUN = (0.3, 0.9, 0.3)


def bake(record):
    nx, ny, nz = struct.unpack('>bbb', record[12:15])
    length = max(1e-6, (nx * nx + ny * ny + nz * nz) ** 0.5)
    sl = (SUN[0] ** 2 + SUN[1] ** 2 + SUN[2] ** 2) ** 0.5
    light = max(0.0, (nx * SUN[0] + ny * SUN[1] + nz * SUN[2]) / (length * sl))
    shade = int(min(1.0, 0.5 + 0.5 * light) * 255)
    return record[:12] + bytes((shade, shade, shade, record[15]))


def fixed_matrix(m):
    """A 4x4 float matrix (rows) as an N64 Mtx: integer halves, then fractions."""
    whole, part = [], []
    for r in range(4):
        for c in range(4):
            v = int(round(m[r][c] * 65536))
            whole.append((v >> 16) & 0xFFFF)
            part.append(v & 0xFFFF)
    return struct.pack('>16H16H', *whole, *part)


def main():
    rom = Rom(sys.argv[1])
    out_dir = sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    scene, header, col, rooms = scene_and_rooms(rom, GREAT_BAY_COAST)
    room = rooms[0]
    scene_tex = rom.file(SCENE_TEXTURE_05)
    cape, _cape_header, cape_col, cape_rooms = scene_and_rooms(rom, ZORA_CAPE)
    # Where each scene's scenery is (for the seam between them), and what
    # is drawn at all (for the walls nobody sees).
    def scenery_of(segments, room_, move):
        sc = Scenery(segments)
        for opa, xlu in room_lists(room_):
            for a in (opa, xlu):
                if a:
                    sc.walk(a)
        T = mm_merge.triangles(sc)
        return T + np.array(move, float) if len(T) else T
    bay_t = scenery_of({2: scene, 3: room, 6: scene_tex}, room, (0, 0, 0))
    cape_t = scenery_of({2: cape, 3: cape_rooms[0], 6: scene_tex}, cape_rooms[0], ZORA_CAPE_SHIFT)
    owners = mm_merge.Owners([bay_t, cape_t])
    drawn = mm_merge.Visible(np.concatenate([bay_t, cape_t]))
    atlas = Atlas({'scene': scene, 'room': room, 'tex': scene_tex, 'cape': cape, 'cape_room': cape_rooms[0]})
    pack = Pack()
    roots = [a for opa, xlu in room_lists(room) for a in (opa, xlu) if a]
    def coast(t):
        c = np.mean(np.asarray(t, float), axis=0)
        return c[0] > COAST_X and c[2] < COAST_Z and owners.keeps(0, t)
    # The ridge is a backdrop: drawn from the south, it has no foot, and
    # from the open water to the north it (and every copy) hung in the air.
    # Each edge along its bottom (every triangle it is an edge of above it:
    # one triangle's edge, or a fold creased downward -- arches stood open
    # under those, the user's mark 3 the third night) gets a skirt of the
    # same rock down to SKIRT_DEPTH.
    def above(T):
        """Each edge of triangles T with its triangles' third corners'
        heights."""
        out = {}
        for t in T:
            for i in range(3):
                a, b, c = (tuple(int(v) for v in t[(i + k) % 3]) for k in range(3))
                out.setdefault(frozenset((a, b)), []).append(c[1])
        return out

    ridge_t = [t for t in bay_t if coast(t) and not np.all(np.abs(t[:, 1]) < 2.0)]
    thirds = above(ridge_t)
    bottoms = set()
    for t in ridge_t:
        for i in range(3):
            a, b, c = (tuple(int(v) for v in t[(i + k) % 3]) for k in range(3))
            if min(thirds[frozenset((a, b))]) > max(a[1], b[1]) and min(a[1], b[1]) > SKIRT_DEPTH:
                bottoms.add(frozenset((a, b)))

    # The filled-in rock's edges (FILL_REGIONS): the scenery's edges (one
    # triangle's only) above the water in each region, and its folds creased
    # downward.
    fills = set()
    fill_scene = {}
    for which, x0, x1, z0, z1, tops in FILL_REGIONS:
        T = [t for t in (bay_t if which == 0 else cape_t) if owners.keeps(which, t)]
        thirds_ = above(T)
        for t in T:
            c = np.mean(t, axis=0)
            if not (x0 < c[0] < x1 and z0 < c[2] < z1):
                continue
            for i in range(3):
                a, b = tuple(int(v) for v in t[i]), tuple(int(v) for v in t[(i + 1) % 3])
                e = thirds_[frozenset((a, b))]
                if ((len(e) == 1 and tops) or min(e) > max(a[1], b[1])) and min(a[1], b[1]) > -30:
                    fills.add(frozenset((a, b)))
                    fill_scene[frozenset((a, b))] = which

    def skirt(where, recs):
        out = []
        for i in range(3):
            a, b = tuple(where[i]), tuple(where[(i + 1) % 3])
            edge = frozenset((a, b))
            if edge not in bottoms and edge not in fills:
                continue
            depth = SKIRT_DEPTH if edge in bottoms else FILL_DEPTH
            ra, rb = recs[i], recs[(i + 1) % 3]

            def lowered(r, y):
                s_, t_ = struct.unpack('>hh', r[8:12])
                t2 = max(-32768, min(32767, t_ + int((y - depth) * 2)))
                return struct.pack('>hhh', *struct.unpack('>hh', r[0:4])[:1], depth, struct.unpack('>h', r[4:6])[0]) + \
                    r[6:8] + struct.pack('>hh', s_, t2) + r[12:]
            la, lb = lowered(ra, a[1]), lowered(rb, b[1])
            out += [(ra, rb, lb), (ra, lb, la)]
        return out

    # (The ridge where it is skirted too: from the south its peaks hung in
    # the air, the user's mark 7; the water it walled off is past the
    # current's south line now.)
    vertices, words, triangles = convert({2: scene, 3: room, 6: scene_tex}, {2: 'scene', 3: 'room', 6: 'tex'}, roots, atlas, end=None,
                                         keep=lambda t: owners.keeps(0, t) and not rim(t), both_sides=mm_merge.steep, extra=skirt)
    # Zora Cape, next along the coast: its own scene in Majora's Mask, the
    # same world ZORA_CAPE_SHIFT on (its vertices where Great Bay Coast's
    # edge has them too).
    sv, sw, coast_triangles = convert({2: scene, 3: room, 6: scene_tex}, {2: 'scene', 3: 'room', 6: 'tex'}, roots, atlas,
                                      end=0xB8000000, keep=coast, both_sides=mm_merge.steep, extra=skirt)
    cape_roots = [a for opa, xlu in room_lists(cape_rooms[0]) for a in (opa, xlu) if a]
    cv, cw, cape_triangles = convert({2: cape, 3: cape_rooms[0], 6: scene_tex}, {2: 'cape', 3: 'cape_room', 6: 'tex'}, cape_roots, atlas,
                                     end=None, shift=ZORA_CAPE_SHIFT, keep=lambda t: owners.keeps(1, t), both_sides=mm_merge.steep, extra=skirt)
    # Its people's models: each limb's list converted in the limb's units.
    keep = rom.file(GAMEPLAY_KEEP)
    animetion = rom.file(LINK_ANIMETION)
    models = {}
    for name, spec in MODELS.items():
        obj = rom.file(spec['object'])
        atlas.files[name] = obj
        atlas.files[name + '_8'] = obj[spec['seg8']:] if spec['seg8'] is not None else obj
        atlas.files[name + '_9'] = obj[spec['seg9']:] if spec['seg9'] is not None else obj
        atlas.files['keep'] = keep
        skel = mm_character.Skeleton(obj, spec['skeleton'], spec['lod'])
        data = {6: obj, 8: atlas.files[name + '_8'], 9: atlas.files[name + '_9'], 4: keep}
        names = {6: name, 8: name + '_8', 9: name + '_9', 4: 'keep'}
        limbs = []
        for i in skel.order:
            limb = skel.limbs[i]
            if limb['slot'] < 0:
                continue
            lv, lw, _n = convert(data, names, [limb['dl']], atlas, end=0xB8000000, slot=limb['slot'])
            limbs.append((limb['slot'], lv, lw))
        items = []
        for item, dl, hand, seg8 in spec['items']:
            atlas.files['item_' + item] = keep[seg8:] if seg8 is not None else keep
            iv, iw, _n = convert({4: keep, 8: atlas.files['item_' + item]}, {4: 'keep', 8: 'item_' + item}, [0x04000000 | dl], atlas,
                                 end=0xB8000000)
            items.append((item, hand, iv, iw))
        anims = []
        for aname, kind, at_, frames in spec['anims']:
            if kind == 'npc':
                rows = mm_character.npc_frames(obj, at_, len(skel.limbs) + 1)
            else:
                rows = [row[:len(skel.limbs) + 1] for row in mm_character.player_frames(animetion, at_, frames)]
            anims.append((aname, rows))
        models[name] = {'skel': skel, 'limbs': limbs, 'items': items, 'anims': anims, 'scale': spec['scale']}
    tex_at = pack.add(atlas.build(), 8)
    vat = pack.add(bytes(vertices))
    cvat = pack.add(bytes(cv))
    svat = pack.add(bytes(sv))
    # The model matrix: Majora's Mask units scaled to Wave Race's.
    matrix_at = pack.add(fixed_matrix([[SCALE, 0, 0, 0], [0, SCALE, 0, 0], [0, 0, SCALE, 0], [0, 0, 0, 1]]))

    def kseg0(offset):
        return 0x80000000 | (BASE + offset)

    def assemble(ws, vertex_at):
        b = bytearray()
        for w0, w1 in ws:
            if isinstance(w1, tuple):
                if w1[0] == 'tex':
                    w1 = kseg0(tex_at + atlas.offset(w1[1], w1[2]))
                else:
                    w1 = kseg0(vertex_at + w1[1])
            b += struct.pack('>II', w0, w1)
        return bytes(b)

    # The models' vertices, skeletons and animations (the tables the host's,
    # little-endian), then each person: its matrices (one a drawing limb,
    # then one an item), and its list -- each limb under its matrix, each
    # item under its own (the game turns them on and off) -- the limbs'
    # lists its own, their matrices its.
    model_table = bytearray()
    model_index = {}
    for name, m in models.items():
        for k, (slot_, lv, lw) in enumerate(m['limbs']):
            m['limbs'][k] = (slot_, pack.add(bytes(lv)), lw)
        for k, (item, hand, iv, iw) in enumerate(m['items']):
            m['items'][k] = (item, hand, pack.add(bytes(iv)), iw)
        limb_at = pack.add(m['skel'].table(), 4)
        anim_rows = bytearray()
        for aname, rows in m['anims']:
            data_at = pack.add(mm_character.frames_blob(rows), 4)
            anim_rows += struct.pack('<II', len(rows), data_at)
        anims_at = pack.add(bytes(anim_rows), 4)
        items_rows = b''.join(struct.pack('<I', hand) for _i, hand, _v, _w in m['items'])
        items_at = pack.add(items_rows or b'\0' * 4, 4)
        model_index[name] = len(model_index)
        model_table += struct.pack('<IIIIIIfI', len(m['skel'].limbs), limb_at, m['skel'].slots, len(m['anims']), anims_at,
                                   len(m['items']), m['scale'], items_at)
    models_at = pack.add(bytes(model_table), 4)
    people_rows = bytearray()
    people_lists = []
    for name, x, z, yaw in PEOPLE:
        m = models[name]
        mats = pack.add(b'\0' * (64 * (m['skel'].slots + len(m['items']))), 8)

        def placed(ws, vertex_at):
            out_ = []
            for w0, w1 in ws:
                if isinstance(w1, tuple) and w1[0] == 'mtx':
                    w1 = kseg0(mats + 64 * w1[1])
                out_.append((w0, w1))
            return assemble(out_, vertex_at)
        lists = [pack.add(placed(lw, lv), 8) for _slot, lv, lw in m['limbs']]
        item_lists = [pack.add(assemble(iw, iv), 8) for _i, _h, iv, iw in m['items']]
        dl = bytearray()
        first = m['limbs'][0][0]
        dl += struct.pack('>II', 0x01060040, kseg0(mats + 64 * first))      # pushed: put back after
        for (slot_, _v, _w), at_ in zip(m['limbs'], lists):
            dl += struct.pack('>IIII', 0x01020040, kseg0(mats + 64 * slot_), 0x06000000, kseg0(at_))
        item_calls = []
        for k, at_ in enumerate(item_lists):
            dl += struct.pack('>II', 0x01020040, kseg0(mats + 64 * (m['skel'].slots + k)))
            item_calls.append(len(dl))
            dl += struct.pack('>II', 0x00000000, kseg0(at_))                # off until the game says (G_DL is 0x06)
        dl += struct.pack('>II', 0xBD000000, 0) + struct.pack('>II', 0xB8000000, 0)
        dl_at_ = pack.add(bytes(dl), 8)
        people_lists.append(dl_at_)
        calls_at = pack.add(b''.join(struct.pack('<I', dl_at_ + c) for c in item_calls) or b'\0' * 4, 4)
        people_rows += struct.pack('<IIIIfff', model_index[name], mats, len(item_calls), calls_at, x, z, yaw)
    people_at = pack.add(bytes(people_rows), 4)
    coast_list = pack.add(assemble(sw, svat), 8)
    coast_calls = bytearray()
    for dx, dy, dz in COAST_COPIES:
        m = pack.add(fixed_matrix([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [dx, dy, dz, 1]]))
        coast_calls += struct.pack('>IIIIII', 0x01040040, kseg0(m), 0x06000000, kseg0(coast_list), 0xBD000000, 0)
    # Its state first, as Majora's Mask's engine sets it before a room's
    # lists: two cycles (their fog was blended in the first, now passed
    # through: by Wave Race's fog range the land came out all fog, the
    # haze's colour), filtered, in perspective; unlit (lists that want
    # lighting turn it on) and unculled, depth tested, shaded smooth.
    state = struct.pack('>' + 'I' * 18,
                        0xE7000000, 0,
                        0xBA001402, 0x00100000,    # two cycles
                        0xBA000C02, 0x00002000,    # bilinear
                        0xBA001301, 0x00080000,    # perspective
                        0xBA001001, 0x00000000,    # LOD: tile
                        0xB6000000, 0x000F3001,    # clear depth, cull, fog, lighting, texgen
                        0xB7000000, 0x00000205,    # set depth, shade, smooth
                        0xB9000002, 0x00000000,    # no alpha compare (the lists set their own)
                        0xBB000001, 0xFFFFFFFF)    # texture on
    body = assemble(words, vat) + assemble(cw, cvat)
    # The sand floor: the seafloor's sand material (its block of state, from
    # its texture on to its first vertices), then squares of it, under a
    # matrix halving its units (past what a vertex holds otherwise).
    at_ = next(k for k, (w0, w1) in enumerate(words) if w0 >> 24 == 0xFD and w1 == ('tex',) + SAND_TEXTURE)
    first = max(k for k in range(at_) if words[k][0] >> 24 == 0xBB)
    last = next(k for k in range(at_, len(words)) if words[k][0] >> 24 == 0x04)
    sand_v = bytearray()
    sand_w = list(words[first:last])
    dens = 0.1 * 32                                 # the seafloor's 0.1 texels a unit, S10.5
    texels = int(SAND_CELL * dens)
    sand_tris = []                                  # three (x, y, z, s, t, shade) each
    cells = [(x, z) for x in range(SAND_AREA[0], SAND_AREA[1], SAND_CELL) for z in range(SAND_AREA[2], SAND_AREA[3], SAND_CELL)]
    px_, pz_, pr_ = PIT
    for x, z in cells:
        if not (x <= px_ < x + SAND_CELL and z <= pz_ < z + SAND_CELL):
            q = [(x + dx * SAND_CELL, SAND_Y, z + dz * SAND_CELL, dx * texels, dz * texels, 240) for dx, dz in ((0, 0), (1, 0), (1, 1), (0, 1))]
            sand_tris += [(q[0], q[1], q[2]), (q[0], q[2], q[3])]
            continue
        # The pit's square: its floor round a hole in the ring, the hole's
        # side down to PIT_Y, darkening, and its bottom.
        def floor_at(fx, fz, y, shade):
            return (fx, y, fz, int((fx - x) * dens), int((fz - z) * dens), shade)
        corners = [math.atan2(cz - pz_, cx - px_) for cx, cz in ((x, z), (x + SAND_CELL, z), (x + SAND_CELL, z + SAND_CELL), (x, z + SAND_CELL))]
        angles = sorted([2 * math.pi * k / PIT_SIDES - math.pi for k in range(PIT_SIDES)] + corners)
        ring, edge = [], []
        for a_ in angles:
            dx, dz = math.cos(a_), math.sin(a_)
            reach = min(((x + SAND_CELL - px_) / dx if dx > 1e-9 else (x - px_) / dx if dx < -1e-9 else 1e9),
                        ((z + SAND_CELL - pz_) / dz if dz > 1e-9 else (z - pz_) / dz if dz < -1e-9 else 1e9))
            ring.append((px_ + pr_ * dx, pz_ + pr_ * dz))
            edge.append((px_ + reach * dx, pz_ + reach * dz))
        arc = 0.0
        for i in range(len(angles)):
            j = (i + 1) % len(angles)
            (ax, az), (bx, bz) = ring[i], ring[j]
            ea, eb = edge[i], edge[j]
            A, B = floor_at(ax, az, SAND_Y, 240), floor_at(bx, bz, SAND_Y, 240)
            sand_tris += [(A, B, floor_at(*eb, SAND_Y, 240)), (A, floor_at(*eb, SAND_Y, 240), floor_at(*ea, SAND_Y, 240))]
            step = math.hypot(bx - ax, bz - az)
            s0, s1 = int(arc * dens), int((arc + step) * dens)
            arc += step
            deep = int((SAND_Y - PIT_Y) * dens)
            wa, wb = (ax, SAND_Y, az, s0, 0, 200), (bx, SAND_Y, bz, s1, 0, 200)
            wc, wd = (bx, PIT_Y, bz, s1, deep, 70), (ax, PIT_Y, az, s0, deep, 70)
            sand_tris += [(wa, wb, wc), (wa, wc, wd)]
            sand_tris.append((floor_at(px_, pz_, PIT_Y, 70), floor_at(bx, bz, PIT_Y, 70), floor_at(ax, az, PIT_Y, 70)))
    for k in range(0, len(sand_tris), 8):
        group = sand_tris[k:k + 8]
        at = len(sand_v)
        for tri in group:
            for vx, vy, vz, vs, vt, shade in tri:
                sand_v += struct.pack('>hhhhhhBBBB', int(round(vx / 2)), int(vy), int(round(vz / 2)), 0, vs, vt, shade, shade, shade, 255)
        n = len(group) * 3
        sand_w.append(((0x04 << 24) | (n << 9) | (n * 16 - 1), ('vtx', at)))
        for i in range(len(group)):
            sand_w.append((0xBF << 24, (i * 3 * 5 << 16) | ((i * 3 + 1) * 5 << 8) | ((i * 3 + 2) * 5)))
    sand_w.append((0xB8000000, 0))
    sand_list = pack.add(assemble(sand_w, pack.add(bytes(sand_v))), 8)
    sand_m = pack.add(fixed_matrix([[2, 0, 0, 0], [0, 1, 0, 0], [0, 0, 2, 0], [0, 0, 0, 1]]))
    sand_call = struct.pack('>IIIIII', 0x01040040, kseg0(sand_m), 0x06000000, kseg0(sand_list), 0xBD000000, 0)
    people_calls = b''.join(struct.pack('>II', 0x06000000, kseg0(a_)) for a_ in people_lists)
    dl_at = pack.add(state + sand_call + body + people_calls + bytes(coast_calls) + struct.pack('>II', 0xB8000000, 0), 8)
    # Collision triangles, Wave Race units: Great Bay Coast's, then Zora
    # Cape's moved into place (but for the triangles both have).
    # Left out: the seam's other side (as the scenery), and the walls in the
    # water nobody sees (steep, through the waterline, with less than half
    # of them by anything drawn).
    coll = bytearray()
    seen = set()
    count = unseen = 0
    sources = [(0, col, (0, 0, 0), None), (1, cape_col, ZORA_CAPE_SHIFT, None)]
    # The ridge has no collision of its own (Link never reached it): its
    # scenery is made solid at each copy.
    class Ridge:
        vertices = []
        polygons = []
    for t in ridge_t:
        tri = [tuple(int(v) for v in p_) for p_ in t]
        quads = [tri]
        for i in range(3):
            a, b = tri[i], tri[(i + 1) % 3]
            if frozenset((a, b)) in bottoms:
                la, lb = (a[0], SKIRT_DEPTH, a[2]), (b[0], SKIRT_DEPTH, b[2])
                quads += [[a, b, lb], [a, lb, la]]
        for q in quads:
            base = len(Ridge.vertices)
            Ridge.vertices += q
            Ridge.polygons.append({'v': (base, base + 1, base + 2)})
    sources += [(0, Ridge, copy, None) for copy in [(0, 0, 0)] + COAST_COPIES]
    # The filled-in rock's faces, solid (where they are only), by scene.
    fill_sources = []
    for which_ in (0, 1):
        class Fill:
            vertices = []
            polygons = []
        for e in fills:
            if fill_scene[e] != which_:
                continue
            a, b = sorted(e)
            la, lb = (a[0], FILL_DEPTH, a[2]), (b[0], FILL_DEPTH, b[2])
            for q in ([a, b, lb], [a, lb, la]):
                base = len(Fill.vertices)
                Fill.vertices += q
                Fill.polygons.append({'v': (base, base + 1, base + 2)})
        fill_sources.append(Fill)
        sources.append((which_, Fill, (0, 0, 0), None))
    for which, c_, shift, only in sources:
        for p in c_.polygons:
            tri = tuple(tuple(c_.vertices[i][k] + shift[k] for k in range(3)) for i in p['v'])
            if only is not None and not only(tuple(tuple(c_.vertices[i]) for i in p['v'])):
                continue
            key = tuple(sorted(tri))
            if key in seen or not owners.keeps(which, tri):
                continue
            made = c_ is Ridge or c_ in fill_sources
            if not made and (max(p_[1] for p_ in tri) < DEEP or rim(tri)):
                continue
            seen.add(key)
            # (A copy of the coast's is judged where it was copied from.)
            seen_at = tuple(tuple(c_.vertices[i]) for i in p['v']) if only is not None else tri
            if not made and mm_merge.steep(tri) and mm_merge.through_waterline(tri) and drawn.fraction(np.array(seen_at, float)) < 0.5:
                unseen += 1
                continue
            for x, y, z in tri:
                coll += struct.pack('<3f', x * SCALE, y * SCALE, z * SCALE)
            count += 1
    # What stands in the water drawn but not solid: its own triangles.
    raised = np.frombuffer(bytes(coll), dtype='<f4').reshape(-1, 3, 3) / SCALE
    raised = raised[raised[:, :, 1].max(axis=1) > -20].mean(axis=1)
    solid = 0
    for which, T in ((0, bay_t), (1, cape_t)):
        for t in T:
            if t[:, 1].max() <= 30 or t[:, 1].min() >= 30 or not owners.keeps(which, t):
                continue
            # (Not the flat sheet at the scene's origin: a billboard's,
            # placed by its own matrix in Majora's Mask.)
            if np.all(t[:, 2] == 0) and np.all(np.abs(t[:, 0]) < 100):
                continue
            c = t.mean(axis=0)
            if len(raised) and np.hypot(raised[:, 0] - c[0], raised[:, 2] - c[2]).min() < SOLID_REACH:
                continue
            for x, y, z in t:
                coll += struct.pack('<3f', x * SCALE, y * SCALE, z * SCALE)
            count += 1
            solid += 1
    coll_at = pack.add(bytes(coll))
    # The open water, for its people: each square's highest collision below
    # WATER_DEPTH, or none (Majora's Mask units; 1 a byte).
    tris = np.frombuffer(bytes(coll), dtype='<f4').reshape(-1, 3, 3) / SCALE
    wx0, wz0 = -11000.0, -10000.0
    ww, wh = int(17500 / WATER_CELL), int(30000 / WATER_CELL)
    # (The height at each square's middle, on each triangle over it.)
    top = np.full((wh, ww), -1e9)
    cx = wx0 + (np.arange(ww) + 0.5) * WATER_CELL
    cz = wz0 + (np.arange(wh) + 0.5) * WATER_CELL
    for t in tris:
        (ax, ay, az), (bx, by, bz), (qx, qy, qz) = t
        i0, i1 = np.searchsorted(cx, min(ax, bx, qx)), np.searchsorted(cx, max(ax, bx, qx), 'right')
        j0, j1 = np.searchsorted(cz, min(az, bz, qz)), np.searchsorted(cz, max(az, bz, qz), 'right')
        if i0 >= i1 or j0 >= j1:
            continue
        X, Z = np.meshgrid(cx[i0:i1], cz[j0:j1])
        d = (bz - qz) * (ax - qx) + (qx - bx) * (az - qz)
        if abs(d) < 1e-6:
            continue
        l1 = ((bz - qz) * (X - qx) + (qx - bx) * (Z - qz)) / d
        l2 = ((qz - az) * (X - qx) + (ax - qx) * (Z - qz)) / d
        l3 = 1 - l1 - l2
        inside = (l1 >= -0.01) & (l2 >= -0.01) & (l3 >= -0.01)
        y = l1 * ay + l2 * by + l3 * qy
        sub = top[j0:j1, i0:i1]
        sub[inside] = np.maximum(sub[inside], y[inside])
    water = (top < WATER_DEPTH).astype(np.uint8)
    water_at = pack.add(water.tobytes(), 4)
    if BASE + len(pack.data) > LIMIT:
        raise SystemExit(f'pack too big: {len(pack.data)} bytes')
    water_y = col.water[0]['y'] * SCALE if col.water else 0.0
    head = struct.pack('<8sIIIIIIfffffIIIIfffffffIII', b'WR64GBAY', 3, len(pack.data), dl_at, matrix_at, coll_at, count,
                       water_y, START[0] * SCALE, START[1] * SCALE, START[2], SCALE,
                       len(PEOPLE), people_at, len(models), models_at, *BOUNDS,
                       wx0, wz0, WATER_CELL, ww, wh, water_at)
    pack.data[0:len(head)] = head
    with open(os.path.join(out_dir, 'greatbay.pack'), 'wb') as f:
        f.write(pack.data)
    print(f'greatbay.pack: {len(pack.data)} bytes (textures {len(atlas.build())}), {triangles} + {cape_triangles} scenery triangles ({coast_triangles} of coast, {len(COAST_COPIES)} copies), '
          f'{count} collision triangles ({unseen} unseen walls left out, {solid} made solid, {len(fills)} rock edges filled in), {len(sand_tris)} triangles of sand, water at {water_y}, {len(PEOPLE)} people')


if __name__ == '__main__':
    main()

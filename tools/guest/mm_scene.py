#!/usr/bin/env python3
"""A Majora's Mask scene from the player's ROM: its header, collision, water
boxes and rooms, and each room's scenery as triangles.

Scene and room files start with 8-byte header commands (zeldaret/mm
z64scene.h) up to 0x14 (end). The scene is segment 2, a room segment 3.
Collision header (0x2C): s16 min[3], max[3]; u16 vertex count, Vec3s
vertices; u16 polygon count, polygons (0x10: type, three vertex indices in
their low 13 bits, s16 normal[3] scaled by 0x7FFF, s16 distance); surface
types; cameras; u16 water box count, water boxes (0x10: s16 x, y (the
surface), z, x length, z length, pad, u32 properties).

A room's mesh (command 0x0A) is one of three shapes: 0, a list of (opaque,
see-through) display lists; 1, prerendered (not outdoors); 2, the same as 0
with a cull sphere per entry (16 bytes: s16 x, y, z, radius, opaque,
see-through). The display lists are F3DEX2: G_VTX 0x01, G_TRI1 0x05,
G_TRI2 0x06, G_DL 0xDE, G_ENDDL 0xDF, and G_BRANCH_Z 0x04 (with
G_RDPHALF_1 0xE1 before it) switching to a nearer, more detailed list; RDP commands as on any microcode.
"""

import struct

from mm_rom import Rom


def u32(d, o):
    return struct.unpack('>I', d[o:o + 4])[0]


def commands(d, at=0):
    out = []
    c = at
    while c + 8 <= len(d):
        cmd = d[c]
        out.append((cmd, d[c + 1], u32(d, c + 4)))
        if cmd == 0x14:
            break
        c += 8
    return out


class Collision:
    def __init__(self, d, offset):
        o = offset
        self.min = struct.unpack('>3h', d[o:o + 6])
        self.max = struct.unpack('>3h', d[o + 6:o + 12])
        nv = struct.unpack('>H', d[o + 12:o + 14])[0]
        vp = u32(d, o + 16) & 0xFFFFFF
        np_ = struct.unpack('>H', d[o + 20:o + 22])[0]
        pp = u32(d, o + 24) & 0xFFFFFF
        self.surface_types = u32(d, o + 28) & 0xFFFFFF
        nw = struct.unpack('>H', d[o + 36:o + 38])[0]
        wp = u32(d, o + 40) & 0xFFFFFF
        self.vertices = [struct.unpack('>3h', d[vp + i * 6:vp + i * 6 + 6]) for i in range(nv)]
        self.polygons = []
        for i in range(np_):
            b = pp + i * 16
            kind, a, bb, c = struct.unpack('>4H', d[b:b + 8])
            normal = struct.unpack('>3h', d[b + 8:b + 14])
            dist = struct.unpack('>h', d[b + 14:b + 16])[0]
            self.polygons.append({'type': kind, 'flags_a': a >> 13, 'flags_b': bb >> 13,
                                  'v': (a & 0x1FFF, bb & 0x1FFF, c & 0x1FFF),
                                  'normal': tuple(n / 0x7FFF for n in normal), 'dist': dist})
        self.water = []
        for i in range(nw):
            b = wp + i * 16
            x, y, z, xl, zl = struct.unpack('>5h', d[b:b + 10])
            self.water.append({'x': x, 'y': y, 'z': z, 'x_length': xl, 'z_length': zl, 'properties': u32(d, b + 12)})


class Scenery:
    """A room's display lists walked into triangles (world positions, vertex
    colours or normals, texture coordinates) with the texture image last set
    for each, as an F3DEX2 machine would."""

    def __init__(self, segments):
        self.segments = segments    # segment number -> bytes
        self.triangles = []         # (verts[3], texture image address, tile size)
        self.unresolved = set()

    def resolve(self, address):
        seg = (address >> 24) & 0x0F
        if (address >> 24) >= 0x80 or seg not in self.segments:
            self.unresolved.add(address >> 24)
            return None, 0
        return self.segments[seg], address & 0xFFFFFF

    def walk(self, address, depth=0):
        data, at = self.resolve(address)
        if data is None or depth > 8:
            return
        vbuf = [None] * 64
        while at + 8 <= len(data):
            w0, w1 = struct.unpack('>II', data[at:at + 8])
            op = w0 >> 24
            at += 8
            if op == 0x01:      # G_VTX: n, end index, address
                n = (w0 >> 12) & 0xFF
                end = (w0 >> 1) & 0x7F
                vd, va = self.resolve(w1)
                if vd is None:
                    continue
                for i in range(n):
                    o = va + i * 16
                    x, y, z, _flag, s, t = struct.unpack('>3hHhh', vd[o:o + 12])
                    r, g, b, a = vd[o + 12:o + 16]
                    vbuf[end - n + i] = (x, y, z, s, t, r, g, b, a)
            elif op in (0x05, 0x06):   # G_TRI1, G_TRI2: indices * 2
                tris = [((w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF)]
                if op == 0x06:
                    tris.append(((w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF))
                for t in tris:
                    vs = [vbuf[i // 2] for i in t]
                    if all(v is not None for v in vs):
                        self.triangles.append((vs, self.texture))
            elif op == 0xE1:    # G_RDPHALF_1: the next G_BRANCH_Z's list
                self.half = w1
            elif op == 0x04:    # G_BRANCH_Z: the near, detailed list instead
                self.walk(self.half, depth + 1)
                return
            elif op == 0xDE:    # G_DL: call (0) or branch (1)
                self.walk(w1, depth + 1)
                if (w0 >> 16) & 0xFF:
                    return
            elif op == 0xDF:    # G_ENDDL
                return
            elif op == 0xFD:    # G_SETTIMG
                self.texture = w1
        return

    texture = 0
    half = 0


def scene_and_rooms(rom, scene_index):
    d = rom.file(scene_index)
    header = commands(d)
    rooms = []
    for cmd, count, w in header:
        if cmd == 0x04:
            base = w & 0xFFFFFF
            for i in range(count):
                vstart, vend = struct.unpack('>II', d[base + i * 8:base + i * 8 + 8])
                rooms.append(rom.file(rom.file_at_vrom(vstart)))
    collision = None
    for cmd, count, w in header:
        if cmd == 0x03:
            collision = Collision(d, w & 0xFFFFFF)
    return d, header, collision, rooms


def room_lists(room):
    """A room's (opaque, see-through) display lists, from its mesh."""
    out = []
    for cmd, _b1, w in commands(room):
        if cmd != 0x0A:
            continue
        m = w & 0xFFFFFF
        kind, count = room[m], room[m + 1]
        start = u32(room, m + 4) & 0xFFFFFF
        size = 8 if kind == 0 else 16 if kind == 2 else 0
        for i in range(count if size else 0):
            e = start + i * size
            opa, xlu = struct.unpack('>II', room[e + size - 8:e + size])
            out.append((opa, xlu))
    return out

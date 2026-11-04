#!/usr/bin/env python3
"""A Majora's Mask character from the player's ROM, posed: its skeleton set
to one frame of one of its animations and its limbs' display lists walked
with each limb's matrix, into one display list in the model's own units
with its vertices moved into place (positions and normals), ready for the
pack's converter (greatbay_pack.py) to turn into Wave Race's microcode.

A skeleton (z64animation.h) is a header (u32 limb table, u8 limb count, and
for a flex skeleton u8 display list count); each limb (StandardLimb) s16
joint position[3], u8 child, u8 sibling (0xFF none), u32 display list. An
animation header: s16 frame count, u32 frame data (s16 values), u32 joint
indices (Vec3s per limb plus the root's translation first), u16 static
index max: an index below it is a fixed value, at or above it the value at
that index plus the frame. A limb's matrix is its parent's, then its joint
position (the root's the animation's translation), then rotations about z,
y and x (Matrix_TranslateRotateZYX). A flex skeleton's limbs that draw
write their matrices in order to segment 13, which their lists load from
(G_MTX 0xDA) to bend vertices across joints.
"""

import math
import struct


def u32(d, o):
    return struct.unpack('>I', d[o:o + 4])[0]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def translate(x, y, z):
    return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]]


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]]


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def angle(v):
    return (v if v < 0x8000 else v - 0x10000) * math.pi / 32768.0


def pose(obj, skeleton, animation, frame):
    """Each drawing limb's matrix, in drawing order: [(display list, matrix)]."""
    table = u32(obj, skeleton) & 0xFFFFFF
    count = obj[skeleton + 4]
    limbs = []
    for i in range(count):
        p = u32(obj, table + i * 4) & 0xFFFFFF
        x, y, z, child, sibling, dl = struct.unpack('>hhhBBI', obj[p:p + 12])
        limbs.append(((x, y, z), child, sibling, dl))
    frames, _pad, data, indices, static_max = struct.unpack('>hhIIH', obj[animation:animation + 14])
    data &= 0xFFFFFF
    indices &= 0xFFFFFF
    frame %= max(1, frames)

    def value(index):
        at = index + (frame if index >= static_max else 0)
        return struct.unpack('>H', obj[data + at * 2:data + at * 2 + 2])[0]

    joints = [struct.unpack('>3H', obj[indices + i * 6:indices + i * 6 + 6]) for i in range(count + 1)]
    out = []

    def walk(i, parent):
        if i == 0xFF:
            return
        pos, child, sibling, dl = limbs[i]
        if i == 0:
            t = [value(j) for j in joints[0]]
            pos = tuple(v if v < 0x8000 else v - 0x10000 for v in t)
        rx, ry, rz = (angle(value(j)) for j in joints[i + 1])
        m = mat_mul(parent, mat_mul(translate(*pos), mat_mul(rot_z(rz), mat_mul(rot_y(ry), rot_x(rx)))))
        if dl:
            out.append((dl, m))
        walk(child, m)
        walk(sibling, parent)

    walk(0, translate(0, 0, 0))
    return out


def bake(obj, skeleton, animation, frame, eye_texture, vertex_segment=0x0A, object_segment=0x0B):
    """The posed model: (vertex blob, F3DEX2 display list words) where the
    list's G_VTX point into the blob (segment `vertex_segment`) and its
    textures into the object (segment `object_segment`); segment 8 (the
    eyes) becomes `eye_texture` in the object."""
    limbs = pose(obj, skeleton, animation, frame)
    matrices = [m for _dl, m in limbs]
    vertices = bytearray()
    words = []

    def place(m, record):
        x, y, z = struct.unpack('>hhh', record[0:6])
        px = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3]
        py = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3]
        pz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]
        nx, ny, nz = struct.unpack('>bbb', record[12:15])
        qx = m[0][0] * nx + m[0][1] * ny + m[0][2] * nz
        qy = m[1][0] * nx + m[1][1] * ny + m[1][2] * nz
        qz = m[2][0] * nx + m[2][1] * ny + m[2][2] * nz
        ln = max(1e-6, math.sqrt(qx * qx + qy * qy + qz * qz))
        n = [max(-127, min(127, int(round(v / ln * 127)))) for v in (qx, qy, qz)]
        clamp = lambda v: max(-32768, min(32767, int(round(v))))
        return struct.pack('>hhh', clamp(px), clamp(py), clamp(pz)) + record[6:12] + struct.pack('>bbb', *n) + record[15:16]

    def walk(address, m, depth=0):
        if depth > 8 or (address >> 24) != 0x06:
            return
        at = address & 0xFFFFFF
        while at + 8 <= len(obj):
            w0, w1 = struct.unpack('>II', obj[at:at + 8])
            op = w0 >> 24
            at += 8
            if op == 0xDF:
                return
            if op == 0xDA:      # a matrix: another limb's (segment 13), for the next vertices
                if (w1 >> 24) == 0x0D and (w1 & 0xFFFFFF) // 0x40 < len(matrices):
                    m = matrices[(w1 & 0xFFFFFF) // 0x40]
                continue
            if op == 0xD8:
                continue
            if op == 0x01:
                n = (w0 >> 12) & 0xFF
                if (w1 >> 24) != 0x06:
                    continue
                src = w1 & 0xFFFFFF
                start = len(vertices)
                for i in range(n):
                    vertices.extend(place(m, obj[src + i * 16:src + i * 16 + 16]))
                words.append((w0, (vertex_segment << 24) | start))
                continue
            if op == 0xDE:
                if (w1 >> 24) == 0x06:
                    walk(w1, m, depth + 1)
                    if (w0 >> 16) & 0xFF:
                        return
                continue        # the actor's own lists (segment 12): its colours, left out
            if op == 0xFD:
                seg = w1 >> 24
                if seg == 0x06:
                    w1 = (object_segment << 24) | (w1 & 0xFFFFFF)
                elif seg == 0x08:
                    w1 = (object_segment << 24) | eye_texture
                else:
                    continue
            words.append((w0, w1))

    for dl, m in limbs:
        walk(dl, m)
    words.append((0xDF000000, 0))
    return bytes(vertices), words


def find_texture(obj, image):
    """An image's offset in an object, found by its bytes (or None)."""
    at = obj.find(image)
    return at if at >= 0 else None

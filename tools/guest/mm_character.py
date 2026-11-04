#!/usr/bin/env python3
"""Majora's Mask characters drawn and posed by the game as it goes, rather
than baked into frames (mm_model.py): each limb's display list converted
once, in the limb's own units, and a table of the skeleton and its
animations the game poses the limbs from every frame (src/guest_people.cpp).

A skeleton header is u32 limb table, u8 limb count, (flex) u8 display
list count; a limb is s16 joint position[3], u8 child, u8 sibling (0xFF
none), then u32 display list (StandardLimb, 12 bytes) or two, near and far
(LodLimb, 16 bytes: Link's). Limbs are drawn depth first, child then
sibling; each that draws writes its matrix to the next of segment 13's
slots, which the lists load from (G_MTX) to bend vertices across joints.

Animations become frames of (limb count + 1) joints, each three s16: the
root's translation first, then each limb's rotation (binary angles,
applied about z, then y, then x, after the joint's translation).

  - An NPC's (AnimationHeader, in its object): s16 frame count, u32 frame
    data, u32 joint indices, u16 static index max; an index below it is a
    fixed value, at or above it the value at that index plus the frame.
  - The player's (link_animetion, file 7 in the USA ROM): each frame its
    joints one after another as s16s, then a u16 for the face.
"""

import struct


def u32(d, o):
    return struct.unpack('>I', d[o:o + 4])[0]


class Skeleton:
    def __init__(self, obj, header, lod=False):
        table = u32(obj, header) & 0xFFFFFF
        count = obj[header + 4]
        size = 16 if lod else 12
        self.limbs = []
        for i in range(count):
            p = u32(obj, table + i * 4) & 0xFFFFFF
            x, y, z, child, sibling, dl = struct.unpack('>hhhBBI', obj[p:p + 12])
            self.limbs.append({'pos': (x, y, z), 'child': child, 'sibling': sibling, 'dl': dl})
        # Drawing order and each drawing limb's slot in segment 13.
        self.order = []
        slot = [0]

        def walk(i):
            if i == 0xFF:
                return
            limb = self.limbs[i]
            limb['slot'] = -1
            if limb['dl']:
                limb['slot'] = slot[0]
                slot[0] += 1
            self.order.append(i)
            walk(limb['child'])
            walk(limb['sibling'])

        walk(0)
        self.slots = slot[0]

    def table(self):
        """The limbs for the game: s16 x, y, z, u8 child, u8 sibling, s8 slot,
        pad -- 10 bytes each, little-endian (read by the host, not the N64)."""
        b = bytearray()
        for limb in self.limbs:
            b += struct.pack('<hhhBBbx', *limb['pos'], limb['child'], limb['sibling'], limb['slot'])
        return bytes(b)


def npc_frames(obj, header, joints):
    """An NPC animation's frames: [[(x, y, z) * joints]]."""
    frames, _pad, data, indices, static_max = struct.unpack('>hhIIH', obj[header:header + 14])
    data &= 0xFFFFFF
    indices &= 0xFFFFFF
    idx = [struct.unpack('>3H', obj[indices + j * 6:indices + j * 6 + 6]) for j in range(joints)]
    out = []
    for f in range(frames):
        row = []
        for triple in idx:
            row.append(tuple(struct.unpack('>h', obj[data + (i + (f if i >= static_max else 0)) * 2:][:2])[0] for i in triple))
        out.append(row)
    return out


def player_frames(anim_file, offset, frames, joints=22):
    """A player animation's frames, from link_animetion."""
    out = []
    step = joints * 6 + 2
    for f in range(frames):
        at = offset + f * step
        out.append([struct.unpack('>3h', anim_file[at + j * 6:at + j * 6 + 6]) for j in range(joints)])
    return out


def frames_blob(frames):
    """Frames for the game: s16s, little-endian, frame after frame."""
    b = bytearray()
    for row in frames:
        for x, y, z in row:
            b += struct.pack('<3h', x, y, z)
    return bytes(b)

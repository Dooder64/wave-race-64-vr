#!/usr/bin/env python3
"""Reading Majora's Mask (USA) from the player's own ROM.

The ROM's file table (dmadata, at 0x1A500 in the USA ROM) lists each file's
place in the virtual ROM and in the cartridge: (vrom start, vrom end, rom
start, rom end), rom end 0 for an uncompressed file and rom start
0xFFFFFFFF for a missing one. Compressed files are Yaz0. Nothing of the
other game is kept in this repository: everything is read from the player's
ROM when the tools run (docs/GUEST_COURSES.md).
"""

import struct

DMADATA = 0x1A500


def yaz0(data):
    """Decompresses a Yaz0 block (magic, decompressed size, 8 reserved)."""
    if data[:4] != b'Yaz0':
        raise ValueError('not Yaz0')
    size = struct.unpack('>I', data[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = data[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(data[i])
                i += 1
            else:
                b1, b2 = data[i], data[i + 1]
                i += 2
                back = ((b1 & 0x0F) << 8 | b2) + 1
                count = b1 >> 4
                if count == 0:
                    count = data[i] + 0x12
                    i += 1
                else:
                    count += 2
                start = len(out) - back
                for k in range(count):
                    out.append(out[start + k])
    return bytes(out)


class Rom:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        if self.data[0x20:0x34] != b"ZELDA MAJORA'S MASK ":
            raise ValueError("not a Majora's Mask ROM (big-endian .z64)")
        self.files = []
        off = DMADATA
        while True:
            vstart, vend, pstart, pend = struct.unpack('>4I', self.data[off:off + 16])
            if vstart == 0 and vend == 0 and off > DMADATA:
                break
            self.files.append((vstart, vend, pstart, pend))
            off += 16

    def file(self, index):
        """The file's bytes, decompressed; None for a missing one."""
        vstart, vend, pstart, pend = self.files[index]
        if pstart == 0xFFFFFFFF or vend <= vstart:
            return None
        if pend == 0:
            return self.data[pstart:pstart + (vend - vstart)]
        return yaz0(self.data[pstart:pend])

    def file_at_vrom(self, vrom):
        for i, (vstart, vend, pstart, pend) in enumerate(self.files):
            if vstart == vrom:
                return i
        return None

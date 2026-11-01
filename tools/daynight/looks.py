"""Prints the nine courses' looks (0x800D9964 + course*0x110) from the ROM.

Usage: python looks.py <Wave Race 64 USA Rev A .z64>
"""
import struct, sys

ROM_OFFSET = 0x80045800        # main segment: vram - this = ROM offset
LOOKS, SIZE, SKIES = 0x800D9964, 0x110, 0x800DC0E4
NAMES = ["Dolphin Park", "Sunny Beach", "Sunset Bay", "Drake Lake", "Marine Fortress",
         "Port Blue", "Twilight City", "Glacier Coast", "Southern Island"]

rom = open(sys.argv[1], "rb").read()
for c, name in enumerate(NAMES):
    at = LOOKS - ROM_OFFSET + c * SIZE
    words = struct.unpack(">68I", rom[at:at + SIZE])
    sky = struct.unpack(">I", rom[SKIES - ROM_OFFSET + c * 4:][:4])[0]
    print(f"== {c} {name}  sky list {sky:08X}")
    for i in range(0, 68, 4):
        row = words[i:i + 4]
        fl = [struct.unpack(">f", struct.pack(">I", w))[0] for w in row]
        print(f"  +{i*4:03X}  " + " ".join(f"{w:08X}" for w in row) + "   " +
              " ".join(f"{f:10.3f}" if 1e-4 < abs(f) < 1e7 else f"{w:10d}" if w < 0x10000 else "         ?" for f, w in zip(fl, row)))

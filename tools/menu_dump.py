#!/usr/bin/env python3
"""Print the main menu's page table and the menu lettering, read from the ROM.

What it reads is described in docs/MENUS.md. The ROM must be Wave Race 64 USA
Rev 1 (Rev A); nothing is written, and nothing from the ROM is saved.

    python3 tools/menu_dump.py ROM pages          the main menu's pages and choices
    python3 tools/menu_dump.py ROM words [FONT]   the word pictures always loaded
    python3 tools/menu_dump.py ROM letters [FONT] the string fonts' letters

Pictures are printed as text: '#' bright and solid, '+' solid but dark
(outline), blank see-through.
"""
from __future__ import annotations

import struct
import sys

# Where things are in the ROM (USA Rev 1).
CODESEG_ROM, CODESEG_VRAM = 0xA95D0, 0x801DAFA0      # loaded uncompressed
TITLE_OVERLAY_ROM, OVERLAY_VRAM = 0x1B3EC0, 0x802C5800  # ovl_i0: title + main menu
SEGMENT_1_ROM = 0xF6090                              # the shared pictures

PAGES, PAGE_COUNT = 0x802C6E08, 6

# func_801E3EE0's word-picture tables, by its font argument: table, height.
WORD_FONTS = {0: (0x802263C0, 8), 1: (0x80226568, 10), 2: (0x80226A48, 18),
              3: (0x80226890, 14), 4: (0x802264F8, 10)}
# func_801E80B0's letters, by func_801E9858's font argument: picture, size, count.
LETTER_FONTS = {0: (0x01033CB8, 24, 20, 40), 1: (0x010387C0, 8, 8, 42), 2: (0x01039248, 16, 12, 46)}

# What some word pictures say (font 1), for the page listing.
WORDS = {0x16: "NORMAL", 0x17: "HARD", 0x18: "EXPERT", 0x45: "START THE RACE", 0x59: "REVERSE",
         0x12: "CHAMPIONSHIP*", 0x13: "TIME TRIALS*", 0x3B: "STUNT MODE*", 0x14: "2P VS*",
         0x15: "OPTIONS*", 0x46: "WARM UP*"}
MODES = {-1: "-", 0: "Time Trials", 1: "2P VS", 4: "Championship", 5: "Options", 11: "Stunt Mode"}
KINDS = {0: "", 1: "remembers Warm Up", 2: "skips Warm Up", 3: "to page 5 once Reverse is open",
         4: "Reverse"}


def word(rom: bytes, vram: int) -> int:
    if vram >= OVERLAY_VRAM:
        at = TITLE_OVERLAY_ROM + vram - OVERLAY_VRAM
    else:
        at = CODESEG_ROM + vram - CODESEG_VRAM
    return struct.unpack(">I", rom[at:at + 4])[0]


def signed16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def pages(rom: bytes) -> None:
    for p in range(PAGE_COUNT):
        at = PAGES + p * 0x18
        items, rows, choices, x, y, flag = (word(rom, at + 4 * i) for i in range(6))
        print(f"page {p}: {rows} rows, {choices} to choose from"
              f"{' (only the difficulties opened)' if flag else ''}, words at x {x}+, rows from y {y}")
        for i in range(rows):
            e = items + i * 0x18
            nxt = signed16(word(rom, e) >> 16)
            mode = word(rom, e + 4)
            mode = mode - 0x1_0000_0000 if mode & 0x8000_0000 else mode
            text, dx = word(rom, e + 8), word(rom, e + 12)
            players, riders = word(rom, e + 16) >> 16, word(rom, e + 16) & 0xFFFF
            difficulty, kind = word(rom, e + 20) >> 16, word(rom, e + 20) & 0xFFFF
            goes = f"page {nxt}" if nxt >= 0 else "start"
            print(f"   {i}: word {text:#04x} {WORDS.get(text, '?'):15} x+{dx:<3} -> {goes:7}"
                  f" mode {MODES.get(mode, mode):12} players {players} riders {riders}"
                  f" difficulty {difficulty} {KINDS.get(kind, kind)}")
    print("* drawn from segment 8 (loaded with the title screen), not segment 1")


def picture(rom: bytes, address: int, width: int, height: int, first: int = 0) -> list[str]:
    base = SEGMENT_1_ROM + (address & 0xFFFFFF) + first
    rows = []
    for y in range(height):
        row = rom[base + y * width: base + (y + 1) * width]
        rows.append("".join("#" if (b >> 4) > 8 and (b & 0xF) > 6 else "+" if (b & 0xF) > 6 else " "
                            for b in row).rstrip())
    return rows


def words(rom: bytes, fonts: list[int]) -> None:
    ends = sorted(t for t, _ in WORD_FONTS.values()) + [0x80226B80]
    for font in fonts:
        table, height = WORD_FONTS[font]
        end = 0x80226B80 if font == 1 else ends[ends.index(table) + 1]
        print(f"font {font}: table {table:#x}, {height} rows tall")
        for i in range((end - table) // 8):
            address, width = word(rom, table + 8 * i), word(rom, table + 8 * i + 4)
            if address == 0 or width == 0:
                continue
            if address >> 24 != 1:
                print(f"  id {i:#04x}: {address:08X} {width} wide (segment {address >> 24}, loaded per screen)")
                continue
            print(f"  id {i:#04x}: {address:08X} {width} wide")
            for row in picture(rom, address, width, height):
                if row:
                    print("     " + row)


def letters(rom: bytes, fonts: list[int]) -> None:
    for font in fonts:
        address, width, height, count = LETTER_FONTS[font]
        print(f"font {font}: {address:08X}, {width}x{height}, {count} letters "
              "(0-9 are 0-9, A-Z are 10-35)")
        per_line = max(1, 150 // (width + 1))
        for start in range(0, count, per_line):
            glyphs = range(start, min(count, start + per_line))
            print("   " + "".join(f"{g:<{width + 1}}" for g in glyphs))
            drawn = [picture(rom, address, width, height, g * width * height) for g in glyphs]
            for y in range(height):
                print("   " + "".join(f"{d[y]:<{width}}|" for d in drawn))


def main() -> None:
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rom = open(sys.argv[1], "rb").read()
    what = sys.argv[2]
    fonts = [int(f) for f in sys.argv[3:]]
    if what == "pages":
        pages(rom)
    elif what == "words":
        words(rom, fonts or sorted(WORD_FONTS))
    elif what == "letters":
        letters(rom, fonts or sorted(LETTER_FONTS))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Let the settings' rows of choices, and its tabs, wrap onto more lines.

A row of choices (an enum option: the Free Roam tab's sky, music and waves)
lays its choices side by side and, with more than fit, squeezes them: the
words broke over two lines and ran into each other ("STORMYWILD"). The user
found the settings hard to read. Each choice now keeps its width and its
words on one line, and the row wraps onto as many lines as it needs. The
tabs likewise: on a 4:3 screen (a user's) they were squeezed until their
names ran together ("GeneralHaptics") and "Free Roam" broke in two; now
they keep their names whole and go onto a second row.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "lib/RecompFrontend/recompui/src/elements/ui_radio.cpp"
TABS = ROOT / "lib/RecompFrontend/recompui/src/elements/ui_tab_set.cpp"

PIECES = [
    ("a choice keeps its width and its words on one line",
     """        set_text_transform(TextTransform::Uppercase);
        set_height_auto();
""",
     """        set_text_transform(TextTransform::Uppercase);
        set_height_auto();
        // wr64: kept whole, the row wrapping instead (tools/patch_frontend_layout.py).
        set_flex_shrink(0.0f);
        set_white_space(WhiteSpace::Nowrap);
"""),
    ("a row of choices wraps",
     """        set_gap(24.0f);
        set_align_items(AlignItems::FlexStart);
""",
     """        set_gap(24.0f);
        set_align_items(AlignItems::FlexStart);
        // wr64: onto more lines rather than squeezed, across the whole width
        // (left to size itself, it wrapped short rows too) (tools/patch_frontend_layout.py).
        set_flex_wrap(FlexWrap::Wrap);
        set_width(100.0f, Unit::Percent);
"""),
]


TAB_PIECES = [
    ("a tab keeps its name whole",
     """    set_tab_index_auto();
    set_cursor(Cursor::Pointer);
""",
     """    set_tab_index_auto();
    set_cursor(Cursor::Pointer);
    // wr64: kept whole, the tabs wrapping instead (tools/patch_frontend_layout.py).
    set_flex_shrink(0.0f);
    set_white_space(WhiteSpace::Nowrap);
"""),
    ("the tabs wrap",
     """    set_gap(0.0f);
    set_as_navigation_container(NavigationType::Horizontal);
""",
     """    set_gap(0.0f);
    // wr64: onto a second row rather than squeezed (tools/patch_frontend_layout.py).
    set_flex_wrap(FlexWrap::Wrap);
    set_as_navigation_container(NavigationType::Horizontal);
"""),
]


def apply(target, pieces):
    if not target.is_file():
        raise SystemExit(f"Missing {target}; run git submodule update --init --recursive")
    source = target.read_text()
    patched = source
    for name, anchor, replacement in pieces:
        if replacement in patched:
            print(f"  {target.name}: {name} already patched")
            continue
        if patched.count(anchor) != 1:
            raise SystemExit(f"{target}: anchor for '{name}' changed; inspect the pinned frontend")
        patched = patched.replace(anchor, replacement, 1)
        print(f"  {target.name}: {name} patched")
    if patched != source:
        target.write_text(patched)


def main() -> None:
    apply(TARGET, PIECES)
    apply(TABS, TAB_PIECES)


if __name__ == "__main__":
    main()

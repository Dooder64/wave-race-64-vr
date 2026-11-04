# The game's menus

Research for the v1.1.0-vr checklist, section 1 ("customization in the game's
own menus"): how Wave Race 64 builds its menu screens, so new pages can be
added that look and behave like the game's own. Everything here is for the
USA Rev 1 (Rev A) ROM. Addresses are the game's (KSEG0); ROM offsets are
marked "ROM".

How it was found: the decomp in `reference/wr64-decomp` (mostly undecompiled
assembly for these parts), the ROM's data read directly, and the decompiled
pause menu (`src/codeseg/wr64_pause_menu.c` in the decomp), which uses the same
drawing calls in readable C. `tools/menu_dump.py` prints the page table, every
word picture and the letter fonts from the ROM, which is how the tables below
were checked.

## In short

- The title screen and the main menu are one game state (`0x03`) in one
  overlay (`ovl_i0`, ROM `0x1B3EC0`, loaded at `0x802C5800`).
- The main menu is **table driven**: six pages, each a list of choices. A
  choice either opens another page or ends the menu with a mode, player count,
  rider count and difficulty. Up/down moves, A/Z/Start chooses, B goes back a
  page. New pages can be added to the same table.
- Menu words are **pictures**, not text: each word ("NORMAL", "START THE
  RACE") is a small image looked up by number. About 60 are always loaded;
  the mode names are loaded with the title screen only.
- The game also has **three real alphabets** (A-Z and 0-9) always loaded, used
  by a string-drawing function. They are italic, in the HUD's style, not the
  upright lettering of the menu words.
- The game's own menu bars, colours, highlight and sounds are all reusable
  calls.

## Game states on the way into a race

`gGameState` is at `0x800DAB24` (see RESEARCH.md for the full list).

| State | Screen | Overlay |
|---|---|---|
| `0x02` | Title screen | `ovl_i0` |
| `0x03` | Title "press start" and the main menu | `ovl_i0` |
| `0x04` | Fading out of the main menu; `D_801CE630` holds where to next | `ovl_i0` |
| `0x0A` | Rider select | `ovl_i2` |
| `0x14` | Course select (Time Trials and Stunt Mode choose difficulty here) | `ovl_i4` |
| `0x1E` | Course overview card | `ovl_i3` |
| `0x28` | The race | `ovl_i1` |
| `0x3C` | Options menu (its pages are `0x3E`-`0x48`) | `ovl_i7` |

Every state change is the same few lines: `gPrevGameState = gGameState`, the
new state, `D_801CE63C = 1` (the new screen's "first frame" flag, cleared by
its draw function), and the frame divider `0x800D461C = 3` (menus run at
20 fps). Leaving the main menu is `func_i0_802C6A1C(next)`, which sets state
`0x04`, starts a fade (`FadeTransition_SetProps(1, 4, 0)`) and the next
screen's music.

## The main menu

The functions (all `ovl_i0`; the recompiled C names are the same, e.g.
`func_i0_802C63AC` in `RecompiledFuncs/`):

| Function | Does |
|---|---|
| `func_i0_802C5A7C` | State `0x03` each frame. Phase `D_i0_802C6BE0`: 0 = the "press start" words (word font 1 ids `0x58` and `0x56`, loaded with the title, one blinking), 1 = the menu. Draws, then calls the input handler. Also starts the attract demo after 600 frames (`0x258`) of no input. |
| `func_i0_802C6044` | Draws the current page. |
| `func_i0_802C63AC` | Reads the controller and moves through the pages. |
| `func_i0_802C6878` | Title -> main menu (state `0x03`, sound `0x2D`). |
| `func_i0_802C6A1C(next)` | Main menu -> `next` (state `0x04`). |

### Its variables

| Address | Meaning |
|---|---|
| `D_802C6EE0` | Current page (0-5). |
| `D_802C6EE4` | Cursor: the row chosen on the page. |
| `D_802C6F0C` | Rows on the page (bars drawn). |
| `D_802C6F10` | Rows that can be chosen (fewer when difficulties are locked). |
| `D_i0_802C6BCC` | How many pages deep (0-2; the handler stops counting at 2). |
| `D_802C6EE8[4]`, `D_802C6EF8[4]` | The cursor and page at each depth, so B can go back. |
| `D_802C6F08` | Difficulties opened: 1 + Hard + Expert, from the save's bits in `0x801CB280` (bit 0 Hard, bit 1 Expert, bit 3 Reverse). |
| `D_i0_802C6BC0` | Reverse opened (bit 3 above). |
| `D_802C6F14` | Frames without input (600 -> attract demo). |
| `D_800DA9D8` | The main page's cursor, remembered between visits. |

### The page table

Six pages at `0x802C6E08`, 0x18 bytes each:

| Offset | Meaning |
|---|---|
| `+0x00` | Pointer to the page's choices. |
| `+0x04` | Rows drawn. |
| `+0x08` | Rows that can be chosen. |
| `+0x0C` | Left edge for the words (98). |
| `+0x10` | Top row's y (78); rows are 20 pixels apart. |
| `+0x14` | 1 = a difficulty page: only the opened difficulties can be chosen (`D_802C6F08`), the rest drawn grey. |

Each choice is 0x18 bytes:

| Offset | Size | Meaning |
|---|---|---|
| `+0x00` | s16 | Page to open, or `-1` to end the menu with this choice. |
| `+0x04` | s32 | Mode: `0` Time Trials, `1` 2P VS, `4` Championship, `5` Options, `11` Stunt Mode. Copied to `D_801CE608.gameMode`. |
| `+0x08` | s32 | Word picture (word font 1). |
| `+0x0C` | s32 | The word's x, from the page's left edge (centres it in the bar). |
| `+0x10` | s16 | Human players, to `D_801CE608 + 4`. |
| `+0x12` | s16 | Riders in the race, to `D_801CE608 + 6` (4 in a Championship race, 1 in Warm Up). |
| `+0x14` | s16 | Difficulty, to `gDifficulty` (`0x801CB338`). |
| `+0x16` | s16 | Special: `1` the next page's cursor starts on its row 1 when `D_800DAA00 > 0` (Normal -> START THE RACE, likely once Warm Up has been ridden); `2` skip Warm Up (`D_801CB334 = 1`); `3` go to page 5 instead once Reverse is open; `4` Reverse (`D_800DAB68 = 1`). |

The pages as the ROM has them (`tools/menu_dump.py ROM pages`):

| Page | Rows | Goes to |
|---|---|---|
| 0 main | CHAMPIONSHIP, TIME TRIALS, STUNT MODE, 2P VS, OPTIONS | Championship -> page 1 (page 5 once Reverse is open); the others end the menu |
| 1 Championship difficulty | NORMAL, HARD, EXPERT | Normal -> page 2; Hard and Expert end it |
| 2 after Normal | WARM UP, START THE RACE | both end it |
| 3 (unused) | NORMAL, HARD, EXPERT for Time Trials | nothing leads here |
| 4 (unused) | NORMAL, HARD, EXPERT for 2P VS | nothing leads here |
| 5 Championship difficulty, Reverse open | NORMAL, HARD, EXPERT, REVERSE | Normal -> page 2 |

Pages 3 and 4 are leftovers: no choice points at them, so in the finished
game Time Trials and 2P VS pick difficulty on the course select screen
instead.

Where the menu goes once a choice ends it (by mode, a jump table at
`0x802C6EB0`): Championship, Time Trials, Stunt Mode and 2P VS go to rider
select (`0x0A`), Options to `0x3C`, anything else back to the title. Each
mode sets up its own race state first (Championship clears
`0x801CB340`-`0x801CB3D0` and sets `0x801CB330` from a table at `0x800EAC14`
indexed by difficulty; what those hold is unchecked).

### The controls

From `func_i0_802C63AC`, reading `D_801CE65A` (controller 1's buttons pressed
this frame; `D_801CE658[n].unk_00.unk_2` in the pause menu's C):

| Button | Does | Sound |
|---|---|---|
| A, Z or Start (`0xB000`) | Choose: opens the next page (cursor at 0) or ends the menu | `0x11` for a page, `0x37` for the end |
| B (`0x4000`) | Back a page, restoring its cursor | `0x16` |
| Up (`0x0800`) | Cursor up, wrapping | `0x10` |
| Down (`0x0400`) | Cursor down, wrapping | `0x10` |

There is no left/right; the main menu never needed it. The stick moves the
cursor too (seen in earlier scripted runs: a stick press moves WARM UP to
START THE RACE), so the game's controller code turns it into the same bits,
and the port's gamepad, keyboard and Touch controller mappings already work
here.

Sounds are `AudioGeneral_PlaySound(id, 0)`. The ones menus use: `0x10` move,
`0x11` choose/OK, `0x16` back, `0x37` start (leaving the main menu), `0x20`
pause opens, `0x2D` title -> menu, `0x31` title chime.

## Drawing a menu

All of these are in code that is always loaded, take the display list and
return it, and are used the same way by the main menu and the pause menu.

| Call | Draws |
|---|---|
| `func_80094338(gfx, 0, x, y, w, h)` | One of the menu's bars. The main menu uses x 90, w 140, h 15, at each row's y - 2. |
| `gSPDisplayList(D_106F628)` | The bars' render setup, before them. |
| `gSPDisplayList(D_106F168)` | Setup after them; then a fill rectangle per row, x 89-231, y-3 to y+14 (the bars' backing). |
| `func_801E946C(gfx, style, 12 x 0xFF)` | Sets the colour for the next words. Style `0` normal, `6` the chosen row (highlight), `7` greyed (a locked difficulty). The menus always pass 255 for the twelve numbers (probably tints). |
| `func_801E3EE0(gfx, font, id, x, y, 0)` | A word picture. |
| `func_801E5554(gfx)` | Ends a run of words. |
| `func_801E9858(gfx, font, "TEXT", x, y, ...)` | A string in one of the letter fonts. The title uses it for a hidden memory readout. |
| `func_801E7C58(gfx, x0, y0, x1, y1, r, g, b, a)` | A see-through box (the pause menu's dark backdrop). |

The page draw is: the setup list, a bar per row, then every row but the
chosen one in style 0 (or 7 if locked), then the chosen row in style 6, then
`func_801E5554`.

### Word pictures

`func_801E3EE0` picks a table by its font argument and looks up an 8-byte
entry `{ picture address, width }` by the id. Pictures are IA8 (grey and
transparency, a byte a pixel), drawn as texture rectangles.

| Font | Table | Height | Used for |
|---|---|---|---|
| 0 | `0x802263C0` | 8 | Small labels: NORMAL, HARD, EXPERT, BEST LAP, BEST TOTAL, POINTS, REVERSE |
| 1 | `0x80226568` | 10 | Menu words: the main menu, pause menu, course names, results |
| 2 | `0x80226A48` | 18 | Big course names on the course cards |
| 3 | `0x80226890` | 14 | Long sentences (help text), all loaded per screen |
| 4 | `0x802264F8` | 10 | A few |

Pictures with addresses `0x01xxxxxx` are in segment 1, the shared pictures,
which are always in memory (ROM `0xF6090` onwards, uncompressed; in RDRAM at
`0x228E10`). Those with `0x08xxxxxx` are loaded with a particular screen, so
they exist only while that screen is up. The main menu's mode names and
WARM UP are in that second group.

Font 1 words always available (segment 1), by id: TIME `0x03`, EXECUTE
`0x04`, CANCEL `0x05`, YOUR POINTS `0x07`, the nine course names `0x08`-`0x10`,
LEVEL `0x11`, NORMAL `0x16`, HARD `0x17`, EXPERT `0x18`, RANK `0x1F`, RIDER
`0x20`, RETRY `0x2C`, COURSE SELECT `0x2E`, QUIT GAME `0x2F`, CONTINUE
`0x30`, RETIRE `0x31`, RESTART `0x32`, ON `0x37`, OFF `0x38`, POINTS `0x3A`,
SCORE `0x3E`, END `0x3F`, START THE RACE `0x45`, ROUND 1-8 `0x47`-`0x4E`,
TO ADVANCE `0x4F`, FINAL ROUND `0x57`, REVERSE `0x59`, a dash `0x54`, and a
few longer phrases (`tools/menu_dump.py ROM words 1` prints them all).

Not there, and needed for section 1: LAPS, FREE ROAM, BUOYS, RINGS, RIDERS,
WAVES, SKY, WATER, MUSIC, FOG, KM/H, MPH, numbers for a lap count. Several
table entries are empty (e.g. font 1 ids `0x28`, `0x2B`, `0x42`, `0x43`,
`0x5A`, `0x5B`, `0x61`, `0x65`), so new pictures could be given ids of their
own without moving anything.

### Letter fonts

`func_801E9858` draws a string a letter at a time through `func_801E80B0`;
`func_801E9B04` turns a character into a letter number (`0`-`9` -> 0-9,
`A`-`Z` -> 10-35, plus `-`, `'`, `/` in some fonts). All three alphabets are
in segment 1:

| Font | Pictures | Letter size | Letters | Look |
|---|---|---|---|---|
| 0 | `0x01033CB8` | 24x20 | 40 | Big, italic |
| 1 | `0x010387C0` | 8x8 | 42 | Tiny |
| 2 | `0x01039248` | 16x12 | 46 | Medium, italic |

Fonts 3-5 are digits only (`0x0103B4D0` 12x16 and `0x0103D8D8` 16x20, the
HUD's numbers, RGBA32).

## What this means for section 1

**Recommended approach: new pages in the game's own menu flow.** The main
menu already does most of the work: its table holds pages, it remembers the
way back for B, it plays the right sounds and fades to rider select. New
choices on the existing pages are just new table rows; a new page is a new
table entry. Everything drawn is the game's own bars, colours and
highlight, so it looks native, and it goes onto the VR panel like every other
menu with no VR work.

What the game cannot do and would be new code (a patch replacing
`func_i0_802C63AC` and `func_i0_802C6044` for our pages only, calling the
originals for the game's):

- **Left/right to change a value** (ON/OFF, a lap count). The handler only
  knows up, down, choose and back.
- **More rows than fit.** A page fits about seven rows between y 78 and the
  bottom of the screen (rows are 20 apart). The list proposed for section 1 is
  about eleven, so it needs two pages, or tighter rows.
- **A fourth page deep.** Championship -> Normal -> Warm Up is already two
  deep, and the handler stops counting at 2. The arrays have room for four,
  so the cap only needs raising in our version.
- **Words the game doesn't have.** Two ways: draw new word pictures in the
  menu words' own style, as `src/speed_units.cpp` already does for "mph" (a
  pixel mask coloured from the game's own picture), and give them empty ids
  in the font 1 table; or spell them with letter font 2, which is quicker but
  italic and visibly different from the words around it. The first matches;
  the second is a fallback.
- **Remembering the choices.** Saved in the port's own settings file, as the
  Esc menu's are, not in the game's save.

The other approach, drawing our own screens on top of a menu state, means
rebuilding the input, sounds, fades and back-tracking the game already has,
for no gain in looks, so it isn't recommended.

## The course select

Game state `0x14`, overlay `ovl_i4` (ROM `0x1BE0B0`, loaded at `0x802C5800`;
partly decompiled in `src/overlays/ovl_i4/ovl_1BE0B0.c`).

| Function | Does |
|---|---|
| `func_i4_802C5B78` | The frame: the water backdrop, the screen (`func_i4_802C5CDC`), then, in state `0x14`, the controls. |
| `func_i4_802C6BAC` | The controls, by step `D_i4_802C7538`: 0 the course list (`func_i4_802C6C2C`), 1 the difficulty box (`func_i4_802C6ED0`), 2 `func_i4_802C70C0`, 3 a left/right ON/OFF choice (`func_i4_802C7344`; 2P VS's HANDICAP, `D_800DAA04`). |
| `func_i4_802C7474(next)` | Leaves the screen for state `next` (state `0x15` while it fades). `0x28` starts the race; every way into a race from the screen goes through it, followed straight away by the "start" chime, sound `0x37`. |

Stunt Mode starts the race from the course list; Time Trials and 2P VS go
through the difficulty box first (2P VS then HANDICAP). The controls read
controller `D_802C76B0` (`D_801CE65A` + 0x1C each). The chosen course is
`D_800DAAD8[D_800DA9D0]`. Stunt Mode's list holds all nine courses, Dolphin
Park first.

## The mod's pages

`src/game_menus.cpp`; the functions above are swapped in `src/overlays.cpp`
by editing the generated section table (the overlays all load at
`0x802C5800`, so ovl_i0 and ovl_i4 are told apart by their ROM addresses:
ovl_i0 offsets `0x844` draw and `0xBAC` controls; ovl_i4 `0x378` frame,
`0x13AC` controls, `0x1C74` way out). The hooks call the originals for
everything of the game's own. Nothing is regenerated.

### GUEST COURSES on the course select

In a free roam, down from the last course (`D_800DA9D0` 8, the list's step
`D_i4_802C7538` 0) opens GUEST COURSES: the mod's page over a dark box, its
title, GREAT BAY and BACK, and the guest's picture (for now a blank square)
over the course model's box (`func_801E7C58` at 150,54 to 292,159). Up from
its first row, BACK or B return to the list. GREAT BAY chooses the guest for
the next free roam (`guest::choose`, src/guest_course.cpp), puts the row on
Dolphin Park, its base course, and opens the Free Roam options without
LAYOUT; B there, or arriving on the course select again, unchooses it.
While the course list has the cursor, a small green arrow, pointing down
and bobbing, left of SOUTHERN ISLAND's bar says there is more below.

### FREE ROAM on the main menu

The main menu's first page gets a sixth row, FREE ROAM, above OPTIONS: the
page's table entry is pointed at a rebuilt list of its choices at physical
`0x6F7000` (the game's five with a copy of STUNT MODE's put in at row 4, its
word and x changed), rewritten every frame since the overlay is loaded
afresh with the title. Whichever row ends the menu says whether the run is a
free roam (`set_free_roam`, `patches/free_roam.cpp`). A free roam is a Stunt
Mode run, on any of the nine courses.

Stunt Mode has no race buoys; its course has the marker buoy rows
(`0x0102CD78`/`0x0102CD90`), CHECK POINT gates (`0x01018AA8` banner,
`0x01018D58` posts) and a buoy by each (`0x0102CB78`), found from a traced
Sunny Beach frame and by leaving lists out of a scripted run. A free roam
always leaves all of those out, and the rings, since buoys and rings are
what Time Trials and Stunt Mode are for.

### WAVES and the Free Roam options

Each run's last menu choice opens a page of the mod's own:

| Mode | Where | Page |
|---|---|---|
| Championship | Main menu, after the difficulty (or WARM UP / START THE RACE): any choice that ends the menu with mode 4 | WAVES, once for the whole championship |
| Time Trials, 2P VS | Course select, after the difficulty (and HANDICAP) | WAVES |
| Stunt Mode | Course select, after the course | WAVES |
| Free Roam | Course select, after the course | WAVES, SKY (and water), MUSIC, FOG, RIDERS; on Dolphin Park, LAYOUT first |

Every page ends with CONTINUE; B goes back to where it was opened from. On
the main menu the page is drawn in the game's page's place and CONTINUE runs
the game's controls with the cursor put back on the row it was opened from.
On the course select the page is opened by catching `func_i4_802C7474(0x28)`;
the chime after it is left out once (`swallow_sound_once`, through the hook on
`AudioGeneral_PlaySound`), the screen's own controls are not run while the
page is up, the page is drawn after the screen over a dark box with a solid
strip behind each bar, and CONTINUE makes the call itself and plays the chime.

The wave level is applied where the water is set up (the hook on
`func_8004F9E0`) and held every race frame, since the race sets it again as
it goes. Levels: the course's own, then 0 (Still), 10, 20, 30, 40 (the
game's roughest), 60, 80, 100. Sky, music and fog apply to free roams only.
The values are the settings' Races and Free Roam tabs'.

How the pages are built:

- Game functions are called from C++ with a stack frame of their own: four
  register arguments, the rest at `sp + 0x10` onwards.
- The markers either side of the main menu's chosen row follow `D_802C6EE4`,
  so a main menu page keeps it on its own row while open and puts it back after.
- New words are cut from the game's lettering at run time: each letter's
  bright core (found between see-through columns), plus its dark outline
  column either side. Flat letters span rows 1-9 and round ones (O, S, C, G)
  rows 0-9; HARD, EXPERT, ON, END, RETRY, RETIRE, EXECUTE and CANCEL use a
  taller cut. W is NORMAL's M upside down; X is EXPERT's with a row taken out
  of its middle; H is SOUTHERN ISLAND's (the courses' names are mostly a
  thinner cut, but its H matches). The pictures sit at physical `0x6F0000`,
  the cartridge's letters are read to `0x6F8000`, both free below the
  rewriter's scratch list at `0x700000`.
- FREE ROAM, drawn by the game's own main page, takes the empty font 1 id
  `0x28`. Every other made word is drawn through id `0x2B`, its table entry
  rewritten before each word: `func_801E3EE0` copies the address into the
  display list as it draws. (Font 1's table runs on into font 3's at id `0x65`.)
- "The course's own" is the game's NORMAL; the courses' names, ON and OFF are
  the game's own words.
- Verified flat with scripted runs of every mode: each page opens where
  listed, values change and save, B goes back, CONTINUE starts the run, and
  the log shows the chosen wave level replacing the course's (e.g. Sunny
  Beach's 20 as 10, 100 or 0; 2P VS's 15 as 80).
- HD: the texture pack replaces word pictures by their RT64 hash, so each
  made word has its own 4x replacement in the bundled pack (46 entries with
  OFF and REVERSE, which the pack lacked). They were made by capturing the
  menus with `WR64_TEXTURE_DUMP`, decoding with `tools/textures/decode_tmem.py`,
  taking the 10-pixel-tall images the pack doesn't map, and redrawing each
  whole word in Arial Bold fitted to the native word's ink
  (`tools/textures/refine_fonts.py`'s `vector_font`, without per-letter
  placement), then adding them to `rt64.json` and `manifest.json`
  (`tools/bundled_assets.py` validates). A word's hash is its pixels: change
  a letter, its spacing or a word, and its replacement must be made again.

### Dolphin Park's Warm Up as a free roam

The championship's Warm Up is not a layout of its own but a Championship run
with one rider at Dolphin Park: it starts by the beach, with a dolphin to
follow, a help ticker along the bottom, the speed alone on the HUD, and no
end. Stunt Mode's Dolphin Park starts out among its rings.

| What | Where |
|---|---|
| The menus' choice, `D_801CE608` | `+0x0` mode, `+0x4` players (s16), `+0x6` riders (s16); copied to `gGameModes` (`0x801CE620`), `gPlayers` and `gRiders` as a race sets up (`0x80093644`) |
| Warm Up skipped | `D_801CB334` (word) |
| Difficulty | `gDifficulty`, `0x801CB338` (word) |
| The course select's row, and each row's course | `D_800DA9D0`; `D_800DAAD8[row]`. `gCourseID` is set from them only after the screen fades out (`0x802C5C7C`). |
| The course select's setup | `func_i4_802C5800` (offset `0x0`); the next state, `0x15`, runs it too |
| Pause menu rows | Tables in the decomp's `wr64_pause_menu.c`, `{u16 action, s16 x offset, s32 word}` each. Warm Up's (3 rows) at `0x80226BD0`: CONTINUE, START THE RACE (action 6, word `0x45`), QUIT GAME. Stunt Mode's COURSE SELECT is action 5, x `-0x16`, word `0x2E`. The cursor opens on row 1 for a Championship at Dolphin Park. |
| The choice made in the pause menu | `D_801CE628` (s16): 4 quit, 6 course select, 7 start the race |

LAYOUT (STUNT MODE or WARM UP) on the Free Roam options. With WARM UP, CONTINUE
makes the menus' choice a Championship (players 1, riders 1, Normal, the Warm
Up not skipped) and sets `gGameModes` too, since the course select's way into a
race does not pass the step that copies it. The race then is the Warm Up. In a
free roam:

- The pause menu's START THE RACE row is rewritten to COURSE SELECT, and the
  cursor is put on CONTINUE as it opens. The game's own Warm Up is untouched.
- Back on the course select (its setup, in state `0x14` only), the choice and
  `gGameModes` are Stunt Mode's again, with its difficulty.
- The help ticker's rectangles (top at or below y 188) are left out, paused or
  not; it says how to start racing.
- Its music: the championship's way in starts it from the course overview,
  which this way passes by. So the first race frame starts it as the go's
  sound would (`D_800E7CD8 = 10`, with the announcer's timers), once the sound
  driver is racing.

### MUSIC previews

On the Free Roam options' MUSIC row, the music shown plays: NORMAL plays the
chosen course's own. The replacement music's recording of that course (course
+ 6 as the game numbers sequences) plays from its start, fading in over a
quarter of a second as the menu's music fades out, and the other way when the
cursor leaves the row or the page closes. It starts past the recording's
opening quiet, where a race has its countdown and the announcer: the first
quarter second within 15 dB of the loudest of its first 20 seconds (Sunny
Beach 4.5 seconds in, Dolphin Park 0.5). The page asks for it every frame,
and the mixer lets it go after a quarter of a second without asking, so it
can't be left playing. With the original music there is no preview, as the
MUSIC choice has no effect there either: another course's sequence can't be
played in place of this one's (its instruments may not fit).

Verified flat by scripted runs: the LAYOUT row shows on Dolphin Park only;
WARM UP starts by the Dolphin Park arch with the speed alone on screen and
Dolphin Park's music; its pause menu reads CONTINUE, COURSE SELECT, QUIT GAME
with the cursor on CONTINUE; COURSE SELECT returns to Stunt Mode's course
select, where the page opens again; the game's own Warm Up still offers START
THE RACE and its ticker. The log shows `preview: dolphin_park`, then
`sunny_beach`, then `ended` as the MUSIC row is changed and left.

### EXTRAS on the OPTIONS screen

The OPTIONS screen (state `0x3C`, `ovl_i7`, ROM `0x1C43F0`) is three small
functions:

| Function | Offset | What |
|---|---|---|
| `func_i7_802C5800` | `0x0` | The frame: on the first frame (`D_801CE63C`) it resets the row (`gCurrentOptionsMenuItem`, `0x800DA9D4`) when coming from the main menu; then draws, and in state `0x3C` reads controller 1: A, Z or Start leave for the row's state, B for state `2`, up and down move over 7 rows with wrap. In state `0x3D` (fading out) it waits for the fade and calls the page's setup by a jump table. |
| `func_i7_802C5A18` | `0x218` | The draw: four setup commands (`0x01030040 0x07000000`, `0x01020040 0x02000A40`, lists `0x0106F550` and `0x0106F628`), a bar a row (`func_80094338`, x `0x3A`, width `0xCC`, from y `0x49` every `0x13`), the title (font 2, id 5, at `0x6B, 0x22`), the words (font 1) at `x + 0x6D, 0x4C + 19 row`, the chosen in style 6. |
| `func_i7_802C5C94` | `0x494` | Leaves for state `a0` with a fade. |

Its rows (`D_i7_802C5D3C`, `{s16 x, s32 word}`): CHANGE NAMES `0x19`, VIEW
RECORDS `0x1A`, CHANGE CONDITIONS `0x1B`, AUDIO `0x1C`, ERASE COURSE RECORDS
`0x1D`, SAVE AND LOAD `0x1E`, END `0x3F`; their states (`D_i7_802C5D20`):
`0x3E`, `0x42`, `0x44`, `0x48`, `0x46`, `0x40`, `2`.

Calls inside an overlay go through the function table (`LOOKUP_FUNC`), so
both the frame and the draw are hooked. The mod draws eight rows (from y 68
every 18), EXTRAS before END, and runs up and down itself; A on a game row
sets the game's row and lets the game's frame act on it, so its pages work
as before. EXTRAS opens the mod's page in the rows' and title's place: SPEED
(KPH, MPH), VIEW (CHASE, FIRST PERSON, kept for a flat screen and for the
headset apart), TILT (first-person intensity NONE, HALF, FULL: 0, 50, 100%),
FEEDBACK (OFF, EVENTS, FULL), TEXTURES (ORIGINAL, HD), WATER (ORIGINAL,
MODERN, HIGH), STYLE (MODERN, CLASSIC, AQUA), RIPPLES (SOFT, NORMAL, STRONG),
SPRAY, UNLOCK ALL, then END. The values are the settings menu's, saved as
they change. Q, for AQUA, is NORMAL's O with a tail drawn on.

Mind `a0`: a hook that plays a sound (a call of the game's) before drawing
must keep the display list pointer first, or the draw writes through the
sound's number (this crashed in testing).

### MISSES and LAPS (Championship)

The game's own OPTIONS -> CHANGE CONDITIONS (state `0x44`, `ovl_i10`) keeps,
per difficulty, wave conditions, misses allowed and laps:
`D_801CB308[difficulty][3]` (words at `0x801CB308`, 12 bytes a difficulty),
saved with the game. Its page allows misses 1-5 and laps 3-9 (a min/max
table at `D_i10_802C6AF8`). As the main menu ends a Championship it copies
that difficulty's misses and laps into `D_801CE608 + 0xC / + 0x10` and sets
`D_801CE6F8`; each race's setup (`0x800933C4` on) reads them into
`gBuoyMisses` (`0x800D8174`, compared with the race record's `+0x134`) and the
laps to finish. REVERSE uses a fixed 5 and 3.

The Championship WAVES page shows MISSES (1-10) and LAPS (3-10) for the
chosen difficulty (the choice's `+0x14`), writing those values directly. The
numbers are drawn as that page draws them: `func_801E9858(gfx, 2, string, x,
y)`, the scale `D_80226E24 = 1.0` and `D_80226E28 = 0` set first.

REVERSE has its own MISSES and LAPS (the Races tab's `reverse_misses`,
`reverse_laps`, defaults 5 and 3), held each frame of a Reverse race
(`D_800DAB68` set): `gBuoyMisses` and the laps to finish
(`patches/free_roam.cpp`).

The HUD's MISS row draws one 12-pixel marker per miss allowed (up to 28),
14 pixels apart from `D_80228B54 + 0x38` at y `0xCD` (the race overlay from
`0x801F74A0`). Past 6 they ran under POWER, so the display-list rewriter
moves each marker's rectangle into the room 6 take (`squeeze_misses` in
`src/dlrewrite.cpp`). Verified: a Reverse race with 10 misses shows all ten,
the miss taken crossed out.

### The version on the title

The title's frame in state `0x03` is `func_i0_802C5A7C` (offset `0x27C`); its
big logo is up while `D_i0_802C6BE0` is 0 (then the menu). Under "(C) 1996
Nintendo" the mod draws "VERSION 1.1.0: LATEST VERSION" or "...: NEWER
VERSION AVAILABLE" with `func_801E9858`'s font 1, which has capitals, digits
and full stops but no lowercase or colon (unknown characters draw as 0). At
six tenths size (`D_80226E24 = 0.6`, `D_80226E28 = 1` to scale; with it 0
the font draws at full size) the game's own spacing squeezes the letters,
so each character is drawn on its own at an even step; the colon is two
full stops. Colour comes from `func_801E946C`'s style: 0 white, 3 and 5
yellow, 4 red, 6 green, 7 grey (its 12 tint values don't tint this
lettering). Scaled, a string's y is about 17 pixels above where it lands.

### Lap message past 3 laps

With more laps than 3, the message on crossing the line ("LAP n", from the
lap counter at race record `+0x0`, drawn by `func_801FAEB8` with font 0) was
seen in play showing the wrong number (LAP 8 on lap 4 of a 10-lap Reverse
race), while the HUD's LAP (`+0x320`) was right. The game sets the two
together as a lap is counted (`0x80077A38`, `0x8007940C`), but `+0x320`
alone goes back one when the line is crossed the wrong way and on again
when it is recrossed (`0x80077C90`). A 10-lap Reverse race of computer
riders logged with `WR64_LAP_LOG=1` kept all their counters in step, so
something a human rider does parts them. The message now shows the HUD's
lap (`patches/lap_message.cpp`, registered over the function in
`src/overlays.cpp`; checked with `WR64_LAP_TEST=8,10`, which parts them on
purpose). Not known: whether a counter run ahead could end a race early,
since the race ends on `+0x0`.

Play-test (2026-11-01, Expert Championship, 10 laps): the HUD's lap
flickered back a lap mid-lap now and then, the message ran ahead (LAP 4 on
lap 3, LAP 6 on lap 4, LAP 7 on lap 5) and the race ended after lap 7 or 8:
so `+0x0` does run ahead of `+0x320` and ends races early. The human's line
flag (`+0x324`) flips mid-lap on Sunny Beach too (near buoy 36), as the
HUD's flicker suggests; the computer's riders (and the human's rider driven
by the computer, `WR64_TEST_AUTOPILOT=1`) count true. Championship counts
with `D_800D8178` = 0. The game's own lap-time sum allows at most 9 laps
(`0x80077960`: `slti $at, $a1, 0xA`). `WR64_LAP_LOG=1` now logs each race's
course, mode, difficulty, counting mode and conditions, every change of the
human's line flag, and every rider's counters, for a play-test to find
where `+0x0` gains.

Resolved (2026-11-01). A logged 5-lap Reverse race kept `+0x0` and `+0x320`
in step from 1 to 5, and ended after 5, yet the HUD's LAP showed 1 and 2 by
turns. Two causes:
- **The HUD's LAP** (drawn by codeseg `0x801F5240`: the smaller of `+0x0` and
  the laps to finish, its last digit from the speed's digit pictures,
  `0x0103D8D8`) was read as part of the speed by the port's miles-an-hour
  rewrite (`src/speed_units.cpp`), which took every digit right of x 158: in
  Championship the LAP digit is drawn at x 192, the speed's at 225, 241 and
  257, so lap 3 at 85 km/h read "385" and the LAP showed the first digit of
  239 mph. Only the run of digits side by side at the right is the speed's
  now (`WR64_SPEED_LOG=1` logs which).
- **Ten laps** are more than the game keeps: its lap times stop at nine
  (`0x80077960`), and the ten-lap race's counter ran ahead and ended it
  early. Laps go to 9 now (the page, the Races tab, and any 10 saved before,
  held to 9 in races).

### Still to find (later steps, not menus)

- Custom lap count: laps to finish are `0x801CE728` (Free Roam holds it at 99);
  whether the HUD's "lap x/3" and the results cope with other counts is
  unchecked.
- Stunt Mode's time limit: the human's race record `+0x2C4`, milliseconds
  left (Free Roam holds it at 15000).

# Wave Race 64: what the PC VR edition found out

Everything below was worked out while building the PC VR edition of
Wave Race 64: Recompiled (release `v1.0.0-vr`), and is written down so the
next person modding this game doesn't have to find it again. It is for
**Wave Race 64 (USA) (Rev A / v1.1)** only (sha1
`508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`); other versions lay their code and
data out differently.

Each finding names where it is used in this repository, which is also where
the fuller explanation lives, usually in the comment at the top of the file.
Function and variable names (`func_8006E674`, `D_801AEE20`, `gGameModes`) are
those of the [Wave Race 64 decompilation](https://github.com/LLONSIT/Wave-Race-64)
at revision `a51b38a`; many of the things below could be given proper names
there.

## Conventions

- **Addresses.** `0x80xxxxxx` is the game's virtual address; the same
  memory in physical terms drops the `0x80` (`0x800DAB24` is physical
  `0x0DAB24`). The source uses physical addresses wherever it reads RDRAM
  directly.
- **Reading RDRAM in the recomp runtime.** Memory is stored as native 32-bit
  words. A display-list command is two host words at its offset; a byte is at
  `offset ^ 3`, and a 16-bit half (matrix and vertex fields) at `offset ^ 2`.
- **Segments.** In a race, segment 1 is the shared object data (buoys, HUD,
  rings), segment 3 the frame's buffer (alternating between physical
  `0x11F8E8` and `0x1388D0`), segment 6 the sky's vertices, segment 8 the
  rider models, segments 0x0D and 0x0E the course's own data.
- **How things were verified.** Mostly by scripted runs: the port can play an
  input script (`WR64_INPUT_SCRIPT`), take window screenshots
  (`tools/capture_window.ps1`), dump RDRAM at chosen race frames
  (`WR64_RAM_DUMP`, `WR64_RAM_DUMP_AT`), trace every matrix, vertex load and
  call of a frame (`WR64_3D_TRACE`), and leave chosen display lists out
  (`WR64_DROP_LISTS`). Most findings below were confirmed by changing the
  value and watching the result.

## Game state and modes

| What | Where | Notes |
|---|---|---|
| Game state | `0x800DAB24` | `0x02` title, `0x03` main menu, `0x05` boot, `0x07` attract demo, `0x0A` rider select, `0x14` course select, `0x1E` course overview, `0x28` a race being ridden (`TIME_TRIAL` in the decomp), `0x32`/`0x34`/`0x38` results, `0x3C`-`0x48` options, `0x66` ceremony. `0x28`-`0x2F` are treated as racing. |
| Mode chosen in the menus | `0x801CE608` (`D_801CE608.gameMode`) | Set before the course loads, so it is the one to read while a course is being set up. |
| Mode of the race | `0x801CE620` (`gGameModes`) | `0` Time Trials, `1` 2P VS, `4` Championship, `11` Stunt Mode. |
| Course | `0x800D8170` | `0` Dolphin Park, `1` Sunny Beach, `2` Sunset Bay, `3` Marine Fortress, `4` Drake Lake, `5` Port Blue, `6` Twilight City, `7` Southern Island, `8` Glacier Coast. |
| Human's race slot | `0x800D48DC` | Index into the rider states and race records. |
| Pause menu choice | `0x801CE624` (s16) | `-1` when not paused. |
| Laps to finish | `0x801CE728` (`D_801CE728`) | Held at 99 by Free Roam so a race never ends. |
| Frame divider | `0x800D461C` | The game runs at 20 fps in menus and 30 in races by writing a divider the VI handler counts against (`patches/framerate.cpp`). |
| Opened courses and difficulties | `0x801CB28B` = `0x16`, `0x801CB280` = `0x1F` | The published GameShark codes; set every frame by the Debug tab's unlock (`patches/unlock_all.cpp`). |

## Riders

Rider state: `0x80192690 + slot * 0x1718`.

| Offset | Meaning |
|---|---|
| `+0xC54` | Animation playing: `22` riding, `23` thrown off, `7` climbing back on, `4` the backflip played when a rider retires (started mid-race, it hands the jet ski to the computer). |
| `+0xC58` | Frame of that animation. |
| `+0x1608` | Countdown to being thrown; setting it to 1 throws the rider (how F3 and the X button work). |
| `+0x16D0` | Frames spent stranded on land; 81 of them start COURSE OUT. |

`func_80053EAC(slot, animation, frame, speed)` starts an animation. After a
throw the game starts animation 7 from frame 0, having put the rider by the
stern; if that call is refused it asks again every frame, with the rider
sunk out of sight. That is how the first-person swim holds the climb back
until the swimmer arrives (`patches/swim_back.cpp`).

The rider's drawn parts are matrices in the frame's buffer: part `p` of slot
`s` at segment 3 `+ 0xE108 + s * 0x40 + p * 0x100`. Parts `0`-`2` are the jet
ski (hull, handlebars, a third piece), `3` the head, `4` the chest, `5` the
hips, then the limbs; 18 in all, the six limb parts drawn twice with the right
side mirrored by its matrices. The chest's `-y` is up and its `z` points about
30 degrees above where the rider faces when upright. The game's camera
(view) matrix is at segment 3 `+ 0xE088`, its projection at `+ 0xE008`.

**Racer models** (`src/companions.cpp`). Each character's models are a pair
of MIO0 chunks in the cartridge, a full one for the player's rider and a
smaller one for a computer's, listed in the table at `0x800DABD0`. The parts'
lists are in templates at `0x800DACD0`, `0x44` bytes each, three per
character (full, and two of the smaller). Warm Up loads the other racers and
then overwrites them with course data, so to draw extra riders there the
smaller chunk is unpacked and relocated by hand (segment 8 counts from the
chunk's start). One character has five more parts than the others (hair,
probably), which the companions don't draw yet.

## Race records

`0x801C2938 + slot * 0x378`.

| Offset | Meaning |
|---|---|
| `+0x0C` | The buoy the direction arrow points at next. |
| `+0x130`, `+0x134` | Buoys missed; `+0x134` counts toward disqualification (`func_80077080` and on). |
| `+0x2C4` | Stunt Mode's time left, in milliseconds (15000 at the start). |
| `+0x2E4`, `+0x2E8` | The COURSE OUT countdown. |
| `+0x328` | Frames ridden the wrong way; past 40 the game beeps every 20 (sound `0x35`). |

`func_80074264` asks, every race frame and for each human, whether a point
`(f12, f14)` is outside the course, and runs the COURSE OUT countdown. Nothing
else calls it, so answering "inside" removes COURSE OUT entirely
(`patches/free_roam.cpp`).

## Buoys and course objects

The buoy list is `D_801AEE20`, `0x104` bytes an entry, count in `D_801BB120`.

| Offset | Meaning |
|---|---|
| `+0x98` | Kind: `0` and `1` the race buoys (one per side to pass), `2` Stunt Mode's checkpoints, `3` the start, `4` invisible turn markers. |
| `+0xC4` | Active. The collision and bobbing loops (`func_8007DF4C`, `func_8007DBC0`) skip an entry with it cleared. |
| `+0xC8` | Drawn. The chase camera follows the buoys, so clearing this, or moving buoys away, sends the camera to film from somewhere else. |

Clearing `+0xC4` and leaving the drawing calls out removes buoys without
upsetting the camera. Missed buoys and POWER UP / POWER DOWN are still
counted from the list, though; where power is stored hasn't been found yet.

The drawing calls, one per object, all in segment 1 and the same on every
course (found from a frame trace of each course, then left out in turn):

| Lists | Draws |
|---|---|
| `0x0102CC58`, `0x0102CC70` | race buoys on Sunny Beach, Sunset Bay, Drake Lake, Southern Island |
| `0x0102CCE8`, `0x0102CD00` | race buoys on Marine Fortress, Port Blue, Glacier Coast |
| `0x0102CD78`, `0x0102CD90` | the rows of small buoys marking the course's edges (first set of courses) |
| `0x0102CE08`, `0x0102CE20` | the same (second set) |
| `0x01029BE0`, `0x01029C88` | a race buoy's striped tail and letter, for one side |
| `0x0102AD90`, `0x0102AE38` | the same, for the other side |
| `0x0102CE78` | the arrow sprite over each of the next buoys (about two a frame) |
| `0x0101EA48`, `0x0101E620` | Stunt Mode's rings |

**Engine limits.** The game hands out matrices from fixed areas of the
frame's buffer: 12 race buoys a player (buoy at `0x95C0`, marker at `0x9BC0`,
`+ 0x300 * player`) and 32 "facing" objects a player (`0xA1C0 + 0x800 *
player`; race and edge buoys that turn to the camera). Past the cap an object
keeps a stale matrix and is drawn on top of another. A flat screen never sees
that many; looking all round does, so the VR build moves both areas
elsewhere, with room for 64 and 256 (hooks in `recomp/wr64.toml`,
`src/vr.cpp`).

**Culling.** `func_8006E674` decides each frame which objects of three groups
are drawn, by distance (from the course settings) and by angle from the
camera's heading: 60 degrees for the first two groups, 90 for the third. Its
results are only read by drawing code. See [VR-CULLING.md](VR-CULLING.md) for
the groups, the instructions and the measured angles.

## The course look table

`0x800D9964 + course * 0x110`, copied for the race into `0x801CB058`;
`D_801C0C80` points at the copy.

| Offset | Meaning |
|---|---|
| `+0x48` | Scene fog colour. |
| `+0x78`, `+0x7C` | Scene fog start and end, on the N64's 0-1000 depth scale. The fog factor is `0x1F400 / (end - start)` and must fit in 16 bits: a start of 999 overflows it. Setting both past the far plane (1000 and 1100) removes the fog. |
| `+0x88` | Water fog colour. |
| `+0x98`, `+0x9C` | Water fog start and end; used only on Drake Lake (in `Draw_WaterEffects`). |
| `+0xA0`, `+0xA4` | Object fade start and cut-off distances (see Culling). `+0x80`/`+0x84` are not fog. |

The sky's display lists come from a table of one word per course at
`0x800DC0E4`. Swapping a course's look entry and sky entry together gives it
another course's sky and water, and it is read as the course loads
(`free_roam_frame` in `patches/free_roam.cpp`). Drake Lake's drifting mist is
scenery, not fog.

## Water and sky

- **Wave level.** `D_800D49B0`, taken per course from `D_800EABEC` (Drake Lake
  10, the calmest, to Marine Fortress 40; the options' calm and rough are 10
  and 40). It is passed to `func_8004F9E0` (a0 the water's height, a1 the
  level), which sets the water up. Levels from 0 up to 100 were tried
  without trouble; level 0 still leaves the swell some courses build in.
- **The wave mesh** is built to cover only what the flat camera sees:
  `func_8004C1D0` traces the view down to the water, `func_80050B78` places a
  grid and `func_80050F8C` builds the mesh into segment 3; `func_80051238` is
  the whole update. It is a hexagon about 1500 units across on a triangular
  lattice fixed to the world (points 64 apart), inside six flat quads. See
  `patches/water_all_round.cpp` for building it again for other headings.
- **The sky** is three fans of six triangles rebuilt each frame into segment
  6, placed on the water patch about 1300 units ahead of the camera, not on
  the camera: the floor under the waves (list `0x010103F0`), the gradient
  tent (`0x010103B8`) and the clouds (`0x01010470`). Its triangles are
  back-face culled; a hole shows the last frame rather than black. The game
  reads parts of the structure back (the tent peak's `t`), so it must not be
  written. See `patches/sky_all_round.cpp`.
- **The floor.** The game draws only a flat textured floor at the water's
  level (on Sunny Beach, 50 units down). The seafloors in `src/seafloor.cpp`
  were measured from each course's own collision and scenery.

## Sound and music

- `AudioGeneral_PlaySound(sound)`: `0x22` a countdown beep, `0x23` the go,
  `0x21` the lap chime, `0x35` the wrong-way beep.
- **The go starts the music.** With the sound driver in race mode (`7`, the
  byte at `0x800E7C94`) and `0x800E7CB8` clear, sound `0x23` also writes
  `0x800E7CD8 = 10`, `0x800E7CD4 = 0xB4`, `0x800E7CA0 = 1` and
  `0x800E7CE8 = 0x140`; ten ticks later `func_800BF370` starts the course's
  music through `func_800C1540(sequence)`. Silencing the go also silences
  the music unless those writes are made anyway.
- **Sequences.** Course music is `course + 6`; `3` the main theme, `4` the
  options, `15` the victory theme.
- **Another course's sequence doesn't fit.** Starting another course's music
  loads its instrument bank into the room the current course set aside for its
  own. Some fit and some don't: Twilight City's on Marine Fortress crashes in
  `Audio_PatchBank`. The Race music setting swaps only the replacement
  recordings for that reason (`src/music.cpp`).
- **Voices.** `func_800C32A0(line, priority, hold)` queues a voice line,
  directly or after a delay (`func_800C338C`, `func_800C3494`). The announcer
  is everything below `0x60` (the countdown `0x0D`-`0x0B`, go `0x0A`); the
  riders' own shouts are from `0x60` (the random ones `0x6C`-`0x6F`).

## The race HUD

- Speed digits are one texture per digit, ten in a run: Time Trials and
  Championship `16x20` from `0x0103D8D8`, `0x500` apart; Stunt Mode `12x16`
  from `0x0802C3E8`, `0x300` apart. The unit is a `24x12` "km/h" texture.
  The speed sits at x 158-236 (Stunt Mode 230-300), y 18-60 of the 320x240
  frame. See `src/speed_units.cpp`.
- Other lists in segment 1: the direction arrow's model `0x01010528`; the
  start's countdown lights `0x010106C0`, `0x01010730`, `0x01010998`,
  `0x010109F8`; the messages that pop up mid-screen `0x01014820`.
- `D_801CE638` is not the arrow's switch but the play view's: `0` gives the
  replay cameras and no HUD.

## Course geometry

- **Sunny Beach's segments**: `0x0D` at physical `0x2D6800`, `0x0E` at
  `0x2EC718`. Its hill texture `0x0E013000` (64x32, foliage with a sand strip
  at the bottom) and shore texture `0x0E010800` (32x64, sand into cyan
  shallows) were reused for the far side of the island (`src/land_backs.cpp`).
- **Dolphin Park**: the stunt ramp's face rises from x 1033 to its crest at
  1441, 90 units up, across z 3112-4126; the buoy ring has a radius of 7700
  about the origin. Its four Stunt Mode ramps are at (2500, -4952),
  (92, -4952), (-2407, -4952) and (-6065, 664). Island, piers and rocks are
  measured in `src/seafloor.cpp`.
- Units: the chase camera rides about 55 units above the water; the VR build
  takes 33.3 units to a metre.

## Memory free for a mod

The game runs in the first 4 MB and never reads `osMemSize`; the runtime
reports 8 MB, so everything from physical `0x400000` up is free. This build
uses: seafloor vertices `0x400000`, lists `0x500000`, underwater fog
`0x5C0000`; the display-list rewriter's scratch list `0x700000`; merged waves
`0x741000`; the sky `0x7C1000`; first-person views `0x7D0000`; buoy-matrix
areas `0x7D1000` and `0x7D8000`. The audio task keeps a private copy of its
command list at `0x7E0000`, so leave that alone.

## Rendering: the RT64 changes

The VR work is a set of scripted patches to RT64
(`tools/patch_rt64_vr.py`, documented at its top), which may be useful to
anyone adding VR to another RT64 port:

- the headset's pose is applied in `ProjectionProcessor::processScene`, after
  the camera is interpolated and before the lens, so it runs at the display's
  rate, not the game's;
- each eye is one more of the renders RT64 already does per displayed frame
  for interpolation, so the two eyes never come from different moments;
- the headset is a second output beside the window, fed as Direct3D 12
  textures through callbacks, so RT64 itself knows nothing of OpenXR
  (`src/vr_openxr.cpp` is the OpenXR side);
- the game's lens is swapped for each eye's asymmetric frustum, and
  everything drawn flat is moved onto a panel hung in front of the seat, so
  the HUD and menus line up in both eyes.

The game's own projection is off-centre: `[2][1] = 0.3905` against
`[1][1] = 1.3018`, so the screen's middle is 16.7 degrees above where the
camera looks. Any view built from scratch, like first person, has to allow
for that on a monitor.

## Still unknown

- Where the power level (POWER UP / POWER DOWN) is stored.
- What the five extra model parts of one character are.
- Why some instrument banks fit another course's room and others don't.

## Follow-ons

- Many of the functions and variables above have only placeholder names in
  the decompilation; a pull request naming them there would help everyone.
- The RT64 VR changes could be offered to the RT64 and N64Recomp communities
  as a starting point for other VR ports.

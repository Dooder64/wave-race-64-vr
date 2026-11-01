# Course creator: research

Idea (the user, 2026-10-31), in three stages:

1. **Ramps anywhere.** Place ramps on the existing courses.
2. **Race layouts.** Buoys, a start and laps on a course, with computer
   riders racing them.
3. **A course from open sea.** On a blank patch of ocean, place prefabricated
   land pieces and scenery (piers, buildings, boats), with the seafloor
   generated round them, as for the game's own courses.

Status: research only. Nothing is built or tried yet. This shares most of
its groundwork with guest courses (docs/GUEST_COURSES.md): new scenery drawn
from the mod's memory, new collision, a boundary and a seafloor. Whichever
comes first builds that groundwork for the other.

## What's known

- **Land collision** is a grid of bounded planes, and each plane is exactly
  one triangle (docs/GUEST_COURSES.md, "The collision fits triangles
  exactly"). We can add planes for a placed object, as long as we know how
  the game treats them.
- **Drawing our own geometry from free memory** is done already: the
  seafloor and the back slopes build their own lists every course
  (`src/seafloor.cpp`, `src/land_backs.cpp`).
- **Sea floors from shorelines** are done already: each vanilla floor was
  built from where the land meets the water, deepening by distance to a 15 m
  cap. The same rule, run on placed land's shorelines, generates a floor
  for a made course (stage 3).
- **Riders of our own** exist: the Dolphin Park riders (`src/companions.cpp`)
  ride routes of ours with the game's own models, steering round the player.
  They are puppets, though, not jet skis under the game's physics.
- **The game's own computer driver** can take over a rider in mid-race: the
  retire backflip hands the jet ski to it (RESEARCH.md, Riders). So the
  game's AI drives any rider record. What it steers by isn't known yet.
- **The buoy list** (`D_801AEE20`, 0x104 bytes an entry; kinds for the two
  sides, checkpoints, the start, turn markers) is known well enough to write
  our own.

## What isn't known yet (the first research)

1. **How a ramp works.** Dolphin Park has its stunt ramp in every mode, and
   four more only in Stunt Mode. So:
   - Are ramps part of the course's collision planes, or objects of their
     own?
   - How does the jet ski get its launch off one? Possibly just the
     physics on a sloped plane, or a special surface or object.

   Finding this out decides stage 1. Plan: dump the collision planes on
   Dolphin Park in Time Trials and in Stunt Mode, and compare. If the four
   ramps are planes, a ramp is just geometry, and stage 1 is mostly the
   placing.

   **First answer (2026-10-31): ramps are not land collision.** In a memory
   dump of a Dolphin Park Stunt Mode ride, the land collision
   (`D_801C3B7C`/`80`/`84`) holds only about 100 planes, spanning x -5127 to
   5114 and z -4346 to 5567. None is at the main ramp (x 1033-1441, z
   3112-4126), and the grid doesn't reach the Stunt Mode ramps at z -4952 at
   all. So ramps are objects with code of their own, as buoys are.

   **Found (2026-10-31): the ramp tables and the object list.**
   - Each course has two ramp tables, one per mode: `D_800D66A4[course]`
     (races) and `D_800D66CC[course]` (Stunt Mode), pointers into the
     course's data. The course object setup (`func_80071E70`, from
     `0x80072830`) picks them by mode.
   - A ramp entry is five s16s, 10 bytes: x, z, which difficulty or mode it
     appears in (compared with `gDifficulty`), a type, and an angle in
     degrees. The table ends with an angle of -1. Dolphin Park's Stunt Mode
     table (ROM `0x2A9C18`, loaded at `0x8030B548`): (2500, -4952, 270°),
     (92, -4952, 270°), (-2407, -4952, 270°), (-6065, 664, 7°).
   - The setup turns each entry into an object in `D_801BC940` (0xC4 bytes
     each, at most 64, count in `D_801BFA40`): x `+0x24`, y `+0x28`, z
     `+0x2C`, angle `+0x48` (float, degrees), kind `+0x4C`, the entry's
     third and fourth fields at `+0x50` and `+0x58`. In a Dolphin Park Stunt
     Mode ride there are 26 objects: the four ramps (kind 1), then the rings
     (kind 8, at y 60), then others.
   - The same data block holds the course's buoys and Stunt Mode checkpoints
     (x, z, kind, angle: kinds 2 and 3 as in the buoy list) and the rings
     (x, y, z, 0, number, angle).
   - Not known yet: Dolphin Park's race ramp table is empty, so its
     always-there main ramp is made some other way; and the code that
     launches a jet ski off a ramp object (it reads `D_801BC940`).

   **Tried in Free Roam (2026-10-31, `WR64_TEST_RAMP=<x>,<z>,<degrees>`
   in `patches/free_roam.cpp`, scripted runs only):**
   - **Appending an object after setup does nothing.** A copy of a ramp
     object, moved, turned and counted in, was neither drawn nor ridden: the
     game builds what goes with each ramp (its drawing and its slope) as the
     course sets up.
   - **Giving setup a longer table works.** Pointing Dolphin Park's Stunt
     Mode ramp table (`D_800D66CC[0]`) before the course loads at a copy in
     the mod's memory (`0x7DE000`) with a ramp added, the game made it object
     4, numbered after the others (`+0x64` = 24).
   - **Four ramps at most, at first.** The fifth was drawn, but the jet ski
     stopped dead against it: no slope to ride (lifted since: see below).
   - **Moving one of the four works fully.** With the fourth ramp moved to
     (-4800, -4952) at 270°, the jet ski ridden at it climbed the slope
     (heights 31, 51, 74, 96, 111) and flew off the top, as off the game's
     own (screenshots show it on the wooden ramp, then airborne).
   - The way over a ramp is (sin, cos) of its angle in (x, z): 270° is
     ridden toward -x, 90° toward +x.
   - **Only where the course has the ramp's model.** The ramp is drawn from
     two lists in the course's own data (`0x0D02BDE8`, `0x0D02BDF8`, segment
     `0x0D`), so on a course without ramps the model isn't there to draw.
   - Note for tests: Dolphin Park in Stunt Mode has a solid structure near
     (400, -2000); `WR64_FREE_ROAM_TEST_AT`'s heading points the opposite
     way from its comment (0 sends the jet ski toward -x).

   **The limit, found and lifted (2026-10-31):**
   - Each ramp's slope is built as it sets up (`func_800744EC`) as pieces,
     64 bytes each: six for Dolphin Park's ramp type (`0x800D6E70`), five for
     some others'. They go into one array, `D_801BFD78`, with room for 32,
     counted in `D_801C0578` right after it. A ramp's `+0x64` is its first
     piece.
   - The setup builds a piece only while the piece's number plus the ramp's
     piece count stays under 33 (`slti $at, $t8, 0x21`, `0x80074C98`). So
     with 6-piece ramps it stops at 27: a fifth Dolphin Park ramp got 3 of
     its 6 pieces, drawn but met as a wall.
   - The pieces are read by `func_8006E0F4` (the objects' drawing and
     culling) and by `func_8007F700`, `func_8007FAE4` (the jet ski against
     the slopes), each from the count.
   - **Moved:** six hooks in `recomp/wr64.toml` give the array's address as
     `0x80800000` (in the setup at `0x80074558` and, for Sunset Bay's extra
     pieces, before the branch at `0x80074DD8` whose delay slot adds -0x288;
     at `0x8006E170`, `0x8007F714` and `0x8007FAF8`) and raise the cap to 256
     pieces (`patches/ramp_pieces.cpp`). That memory, above the game's 8 MB
     and below the runtime's heap at 16 MB (`0x81000000`), is used by
     nothing else; the runtime keeps 512 MB readable and writable.
   - **Tested:** six ramps on Dolphin Park (its four, plus two added in a
     row at (-4800, -4952) and (-7300, -4952)): 36 pieces built, the fifth
     launched the jet ski about 170 units up, and it rode up onto the
     sixth's deck, across and off it. A Sunset Bay free roam still loads
     and rides, its two ramps and two extra pieces (14) built in the new
     array.

   **Next for "ramps anywhere":** the placing itself (the tables written
   from a saved layout, before the course loads, for any course and mode);
   and the ramp's model (and textures) for courses that have no ramp of
   their own.
2. **What the computer riders follow.** A racing line or points per course,
   or the turn-marker buoys (kind 4)? Two more box lists (`D_801C1F84`,
   `D_801C1F88`) are unexplained, and may be part of it. Stage 2 depends on
   this.
3. **Memory for prefabs.** A piece borrowed from another course has to be
   unpacked from the cartridge into the mod's memory, along with its
   textures. About 2 MB is free now. Measure the pieces: a pier, a Port Blue
   building, a Marine Fortress boat.

## How placing could work (no mouse needed)

The same way in flat and in VR, with a controller, in Free Roam:

- **Edit mode** (a button, or a Free Roam option). A see-through ramp hangs
  ahead of the jet ski. Ride it where you want it. The shoulder buttons turn
  it, up and down sizes it. A button drops it, and another picks up the
  nearest one again.
- **A menu of pieces**, styled like the game's: ramps of a few sizes, then
  buoys, and in stage 3, the prefabs.
- **Saved** as a small course file (JSON) in the player's folder, one per
  course. A file names only pieces and positions, never any game data, so
  players can share them.

Stage 2 can use riding as well:
- **Record a lap.** Ride the line you want. Buoys are offered along it,
  either side of where you rode, then adjusted by hand.
- The recorded lap is also a ready-made racing line for computer riders, if
  their driver can be given one (question 2).

## Stage 3: a course from open sea

- **The blank sea.** Any course as a base, with its scenery, collision and
  objects hidden. Free Roam already removes the buoys, rings and COURSE OUT.
- **Prefab land.** Not free-form sculpting. Pieces cut from the nine courses:
  - islands and beaches, cliffs, rocks and arches (Dolphin Park, Southern
    Island);
  - piers (Dolphin Park);
  - buildings, harbour walls and boats (Port Blue, Twilight City, Marine
    Fortress);
  - ice (Glacier Coast).

  Each piece needs its own collision, cut from its course's planes, and its
  shoreline marked for the floor. Pieces are taken from the player's own ROM
  as the game runs, like everything else.
- **The floor.** Generated from the pieces' shorelines with the vanilla rule:
  shallow by the land, deepening to 15 m.
- **Edges.** Back slopes where a piece is cut off (`src/land_backs.cpp`), and
  a boundary drawn round what's been placed.
- **Look.** Any course's sky and water (the SKY option), and later the
  day/night cycle (docs/DAY_NIGHT.md).

## Effort, roughly

| Stage | Size | Waits on |
|---|---|---|
| 1. Ramps anywhere | medium | question 1 (how ramps work) |
| 2. Race layouts, buoys and laps | medium | the buoy list (known) |
| 2. Computer riders racing them | large | question 2 (what they follow) |
| 3. A course from open sea | very large | the groundwork above, plus cutting out prefabs and their collision |

Stage 3 is ambitious but not out of reach. Every part of it either exists in
the port already or follows from stage 1 and the guest-course groundwork.
The big jobs are cutting prefabs out cleanly and building the editor itself.

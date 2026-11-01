# Guest courses: research and plan

Goal: ride a jet ski round places from other N64 games, starting with
Majora's Mask's Great Bay. The work is the same as for the game's own courses:
sensible boundaries, missing land filled in, seafloors under the water.

Status: research only (2026-10-31). Nothing is built, and nothing here has
been checked against the Majora's Mask ROM yet.

## The rule: nothing of the other game is shipped

Like the HD pack's reliance on the player's own Wave Race dump, a guest
course is built from the player's own ROM of the other game. A converter
reads it once (at first use, or from a tool) and writes the converted course
to the player's own folder. The release holds only the converter and our own
additions: boundaries, filled-in land, seafloors, buoy layouts. That is the
roadmap's plan (course builder, then guest courses from the player's ROMs).

## What a Wave Race course needs

From docs/RESEARCH.md, docs/MODERN_WATER_SOURCE_AUDIT.md and the
decompilation:

| Part | Where the game keeps it | What a guest course needs |
|---|---|---|
| Scenery | compressed segments `0x0D`/`0x0E`, F3DEX-family display lists (`0x04` vertices, `0xBF` triangles) | the guest's models, converted to these commands |
| Land collision | segment 6 at `0x80306800`: a grid (`D_801C3B7C`), plane lists (`D_801C3B80`) and 64-byte bounded planes (`D_801C3B84`), installed by `func_80071E70` | one plane per guest triangle (see below) |
| Inside / outside the course | `D_801C1F80`: a list of boxes (`func_80074264`; Dolphin Park uses a circle instead) | our own boundary, drawn by hand |
| Two more box lists | `D_801C1F84`, `D_801C1F88` | not known yet; find out what they do |
| Water | one flat sea level, waves built round the camera | the guest's sea level (Majora's Mask's water boxes) |
| Look | look table `0x800D9964 + course*0x110` (fog), sky table `0x800DC0E4` | borrow an existing course's, then tune |
| Seafloor, back slopes | ours (`src/seafloor.cpp`, `src/land_backs.cpp`) | the same treatment |
| Buoys, start, AI racing lines | buoy list `D_801AEE20`; the AI's lines not researched yet | only for races, not needed for Free Roam |

**Free Roam first.** Free Roam already has no buoys, laps or COURSE OUT, so
a guest course there needs only scenery, collision, water level, boundary and
look. Races, with buoys and AI lines, can come later.

### The collision fits triangles exactly

`func_8007F448` tests a point against each plane in its grid cell:

- the point is on the inner side of two edge planes (`+0x18`, `+0x24`, each
  ≥ 0);
- it is within a band of a third edge plane (`+0x30`: below 0, above the
  limit `+0x3C`);
- it is 0–16 units above the surface (`+0x0C` normal).

That is a triangle with the vertex at the origin `+0x00`, so each Majora's
Mask collision triangle becomes one Wave Race plane, with no approximation.
The grid is a uniform 2D grid over x/z (cell sizes as shifts, `D_801C3B70` /
`D_801C3B78`), so building one for a new course is simple.

## What Majora's Mask has

Scenes, from the decompilation's scene table (zeldaret/mm):

| Scene | Internal name | For a jet ski |
|---|---|---|
| Great Bay Coast | `Z2_30GYOSON` | **the first target**: open sea, beach, the Marine Lab's pier, the Fisherman's hut |
| Zora Cape | `Z2_31MISAKI` | the second: cliffs, Zora Hall's outside, the sea round the temple |
| Pirates' Fortress exterior | `Z2_KAIZOKU` | the fortress walls in the sea; a good set piece |
| Pirates' Fortress moat | `Z2_TORIDE` | narrow water, maybe |
| Pinnacle Rock | `Z2_SINKAI` | underwater and dark; likely unsuitable |
| Waterfall Rapids | `Z2_35TAKI` | flowing water at many levels; not possible (one sea level) |
| Zora Hall, Marine Lab, Ocean Spider House, Great Bay Temple | indoors | no |

The formats (zeldaret/mm `z64bgcheck.h`):

- **Collision header** (0x2C bytes): bounds, vertex count and list (`Vec3s`),
  polygon count and list, surface types, cameras, water box count and list.
- **Collision polygon** (0x10 bytes): type, three vertex indices (low 13
  bits; the high 3 are flags), a normal (`s16` ×3, scaled by 0x7FFF) and a
  plane distance. The conversion reads these directly.
- **Water box** (0x10 bytes): minimum corner (its `y` is the water's surface),
  x and z lengths, packed properties (room, light, disabled). The sea's water
  box gives the height our waves go at.
- **Scenery**: each scene has rooms, and each room's shape points at F3DEX2
  display lists (opaque and see-through). Textures are in the scene and
  room files, or in shared files (`gameplay_keep`, `gameplay_field_keep`).
- **The ROM**: a file table (`dmadata`) of Yaz0-compressed files. Yaz0 is a
  simple, well-documented compression.

### Majora's Mask helps: its sea has a real floor

Zora Link swims under Great Bay, so the sea there was modelled with a seabed,
unlike Wave Race's bottomless water. Where it exists, it can be the seafloor
as it is, raised wherever it is deeper than our 15 m rule. Our own floors are
needed only past its edges.

## Converting one course

1. **Read the ROM.** Find the scene's and rooms' files in `dmadata`, then
   decompress them.
2. **Scenery.** Walk each room's F3DEX2 lists and decode them to triangles,
   with their textures and render settings. Then emit Wave Race's F3DEX
   commands, the way `src/seafloor.cpp` already builds its own lists:
   - The RDP commands (textures, combiner, colours; opcodes `0xE0` and up)
     are the same in both.
   - The vertex, triangle, geometry-mode and matrix commands differ, and are
     rewritten.
   - Lists are cut into chunks by area, so only the part near the camera is
     drawn, as the seafloor does.
3. **Remove what Wave Race replaces.**
   - Majora's Mask's own flat water surface, since our waves take its place.
   - Its sky.
4. **Collision.** Turn every triangle into a plane, build the grid, and point
   `D_801C3B7C`/`80`/`84` at ours after the base course loads.
5. **Scale.** Measure a known object in both games, such as a doorway, Link's
   height against the rider's, or the pier's height above the water. Wave
   Race takes 33.3 units to a metre.
6. **Boundary, land and floor.** Draw the inside/outside boxes by hand.
   Continue cut-off cliffs as back slopes (`src/land_backs.cpp`). Lay floors
   past the seabed's edge. All of this is our own data, and ships.
7. **Look.** Start from a course with a similar sky and fog, such as Sunny
   Beach or Southern Island, then tune it.

### How it would load

Start a Free Roam on an existing course (the base), then:
- hide the base course's scenery (as `hide_game_floor` does);
- draw the guest's lists from the mod's free memory;
- swap in the guest's collision pointers;
- move the start to the guest's beach.

The game's own loader and its DMA tables stay untouched.

**Memory.** Everything above 4 MB is the mod's (RESEARCH.md, "Memory free
for a mod"). Free now: about `0x5C8000`–`0x700000` and `0x400000`–`0x500000`
past the seafloor's use. That's roughly 2 MB. One Great Bay scene's
geometry and textures may need more, so measure first. If it doesn't fit,
the options are:
- load only the rooms near the rider;
- reduce texture sizes;
- see whether the runtime can give the game more than 8 MB.

## Risks and unknowns

- **Props.** Some buildings and objects in Majora's Mask are separate actors
  with models of their own, not part of the room. Trees, signs and huts may
  be missing until each is converted separately. Check which are actors once
  the scene is opened.
- **Lighting.** Majora's Mask's outdoor rooms are mostly vertex-coloured and
  should look right unlit. Anything lit by its environment lights will need
  its shading baked in.
- **Animated textures.** Flowing water and waterfalls are animated through
  segments `8`–`0xC`. Use a fixed first frame, or animate them ourselves.
- **One sea level.** Wave Race has one water height, so any guest water at
  another level (pools, rivers, the fortress moat if it differs) stays flat
  scenery.
- **Joining scenes.** Each Majora's Mask scene has its own origin. Placing
  Great Bay Coast and Zora Cape side by side means finding the offset by eye.
  Start with one scene.
- **Physics against odd geometry.** Wave Race's land planes were made for
  beaches and walls. Majora's Mask's piers, fences and stairs may catch the
  jet ski strangely. Leaving thin or tiny triangles out of the collision may
  be needed.
- **The base course's own logic.** Things tied to the base course may still
  run, such as Dolphin Park's circle boundary and the base course's own
  objects. Choose the base course with the least of these.

## Milestones

1. **Open it.** With the ROM, the converter lists Great Bay Coast's rooms,
   water boxes and props, and exports the scenery and collision as models to
   look at (top-down pictures, or OBJ files for Blender). Check the scale and
   the sea's height.
2. **See it.** Draw the scenery in Free Roam on a base course, with no
   collision yet. This shows the scale, the look and the memory use.
3. **Ride it.** Add the converted collision, the boundary and the start.
4. **Finish it.** Add the seafloor past the seabed, back slopes, the look, and
   whichever props matter most.
5. **Later.** Zora Cape and the Pirates' Fortress, races (buoys, AI lines),
   and other games (Perfect Dark's Carrington Villa is on the roadmap).

## What's needed from the player (for now, the developer)

- A **Majora's Mask (USA) N64 ROM**, the version the decompilation targets.
  It goes in a folder of its own; nothing of it is committed.
- 2Ship2Harkinian isn't needed. The converter reads the ROM directly.
  2Ship could help as a viewer to compare against, but isn't required.

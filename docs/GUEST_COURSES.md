# Guest courses: research and plan

Goal: ride a jet ski round places from other N64 games, starting with
Majora's Mask's Great Bay. The work is the same as for the game's own courses:
sensible boundaries, missing land filled in, seafloors under the water.

Status (2026-11-03, overnight, uncommitted, not play-tested): **Great Bay
Coast rides in Free Roam.** Its scenery and collision come from the
player's own ROM, on Dolphin Park as the base course. Not yet in any menu:
pick Free Roam, then on the course select go down past Southern Island to
GUEST COURSES and choose GREAT BAY (or start the game with
`WR64_GUEST=greatbay` and pick Dolphin Park).

### What's built

- **Reading the ROM** (`tools/guest/mm_rom.py`): the file table (dmadata at
  0x1A500 in the USA ROM) and Yaz0. Great Bay Coast's scene is file 1330,
  found by its collision header at 0x93F4 (the offset 2Ship2Harkinian's
  archive names it by). It has one room. Its land textures come from
  `scene_texture_05` (file 1118), which Majora's Mask loads to segment 6.
  Rock, rippled sand and gravel make it plain that's the right one.
- **The scene** (`tools/guest/mm_scene.py`): the header commands, the
  collision (923 vertices, 1254 triangles), the water box (surface at
  y = 0, Wave Race's own height), and the room's 43 display lists walked
  into 2535 triangles. The rooms switch levels of detail by `G_BRANCH_Z`;
  the walker takes the near, detailed list.
- **A first look** (`tools/guest/greatbay_export.py`): OBJ models of the
  collision and scenery, and a top-down picture.
- **The pack** (`tools/guest/greatbay_pack.py <rom> <folder>`, written to
  `%LOCALAPPDATA%\WaveRace64Recomp\guest\greatbay\greatbay.pack`, 421 KB).
  It holds the scene, room and scene-texture files as they are, plus a
  display list converted from F3DEX2 to Wave Race's custom Fast3D
  (F3D_OLD: `G_VTX` with v0 × 5 and `n << 9`, `G_TRI1` 0xBF with indices
  × 5, B6/B7/B9/BA/BB). The conversion:
  - vertices are re-batched 24 a load;
  - the render mode's first-cycle fog becomes a pass-through (Wave Race's
    fog range fogged the land out entirely);
  - lit vertices' lighting is baked into their colour, from a fixed sun;
  - animated-texture pieces (segments 8 and 9, mostly Majora's Mask's own
    water) are left out.

  The pack also holds a matrix scaling Majora's Mask units by 0.83 (Link,
  about 50 units tall, is then the rider's size), the collision triangles,
  and the start.
- **In the game** (`src/guest_course.cpp`):
  - The pack is copied to physical 0x400000, the seafloor's vertex area. On
    a guest course our seafloor is off, and Dolphin Park's is rebuilt after.
  - Its collision becomes one of the game's bounded planes per triangle, in
    a grid of its own. The game's pointers (D_801C3B40–84) are set to it
    every frame, and the base course's ramp slope pieces are counted out.
  - The scenery is drawn just before the water, after the sky
    (src/dlrewrite.cpp): drawn in the seafloor's place, before the sky, the
    sky covered all of it above the horizon (the user's first VR look,
    2026-11-03). The game's flat floor is left out. Dolphin Park's own
    segment-13 lists and its start gate (0x8029xxxx) are left out too.
  - Its buoys and gates are gone (D_801BB120 held at 0), and its boundary
    buoys (the floating-things list) are moved far away.
  - The Free Roam current, our riders, the dolphin and our fish are off.
  - The jet ski starts at the pack's start: open water west of the land,
    Majora's Mask (-4250, 2250), facing it. (The first start, (-6500,
    9000), was past the end of the collision at z 8960, and the user began
    the ride as if in a wall.)
    `WR64_GUEST_START=x,z,deg` puts it elsewhere in a scripted run.

### Seen in scripted runs

- It loads and draws, with textured piers, posts and ladders.
- Riding into a pier climbs onto its deck: the collision is solid.
- The beach is textured sand, and the seabed shows through the water.
- A ride crosses the low beach and beaches itself further in.

### The user's first look in VR (2026-11-03)

- The layout came over right, and the collision felt right.
- Fixed since: the scenery above the horizon was missing (the sky drawn
  over it), and the start was outside the collision.

### The second round (2026-11-03, the user at work; uncommitted)

The user's flat play-test: the layout and the visible collision were right,
but the jet ski was stopped by things nobody could see, hit a wall in open
ocean, saw the open backs of rocks and an unfinished stretch of coast to
the south, and the dolphin swam through the beach. They asked for Great
Bay's Zoras too. What was done:

- **The stops were the backs of walls.** Majora's Mask draws its cliffs
  facing Link's side only; from the sea they were see-through and solid.
  Every steep triangle (|normal.y| < 0.7) is now drawn from both sides,
  which fills in the open-backed rocks too. Collision standing in the water
  with nothing drawn near it (`mm_merge.Visible`: steep, through the
  waterline, under half of it within 60 of drawn scenery) is left out: six
  triangles, among them a 750-tall pair in open water to the north-west.
  (Ruled out on the way: the collision conversion -- func_8007F448's test
  matches -- hidden shallows, Dolphin Park's floating things and its ramps'
  slope pieces, all checked.)
- **Zora Cape joined on** (`Z2_31MISAKI`, scene file 1332): its world is
  Great Bay Coast's moved (0, 0, 6981), found from 17 vertices the two
  scenes share exactly. Each scene draws a rough copy of the other's land
  near the seam; in each 500-unit cell the scene with more triangles round
  about owns it and the other's scenery and collision there are left out
  (`mm_merge.Owners`).
- **Textures only, not whole files.** The pack used to hold the scene and
  room files as they are; each load command (`G_LOADBLOCK`, `G_LOADTILE`,
  `G_LOADTLUT`) now says how much of its image it reads and only that is
  copied (`Atlas`): 160 KB of textures for both scenes and the Zoras.
- **Its people** (`tools/guest/mm_character.py`, `src/guest_people.cpp`):
  five Zoras where Great Bay Coast's scene places them (its actors 0x224
  and 0x260, in both day setups) and Link. Each model's limbs are
  converted once, in the limb's own units, each vertex loaded under the
  matrix it was loaded under in Majora's Mask (a flex skeleton's lists load
  other limbs' matrices from segment 13 to bend across joints: in WR's
  microcode, `G_VTX` groups at their own place in the vertex buffer, v0 * 5
  in bits 16-23). The game poses them every frame from the pack's
  animations (SkelAnime's way: the parent's matrix, the joint's
  translation, rotations about z, y, x; between two frames), and writes
  each limb's matrix, the scene's scale folded in, loaded whole.
  - **Zoras** (`object_zo`, file 758; skeleton 0xD208; treading water
    0x4168, its run 0xE400 as the swimming stroke, turned face down). Each
    has a personality -- wanderlust, laziness, sociability, curiosity,
    shyness -- and wants that grow (to roam while it treads, to rest while
    it swims, company always), weighed when it next decides between
    treading water where it is and swimming off: somewhere new, to another
    Zora, or over to look at the jet ski. A shy one darts off from a jet
    ski passing fast and close. They swim only in open water: a grid of
    100-unit squares, open where the collision at the square's middle is
    below -60.
  - **Link** (`object_link_child`, file 654; skeleton 0x1E244, LodLimbs;
    the player's animations in `link_animetion`, file 7: each frame 22
    joints then a u16 for the face). By turns he fishes from the landing
    platform under the Marine Research Lab (the deck at 23; standing, a
    Deku Stick from gameplay_keep, 0x3248, in his left hand), or swims out
    off the beach, dives, comes up and holds up a green rupee (gameplay_keep
    0x62288, its colour 0x61FC0), treads water, dives again or swims back.
    He goes from one to the other only where the player can see neither.
  - The names and offsets came from 2Ship2Harkinian's archive (matching
    texture and animation bytes); the tables from the ROM's code file: the
    object table at 0x11CC80 and the actor table at 0x109510 (each actor's
    init data names its object). The tools themselves read only the ROM.
  - Test aids, in a scripted run: `WR64_PEOPLE_SHOW=<person>,<x>,<z>` draws
    one there, `WR64_PEOPLE_LINK=dive` starts Link swimming out;
    `WR64_PEOPLE_LOG=1` logs their decisions.
- **The coast goes on.** The ridge along the north-east (its triangles
  east of x 500 and north of z -3000) is drawn and solid again, end to end,
  four times beyond the map's north edge and twice on from Zora Cape's
  south-east end (`COAST_COPIES`), and the Free Roam current turns the jet
  ski back past the pack's bounds (x -9500 to 6500, z -13000 to 18500, Wave
  Race units) as past the buoys on the game's own courses. (Copying the
  whole east side first cut through the land by the beach, and its copies'
  mountains hung over the water.)
- **The base course's animals are off** (the kinds in D_801CF060 held at 0):
  Dolphin Park's dolphin and fish followed Dolphin Park's water.
- **Memory:** the pack may now reach 0x4C0000 (768 KB; it is 715 KB); the
  collision's planes follow it (room for 2944), then its lists and grid.
- **The ridge** along the north-east is a backdrop (its lowest point 826
  up): its copies beyond the map's ends hung in the air, so each copy's
  bottom edges get a skirt of its rock down below the water, and it is
  made solid there (the ridge where it is keeps neither: it hangs over the
  bay's north, and skirts walled the water off).

### The third round (the user's flat play-test, 2026-11-03 evening; uncommitted)

Ten F2 marks, and the stops the ride logged (`WR64_GUEST_STOPS=1`), drawn
on a map of the collision:

- **The stops in open water were Dolphin Park's shore.** The base course
  has a third collision besides the planes and the ramps' slope pieces: a
  grid of upright walls as lines on the map (`D_801924F0`–`38`, set by the
  course's loader `func_80071E70`), which `func_80051740` pushes the hull's
  points off and stops them going into (src/game/code_BF40.c). Its extents
  (`D_801924FC`, `D_80192508`) are now held at 0 on a guest course. A
  scripted ride through the user's marks 1 and 8 had stopped dead at
  x −3910, where the same ride on Dolphin Park climbs its beach; with the
  walls emptied it goes straight through.
- **The start held the jet ski** for its first 240 frames: placed again
  each frame, it couldn't get going. It is now let go once it moves off
  the start (by 3 to 1000 units: further is the game putting it back on
  its line, placed again).
- **The north and south lines** are where the user marked them (marks 5
  and 7, the seafloor's ends): z −4737 to 15049, Wave Race units.
- **Sand under the void:** a flat floor of the seafloor's own rippled sand
  (scene_texture_05 at 0x5000, its material copied from the room's list),
  15 m down (500 units), 6400-unit squares from x −44000 to 24000 and
  z −44000 to 52000 (Majora's Mask units, drawn at half under a matrix of
  its own, past what a vertex's s16 holds).
- **Deep collision left out:** nothing whose top is below −150; the ride
  never reaches it (it was not what stopped the jet ski, but it was half
  the planes).
- **Solid where it stands in the water:** the fish statues (mark 3) and the
  spikes (mark 6) had no collision; any triangle of the scenery rising
  through y 30 with no raised collision within 400 is now solid (115, the
  flat sheet at the scene's origin, a billboard's, left out).
- **The ridge where it is** now has its skirts and collision too: from the
  south its peaks hung in the air (mark 7), and what it walled off is past
  the south line now. **Another copy past Zora Cape** (mark 4): the cape's
  mountains end at z 13000 and the first copy began at 19000, its open end
  in plain sight; one more fills the gap.
- **Link** stands on the lab's landing slab (y 15, x −2989 to −2789,
  z 3898 to 4298: the box at the foot of its ladder), facing out; the
  first place, (1300, −1750), was by the eastern cliffs, nowhere near it.
- **After Great Bay, no seafloor anywhere** (the user's Drake Lake): the
  pack fills the seafloor's vertex area, and the seafloor checks that area
  is free before its first build. It is now cleared for it once a pack has
  been there (src/seafloor.cpp, `guest::took_vertex_area`).
- **The picture:** `tools/guest/make_thumbnail.py <image> <folder> [box]`
  makes `thumbnail.bin` beside the pack (284 by 210, RGBA 5551) from a
  screenshot (the user's mark 10); the GUEST COURSES page draws it in its
  square (src/game_menus.cpp). Like the pack, it is made on the player's
  machine; for others it would have to come from their own ride, or be
  drawn by the pack's builder.

### Still to do

- **Music:** the Great Bay Coast theme needs a recording (a WAV, as the
  replacement music is); turning Majora's Mask's sequence into one means
  playing its sequence, instruments and samples, not yet written. Where
  they are: both Great Bay Coast and Zora Cape play sequence 0x10 (their
  scenes' sound command, 0x15); the code file (31) holds the sequence table
  at 0x13BB70 (128 entries, offsets into Audioseq, file 4: sequence 0x10 at
  0x16360, 0xCF0 long) and the sound font table at 0x13B6C0 (41, into
  Audiobank, file 3); the samples are Audiotable (file 5).
- **Looks:** the baked sun is a guess; the other actors (the Marine Lab,
  the Fisherman's hut, trees, the turtle) aren't converted. At SCALE 0.83
  Link is smaller than Wave Race's rider, worth a look in the headset.
- **People:** not seen in VR yet; the Zoras' and Link's matrices change
  with the game's frames (20 a second), which may look stepped at 72.
- **The pack** built from the launcher rather than by hand. (Its picture
  ships with the release from 1.3.1, `guest\greatbay.rgba16` by the
  executable, packaged by tools/package_release.ps1 from the user's own;
  the user chose to bundle it, 2026-11-03. GREAT BAY shows on the page with
  COMING SOON and can't be chosen unless WR64_GUEST_OPEN=1.)
- **The dolphin** is off on a guest course (Dolphin Park's swam through
  the beach); one that knows Great Bay's water would need its own path.

### Showing guest courses in the menus (proposal, 2026-11-03)

Three ways, for the user to choose between:

1. **A course of its own after Southern Island**, with a miniature like the
   game's own. Looks native, but each needs a model, and the list grows a
   row per guest: crowded once there are several.
2. **One GUEST COURSES row after Southern Island**, opening a list laid out
   like the course select, a picture of the place where the rotating model
   would be. Room for any number; the picture can be rendered by the
   pack tool from the scenery it already has (nothing hand-made, nothing of
   the other game shipped).
3. **2, with the miniature made for it**: the same GUEST COURSES list, but
   its preview a small rotating model of the guest course built by the
   pack tool from its own scenery -- the land above the water, few
   triangles, in the game's own style -- as the game's course select shows
   its courses. Native-looking without anything drawn by hand, and it
   scales as 2 does.

Recommended: 2 first (quick, and sound however many courses there are),
growing into 3 once a miniature can be made automatically.

Research before any of this (2026-10-31):

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

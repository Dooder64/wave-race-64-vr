# What a race leaves out, and drawing it for a headset

A headset looks in every direction, while Wave Race 64 draws only what its flat
camera can see. This records what the game leaves out, where it decides, and
how VR mode changes it. Addresses are USA Rev A.

## Measured: the cull survey

`WR64_CULL_SURVEY=1` (src/dlrewrite.cpp) walks every race frame and files each
block of vertices drawn by its direction from the camera's heading and its
distance, and each drawing list the frame calls by the widest angle and the
farthest distance it was ever drawn at. From `tools/scripts/race.txt`:

| Drawing lists | Widest angle drawn | Farthest |
|---|---|---|
| `0x0102CC58`-`0x0102CD90`, `0x01029BE0`, `0x01029C88`, `0x0102AD90`, `0x0102AE38` | 63-67 degrees | about 5000 |
| `0x08065A00`-`0x08065E00` | 74-81 degrees | about 2540 |
| `0x0D015AC0`-`0x0D015F00` | 88-89 degrees | about 5000 |
| `0x080617F0`-`0x08061AB8` | about 100 degrees | about 5900 |
| `0x0802C020`-`0x0802C8C0`, `0x8029Fxxx` | 171-176 degrees: drawn all round | 4000-7300 |

The water (segment 3) exists from about -60 to +30 degrees of the heading and to
about 1500 units; the sky (segment 6) within 20 degrees of it.

## Where it is decided: func_8006E674

One function decides, every frame, for three groups of objects. Its results are
read only by drawing code (func_8006B334, func_8006CB98, func_8006CDE8,
func_8006D034, func_8006D494, and itself), so changing them changes what is
seen and nothing that is played. It also draws two of the groups itself, so it
cannot simply be run more than once.

For each object of a group it measures the flat distance from the camera
(position at offsets 0x4C and 0x54 of the camera), and

- leaves it out (-1) past a cut-off read from the course's settings
  (`*(D_801C0C80)` + 0xA4, doubled for some objects);
- leaves it out when it is off to the side: the cosine between its direction
  and the camera's (offsets 0x64 and 0x6C, flattened and normalised) must
  exceed 0.5 for the first two groups and 0 for the third, with a few object
  types exempt;
- otherwise writes a fade from 0 to 255 between a start distance (+ 0xA0) and
  the cut-off.

| Visibility array | Objects | Distance test | Direction test | Limit |
|---|---|---|---|---|
| `D_801C0840` | `D_801AEE20`, stride 0x104, count `D_801BB120` | `0x8006EAB4` | `0x8006EAF4`, `c.lt.s $f10, $f8` | 60 degrees |
| `D_801C08C0` | count `D_801BC938` | `0x8006EC60` | `0x8006ECA4`, `c.lt.s $f8, $f4` | 60 degrees |
| `D_801C0AC0` | `D_801BC940`, count `D_801BFA40` | `0x8006EDBC` | `0x8006EDFC`, `c.lt.s $f24, $f4` | 90 degrees |

The start and cut-off distances are loaded once into s1 and s2
(`0x8006E9A4`, and `0x8006E9AC` in a branch's delay slot) and serve all three
groups.

## What VR mode changes

Recompiler hooks in `recomp/wr64.toml` add a line before the deciding
instructions; each asks src/vr.cpp first, and changes nothing unless VR mode
(or `WR64_CULL_ALL_ROUND=1`) wants objects all round:

- before each direction test, the threshold is put below any cosine (or, for
  the third group, whose threshold register holds zero for the whole function,
  the cosine above any threshold). Each register changed is written again
  before anything reads it, checked in the generated C;
- at `0x8006E9B0` and `0x8006EBC0` (where the branch skipping an empty first
  group lands) s1 and s2 are read again from the course's settings and scaled
  by `WR64_VR_DRAW_DISTANCE` (2 by default), so objects fade out that much
  farther away rather than being cut off.

Drawing more objects uses more of the frame's 454 matrices (`Mtx` at 0x4140 of
the frame buffer, up to the `LookAt` at 0xB2C0, counted by `D_801AE950`); the
peak is logged in VR mode.

## Not yet

- The lists at about 100 degrees (`0x080617F0`-) are left out somewhere else.
- The sky is built only in front of the camera.

## The water all round

The game's wave mesh (list `0x0100B590`) is a hexagon of waves about 1500
units across, on a triangular lattice fixed to the world (64 units along x,
rows 55.43 apart along z, odd rows offset by 32), its border flattened to the
water level, inside a ring of six flat quads -- all translucent. Only where the
hexagon is centred depends on the camera: about 700 units ahead of it.

`patches/water_all_round.cpp` builds the mesh again for the camera turned 90,
180 and 270 degrees (and puts back everything the builds change), merges the
four point by point on the lattice -- an average weighted by how far inside
each hexagon a point lies, so flattened borders drop out -- and writes one
hexagon mesh with its own flat ring out to 6000 units. `src/dlrewrite.cpp`
draws it in place of the game's. Drawing the four meshes on top of one another
was tried first: the overlaps were doubly opaque, each flat ring cut through
its neighbours' waves, and the modern water renderer saw four surfaces.

`WR64_WATER_DUMP=<file>` (with `WR64_WATER_DUMP_AT=<frames>`, 300 by default)
writes one frame's merged mesh to the file and the game's own to `<file>.game`,
as display-list commands and vertices.

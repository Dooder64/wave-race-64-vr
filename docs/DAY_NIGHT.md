# Day and night in Free Roam: research

Idea (the user, 2026-10-31): in Free Roam, time passes. Each course's look
stands for a time of day, and each one fades into the next, a whole day
taking 24 minutes (a game hour a minute).

Status (2026-10-31): **built, flat-tested, uncommitted, awaiting play-test.**
The Free Roam options page has a DAY AND NIGHT row (OFF/ON, setting
`free_roam_day`). Code: `src/day_night.cpp` (the clock, the keyframes, the
sky pictures), `patches/free_roam.cpp` (`day_frame`: the race's look each
frame), `src/water.cpp` and `src/first_person.cpp` (our water and swim haze
blended). Test switches: `WR64_DAY=1`, `WR64_DAY_SPEED=<n>`,
`WR64_DAY_HOUR=<h>`, `WR64_DAY_LOG=1`. Scripted flat runs on Dolphin Park
and Sunny Beach at 24x speed went through night, dawn, day, sunset and night
again cleanly.

### What the build found

- **The race's look is live.** Writing the race's copy (`0x801CB058`, two
  of them, one per view) each frame changes the fog, the water, the sun's
  highlight and the riders' lighting straight away. Nothing else was needed.
- **The look's fields**, from all nine laid side by side (`tools/daynight/looks.py`):
  `+0x00`/`+0x0C`, `+0x18`/`+0x24`, `+0x30`/`+0x3C` are light and ambient
  colours (Sunset Bay's ambients are orange), `+0x48` the scene fog colour,
  `+0x78`/`+0x7C` its range, `+0x88` the water fog colour, `+0x98`/`+0x9C`
  its range, `+0xA0`/`+0xA4` draw distances (kept per course), `+0xAC..B4`
  and `+0xFC..104` light directions (floats). Not known: `+0xA8`, `+0xC4`,
  `+0xC8`, `+0x108`, `+0x10C`, which are kept as the opening look's.
- **The sky's pictures** are the list each course's `0x800DC0E4` entry points
  to: seven entries of 16 bytes (cartridge start and end, a kind, a place),
  each an MIO0 chunk. Unpacked: 0 and 1 the gradient (16x16 RGBA16), 2 and 3
  clouds (32x64 IA16), 4 the water's texture (32x64 RGBA16), 5 the sun (IA16;
  Dolphin Park's 32x32, others 32x64), but on Sunny Beach, Drake Lake, Marine
  Fortress, Port Blue and Southern Island the water's texture again, and 6
  far clouds (32x64 IA16). They are found in memory by their contents once a
  ride starts, then rewritten as the blend; picture 5 blends only with its
  own kind.
- **HD pack:** its replacements are keyed by a picture's contents, so during
  a fade the sky shows the original pictures. Held looks match a course's
  own, so they get its HD pictures if the pack has them.

### Not done yet

- A chosen hour (rather than starting at the course's own) and a speed
  choice.

(Done since: DAY AND NIGHT has its HD replacement in the pack, and while it
is on, the SKY row is drawn grey, style 7 with tints of 0x80 as the game
greys a locked difficulty.)
- Tested flat only, not in VR.

## What a look is made of

From `patches/free_roam.cpp` (the SKY option) and the decompilation:

| Part | Where | Read when |
|---|---|---|
| Light, fog and sky colours, fog distances, the light's direction | the look table, `0x800D9964 + course*0x110`, copied for the race to `0x801CB058` (`D_801C0C80` points at it) | **every frame**: the scene's fog colour and fog factor are written into the frame's list from it (`code_24B00.c`: `gDPSetFogColor`, `gSPFogFactor` from `+0x48..+0x54`, `+0x78`, `+0x7C`), and the sun's highlight on the water from `+0xAC..+0xB4` (`guLookAtHilite`); the water's fog from `+0x88..+0x94` (`code_43DA0.c`) |
| The sky's pictures: the gradient, clouds near and far, the floor under the water | seven packed textures per course, list at `0x800DC0E4`, unpacked into segment 8 as the course loads | **once**, at load |
| The port's own modern water and haze colours | ours, per course (`look_course()`) | every frame |
| The land itself | the course's models (textures and their shading) | it's the course's own |

So the colours, the fog and the sun can be changed during a ride by writing
the race's copy (`0x801CB058`) each frame, with no help from the game. Only
the sky's pictures need more work.

## How the fade would work

- **The clock.** Free Roam counts time: 24 minutes a day, starting at a chosen
  hour (a new Free Roam option, TIME: a fixed hour, or MOVING).
- **Keyframes.** Each time of day is one course's look. The two looks either
  side of the hour are blended by how far between them it is:
  - colours mixed channel by channel;
  - fog distances mixed;
  - the sun's direction turned between the two. A day could also turn the sun
    right across the sky, so the highlight on the water moves with it.
- **The sky's pictures.** All nine courses' seven textures are unpacked once,
  at the start of a free roam, into the mod's free memory. They're small. Every
  few frames, the two keyframes' textures are mixed texel by texel into
  segment 8, where the game already reads them. That needs the textures to be
  the same size and format for every course. It looks likely, since the game
  unpacks every course's into the same places, but check first.
- **Our water and haze.** Blend them the same way, between the two courses'
  colours.

## Which course for which hour (to check by eye)

A first guess, from each course's character. Look at each one before
settling it:

| Time | Look | Why |
|---|---|---|
| Dawn | Drake Lake | misty, soft light |
| Morning | Dolphin Park or Sunny Beach | bright, clear |
| Noon | Southern Island | tropical, high sun |
| Afternoon | Port Blue | clear, warmer |
| Sunset | Sunset Bay | its name |
| Dusk to night | Twilight City | the only night sky |
| Overcast or stormy | Marine Fortress, Glacier Coast | weather rather than an hour; maybe a separate WEATHER choice later |

With only one night look, night is the weakest part. Midnight could be
Twilight City's look darkened further by us, with the fog darker and closer.

**Timing.** The hours needn't be even. For example: dawn 2 minutes, day 12,
sunset 3, night 7, making 24 minutes in all.

## The hard part: the land

Fog, sky and water carry most of a time of day, but the land is drawn with
the course's own colours. A bright beach under a night sky will look wrong.

- First, find out whether the courses' land is lit by the look's light (then
  it would follow by itself) or has its shading baked into vertex colours
  (more likely).
- If it's baked, darken and tint it ourselves. Either rewrite its colour
  commands through the display-list rewriter (`src/dlrewrite.cpp`), or apply
  a scene colour multiply in our RT64 changes. Both are within what the port
  already does.
- At night the fog can do much of the work, since a dark, nearer fog hides
  bright land in the distance.

## Nice extras, later

- Stars and a moon. Twilight City's sky may already have them. If not, draw
  them ourselves.
- Lights on buildings at night (Twilight City has them; other courses don't).
- The Dolphin Park riders going home at night.
- Glowing water or fireflies.

## Steps

1. **Measure.** Dump all nine looks and sky texture sets. Check that the
   sizes and formats match, and look at each course's colours.
2. **Live colours.** In a free roam, write the race's look each frame: first
   the fog colour, then the sun. Confirm the change shows straight away, in
   flat and in VR.
3. **The sky.** Unpack every course's sky textures and mix two of them into
   segment 8 live.
4. **The clock.** The keyframe table, the 24-minute day, and the TIME option
   on the Free Roam page.
5. **The land.** Darken it at night (see above).

Steps 1 to 4 look like a few sessions' work, with no known blockers. Step 5
is the open question.

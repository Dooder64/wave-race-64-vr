# Target mode and daredevil mode: research

Ideas (the user, 2026-10-31):

- **Target mode.** Hit a jump, bail out on purpose, and send the riderless
  jet ski into a target, with funny results. Bowling pins or similar,
  nothing destructive or questionable.
- **Daredevil mode.** Single, precise, dangerous stunts, one at a time, in
  the spirit of Driver's parking-garage test or Stuntman: Evel Knievel on a
  jet ski.

Status: research only. Nothing is built or tried yet.

## What's already there

- **Bailing on purpose** works now: X (F3) throws the rider by setting the
  countdown at rider state `+0x1608` to 1 (`patches/swim_back.cpp`).
- **After a throw the jet ski carries on** and drifts to a stop under the
  game's physics. The swim back already deals with it coasting
  (`src/first_person.cpp`).
- **The jet ski's motion** is rebuilt each physics step from its speed
  (`+0x68`, about 12 at full throttle) and heading. Free Roam's current
  already pushes on those (`patches/free_roam.cpp`), so we can read the
  riderless jet ski's path, and nudge it if needed.
- **Placing ramps and objects** is course-creator stage 1
  (docs/COURSE_CREATOR.md). Both modes need it.
- **Putting the rider at a set place** for a scripted start exists in test
  form (`test_place` in `patches/free_roam.cpp`), which gives instant
  restarts.
- **The replay cameras**: `D_801CE638` set to 0 gives the game's replay views
  without the HUD (RESEARCH.md). That might do for a "watch it again" shot.

### To find out first

1. **A throw in mid-air.** Does X work while airborne, and does the jet ski
   keep its flight? Target mode depends on this. A quick test: hit the
   Dolphin Park ramp and press X at the crest.
2. **What Stunt Mode knows about tricks.** It scores rolls, flips,
   handstands and so on, so somewhere it records which trick was done and
   whether the landing was clean. Daredevil mode needs those for its
   requirements, such as "a barrel roll over the gap".

## Target mode

**Targets are ours.** Pins, stacks of crates, a giant beach ball, rubber
ducks: original models, built and drawn by us, with a small physics
simulation of our own. Pins topple, slide and splash; the game doesn't need
to know they exist.

**The hit.** Each frame, check the riderless jet ski's position and speed
against the targets' shapes. On contact, the pins get pushed by its speed
and direction and knock into each other. The jet ski itself can carry on as
the game moves it, or we can slow it on impact (it's writable).

**The fun:**
- A bowling lane on a floating platform, ten pins: strike and spare scoring,
  ten frames, a score card in the game's lettering.
- Variety: a pyramid of beach balls, a line of dominoes along a pier, a
  giant dartboard on the beach (score by ring), a ring of rubber ducks.
- A slow-motion replay of a good hit, the rider's own reaction (the climb
  back on is a natural "watch"), and the announcer.
- Each challenge has a set ramp and a target. Hit it from the run-up you
  choose: speed, line and when to bail are the skill.

**Size: medium**, once ramps can be placed. The pin physics and models are
the new work.

## Daredevil mode

**Shape.** A list of single challenges, each a short setup. You start at a
set place, already moving, with a goal. You fail by falling, missing the
landing or running out of time, and get an instant retry. Medals go by how
precise it was, and an attempts counter keeps score.

**What a challenge is built from** (all of it data, so made courses can carry
challenges too):
- placed ramps, at any angle and height;
- **gates**: an area to pass through, at a height, sometimes in a direction
  (through a hoop, under a pier, between two pillars);
- **landing zones**: a patch to land in, scored by how near the middle and
  how clean;
- **trick requirements**: a roll, a flip, a handstand, or none, in the air
  over a given part;
- limits: a time, a top or bottom speed, no touching the water between two
  points.

**Example challenges**, mostly on existing courses:
- Ramp to ramp: jump from one ramp onto another and off again.
- The gap: clear Twilight City's canal from one side to the other, landing in
  a narrow zone.
- Threading: at full speed between Dolphin Park's pier pillars without
  touching.
- The needle: through a small hoop at the top of a jump.
- The long one: as far as possible off the biggest ramp, beating a mark.
- Combination: a roll over the gap, then under the pier, all in 15 seconds.
- Rough water: a narrow run of gates in the highest waves (the full wave
  range already allows any level).

**Feedback:** a ghost of the best attempt, the medal times or distances shown
before each try, and a slow-motion replay when you nail it.

**Size: medium-large.** The engine (gates, zones, trick checks, instant
retry) is moderate. Making many good challenges is the long part, which is
level design, best done in the course creator.

## How they fit with the other ideas

- **Ramps and objects** (course-creator stage 1) are needed by both. Build
  those first.
- Challenges and targets are just placed objects with rules, so the course
  creator could make new ones, and players could share them as files.
- Free Roam riders (docs/RIDER_PERSONALITIES.md) could gather to watch a
  daredevil attempt, or have a go at the bowling lane themselves.

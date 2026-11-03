# Rider personalities: research

Idea (the user, 2026-10-31):

- **Racers** with personalities:
  - aggressive riders who'll ram anyone to win;
  - cautious, sporting riders;
  - specialists in cornering, jumps, or control in rough water.

  All of them judge when to pass or make a move. That would also let them
  race made courses (docs/COURSE_CREATOR.md).
- **Riders out on the water** (Free Roam) with personalities of their own,
  deciding what to do for fun. For example, following you for a while and
  then riding off, or noticing something new, like a place or an activity,
  and choosing to go after it.

Status (2026-10-31): **Free Roam riders' minds built, flat-tested by logs,
uncommitted, awaiting play-test.** Racers: research only.

### What's built (Dolphin Park's three riders)

- `src/rider_minds.cpp` (`include/wr64/rider_minds.h`): the decisions.
  Six personalities (daredevil, sightseer, social, loner, show-off,
  practiser), each a weight for six wants (thrill, sights, company, showing
  off, rest, curiosity) and a patience. Wants rise when unmet and fall
  while an activity satisfies them (a table of how much each activity
  satisfies each want). When patience runs out, each activity open to the
  rider is scored by want x weight x satisfaction, with some chance, and the
  one just done counted down.
- `src/companions.cpp`: the first rider and the pair (who count as one mind,
  since they go everywhere together) each get a personality by chance at
  each outing.
  - The first rider's activities: its stunt loop, laps of the park (or
    Stunt Mode practice), plus new **Follow** (rides beside the player, a
    little behind, matching its speed, for 20-60 s, then peels off), **Rest**
    (rides to a quiet sight away from the player and the pair, stops and
    faces the view) and **Watch**.
  - The pair's: sightseeing, the weave, the jump together, plus **Follow**
    (one either side of the player), **Rest** (a long stop at a sight) and
    **Watch**.
  - Routines now change whenever the mind tires of them, not only at the
    first rider's old changeover point.
- **Noticing:** the player's position comes from its rider record each
  frame. Big air (more than 160 units above the water) is news for 15 s.
  Each mind may drop what it's doing and ride over to watch from about
  1000 units away: likelier the more curious it is and the nearer it is, and
  not again within 25 s. Watchers face the player while it's still near
  where it happened. A show-off who sees it wants to show off more.
- **Test switches** (scripted runs only): `WR64_COMPANION_TEST_MINDS=<first>,<pair>`
  picks personalities; `WR64_COMPANION_TEST_CHANGE=<s>` shortens every
  activity; `WR64_COMPANION_TEST_AIR=<s>[,x,z]` fakes the player's big air.
  `WR64_COMPANION_LOG=1` logs each choice with the wants behind it.

### Seen in the tests

- Choices follow the personalities: a social first rider went to ride along
  with the player and caught up across the park (from about 4000 to 12000
  in x) at up to 1300 units a second, then slowed to the player's speed; a
  show-off pair chose the jump together; a sightseer pair rested by the piers.
- A sightseer pair at rest noticed a big air about 1100 units away, rode
  over, stopped side by side facing it, and went back to their own plans
  about 11 s later.
- The Warm Up starts the player at the far eastern beach (x about 12000),
  so most of the riders' world is far from the player there.

### Not done or not checked

- Not seen in screenshots yet: the scripted player kept getting stuck in
  the Free Roam current far out, so the checks are from logs only.
- Nothing else is news yet: placed ramps, the dolphin, another rider's stunt.
- The two pair riders can briefly come within about 30-80 units of each
  other when changing plans (seen in logs; this was so before as well).
- Riders still only on Dolphin Park.

### More riders (2026-11-02, not play-tested)

Eight riders besides the player: the first rider, the pair, two more
singles and a trio. Each single and the trio has a mind of its own (minds
2, 3 and 4). The singles are copies of the first rider: its routes and
ramp jumps, resting, riding along and watching. The trio's leader has the
same choices without the ramps, at 750 units a second. Its two wingmen
keep a wedge, 300 back and 280 to either side, and come up beside it when
it stops.

There are only four characters, so the new riders reuse the three racers'
models already unpacked. Each has its own matrices at 0x59A000 + 0x1000 i,
with room for six. They throw no spray yet: `src/body_splash.cpp` has
three slots. They are not part of the dolphin's audience yet.

They are on for Dolphin Park only. `WR64_MORE_RIDERS=0` turns them off;
`WR64_MORE_RIDERS=everywhere` puts them on every course. Scripted runs on
Dolphin Park and Sunny Beach, 2-3 minutes each, showed no crash; the
riders were fully drawn and the trio held its wedge.

### Ramps away from the park (2026-11-03, not play-tested)

Riders now ride a course's ramps by the game's own slope pieces
(`D_801BFD78`, moved to 0x80800000 by `patches/ramp_pieces.cpp`). Each is
a solid box: a corner and three axes with lengths, a point inside when
`(corner - p) . axis` is within `[0, length)` for each, as
`func_8007FAE4` tests the jet ski. `ramp_surface(x, z)` is the top of the
pieces there. Each ramp's crest is the highest point on the line through
its middle the way it is ridden, which is where its last climb ends.

Sunny Beach's ramp is filed at (-619, -1065), but its crest is at -1124,
80 up, with a face 250 long climbing 0.56 at the end. Riders had been
riding Dolphin Park's ramp shape, finished at the filed point, where the
real ramp is still under water. Now they slow up the face and leave the
crest at the angle of its last climb, at a speed that brings them down
1300 past it: 647, with a pop of 362. That leaves room for a roll and
lands short of the mines. Route maps also keep off the pieces' whole
footprint, whose corners reach past the old round no-go zone.

`WR64_OBJECT_DUMP=1` also prints every object's words and every piece.

## Two different kinds of rider

| | Free Roam riders | Racers |
|---|---|---|
| What they are now | our own puppets (`src/companions.cpp`): the game's models on routes we steer, swerving round the player | the game's own computer riders, real jet skis under the game's physics |
| Who decides | entirely us | the game's AI, which isn't decompiled (its code is still assembly) |
| Collisions | none (they steer clear) | real: they bump, block and get bumped |
| Hard part | the decision-making | getting control of them at all |

So the two halves are separate projects that can share one personality
model.

## Racers: control them like a player

**The plan:** don't rewrite the game's AI. Replace what it decides: its
steering, throttle and moves. The game's physics, waves, collisions, buoys and
misses then stay exactly as they are, so our racers ride like real riders
and can't cheat. Personality only changes the decisions:
- which line to take;
- when to cut inside;
- whether to back off;
- how hard to push in rough water.

**Found (2026-10-31): the AI drives through a virtual controller.**
`func_80061708(rider, controller)` is each rider's input step. For a human
it copies the controller (buttons, stick) into the rider's state at
`+0xB50` (buttons, u16), `+0xB52` (stick x, s8) and `+0xB53` (stick y, s8),
then `func_80061184` handles them. When the rider's `+0x1446` is negative
it's a computer's rider, and `func_800616B0` runs instead. That calls the
AI, `func_80080400` (code_3AC00: 29 functions, about 8400 lines of
assembly, not decompiled), which writes those same fields, then the same
`func_80061184`. So **the game's computer riders press virtual buttons and
push a virtual stick**, exactly as a player does.

That means step 2 below applies: let `func_80080400` run, then overwrite
`+0xB50`/`+0xB52`/`+0xB53` with our racer's decisions (a patch wrapping
it). The physics, collisions, buoys and misses stay the game's own. Still to
find: what the AI steers by, for borrowing its sense of the course.

**Plan as first written** (the hand-over point is now found):
1. Find the code that turns a computer rider's choices into steering and
   throttle, using the recompiler's hooks and RAM traces. The game can switch
   any rider between human and computer (a retiring rider's jet ski is handed
   to the computer, RESEARCH.md), so there is one place where they meet.
2. If the AI writes stick- and button-like values, hook that and write ours.
   Every decision is then ours, and everything else stays the game's.
3. If instead it moves riders directly (setting speed or heading, or
   rubber-banding), keep its movement and steer its target instead.
   That's less control, and still personality.

**What racers need to know**, all of it readable: every rider's position,
speed and heading (rider state `0x80192690 + slot*0x1718`), the buoys and
which side each is passed on (`D_801AEE20`), the wave level, places and laps
(race records).

### The racer's mind, in three layers

1. **Plan: the line.** Worked out from the buoys alone: the inside of each
   turn, set up for the next. No hand-made data is needed, so it works on
   any course, including made ones. Specialists bend it:
   - a cornering specialist takes a tighter, faster line;
   - a cautious rider goes wider round traffic.
2. **Tactics: the moves, a few times a second.** For each rider near by:
   - Is there a gap?
   - Is the next turn one where the inside line is open?
   - Is there a straight to draft and pass on?
   - Is the rider ahead struggling in the waves?

   Each move is scored by gain and risk. Personality decides what's worth
   it:
   - The aggressive rider counts a collision as a gain if it knocks the
     other rider off the line.
   - The sportsman counts it as a cost and gives room.
3. **Control: the hands.** Steering towards the plan's next point, with
   errors and reaction time by skill:
   - a rough-water specialist reads swells and keeps the throttle on where
     others ease off;
   - a jump specialist lines up ramps and lands clean.

### Personality as numbers

Each racer gets a handful of traits from 0 to 1: aggression, sportsmanship,
risk, cornering, jumping, rough water, consistency (how often they make
mistakes), and nerve (how they handle being passed). The four characters
could each get their own set, with the championship difficulty raising all
the skills. Traits could also show in small ways, such as an aggressive
rider's riskier stunts when well ahead.

## Free Roam riders: a mind for the puppets

Here nothing of the game's needs to be found. It extends what the Dolphin
Park riders already do: a loop with stunts, a pair touring.

**Utility AI.** Each rider has wants (thrill, sightseeing, company, showing
off, rest, curiosity) that rise and fall over time. Each activity on offer
scores by how well it meets them, given who the rider is. The rider does
the best one, with some chance in it, and gets bored after a while:

| Activity | Satisfies |
|---|---|
| Ramp runs, rings, stunts | thrill, showing off |
| Touring the course's sights | sightseeing |
| Following the player for a while, then peeling off | company |
| Racing the player to a point, or copying their stunts | showing off, company |
| Riding with another rider | company |
| Drifting slowly, resting by the beach | rest |
| Going to see something new | curiosity |

**"Something piques their interest":**
- Points of interest know when they're "news": a ramp just placed in the
  course creator, the player doing big tricks, a dolphin.
- Each one gives off interest for a while, falling off with distance.
- Curious riders pick it up first and head over. Others join if their friend
  goes.

**Memory:** what each rider did recently (so they don't repeat), and how
they feel about the player (a show-off who keeps getting beaten might start
challenging you).

**Personalities:** for example:
- the daredevil, who lives on the ramps;
- the sightseer;
- the social one, who follows people;
- the loner, who keeps to the edges;
- the practiser, who runs the rings and checkpoints;
- the show-off, who stunts near the player.

## Order

1. **Free Roam minds first.** Everything is ours already, so a utility AI on
   the existing puppets gives a visible result soon. It also fits the
   roadmap's "riders on every course".
2. **Find the racers' controls.** This is the research step, with the
   uncertain result above.
3. **A racer that drives by our line** with neutral traits. If it finishes a
   race cleanly, the method works.
4. **Tactics and personalities.**
5. **Made courses.** The line is planned from buoys, so made courses come
   free.

| Part | Size | Waits on |
|---|---|---|
| Free Roam personalities | medium | nothing |
| Racers driven by us | large | finding the AI's controls |
| Racing tactics and personalities | large | the above |

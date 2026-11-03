# The dolphin on every course: design and research

Idea (the user, 2026-11-01): put Dolphin Park's dolphin on every Free Roam
course where it makes sense -- all but Twilight City (a city) and Glacier
Coast (too cold) -- with a mind of its own rather than a path per course.

Status (2026-11-01): **built and play-tested on Sunny Beach (it works; the
user found no animation reads as a call, and expects the sound to fix
that), uncommitted.** Since then the pair ride every course too, and can be
its audience (flat-tested only). Code: the dolphin's section at the end of
`src/companions.cpp` (loading, drawing, its mind, its route-finding), the
riders' side in `src/rider_minds.cpp` (Activity::Dolphin, each personality's
`dolphin` liking, `dolphin_act`), the first rider's Free::Dolphin and the
pair's PairRoutine::Dolphin. Out
on every course but Dolphin Park (which has the game's own), in a free
roam: the user asked for Twilight City and Glacier Coast too on
2026-11-03. Test switches: `WR64_DOLPHIN_LOG=1` (its decisions, and where it is
every 2 seconds), `WR64_DOLPHIN_TEST_ANIM=<0-7>` (an animation in front of
the player, in a scripted run).

Not yet: its call has no sound (the game's own chirp would need calling the
game's sound function from the right thread). The game's sound-effect
entry is `AudioGeneral_PlaySound(s32, s32)` at 0x800C37F4, called from the
race overlays (codeseg A95D0, AF8C0). The dolphin's own sound id isn't known
yet; the plan is to log every call to it during a Dolphin Park ride near the
dolphin. Its audience: the player, the
lone rider, or the pair (taken with it together, by the pair's mind; they
watch it abreast some 850 off, or follow it side by side 600 behind).

Drawn after the riders: its setup list (0x800E6CA0) leaves the drawing state
its own, which drew riders after it grey and speckled; the companions' whole
drawing is wrapped in RT64's push and pop of the state.

How it finds its way: the course's open water as squares of 200 (each open if
inside the push-back boundary and deep enough), a breadth-first route search
through them, re-planned every 3 seconds or when its goal moves; it steers for
a point a few squares on, and keeps 450 clear of the jet skis. The stage it
performs on is the first open spot 700 ahead of the one it is with (further,
or up to 20 degrees aside, if that is shallow).

## The dolphin's mind (agreed with the user)

Its motive is people's attention: the player's or the Free Roam riders'.

1. **Seeking.** It picks a person -- the player or a rider -- and tries to
   catch their eye: a leap, its dance (upright on its tail at the surface),
   or its call. With no one biting within a random time of up to 30 seconds,
   it gives up on them and picks someone else.
2. **Attention won: a timer each.**
   - The dolphin's interest: random, up to about a minute, running down only
     while the person is paying attention. At zero it is bored of them and
     seeks another.
   - A rider's interest: random, 10 to 30 seconds. Each time the dolphin
     changes act (a leap, then a dance) and the rider enjoys it, another
     random 10 to 30 seconds. Repeating the same act adds nothing.
3. **One side bored first.** The bored one leaves; the other, frustrated,
   follows for a random 3 to 10 seconds trying to win them back, then gives
   up. If the dolphin wins a rider back in that time, both start afresh with
   new timers.

Filled in (agreed):

- Whether a rider enjoys a new act depends on its personality: sightseers
  and social riders nearly always, loners rarely, the others between.
- The player has no timer: the game judges the player's attention by what
  the player does -- slowed and facing the dolphin (watching), or riding
  behind it, the same way (following). A change of act while the player
  still watches buys the dolphin's interest more time, as a rider's
  enjoying it does.
- After tiring of someone, the dolphin leaves them alone for about a
  minute before it may pick them again.

Where it may go: deep enough water (the courses' sea floors give a depth
anywhere) and inside the course's push-back boundary (the game's own
rectangles, D_801C1F80, tested by func_80074264).

## What the game has (research)

The game keeps its animals in a table, D_801CF060: 30 entries of 0xBC bytes,
+0x00 the kind, +0x0C/+0x10/+0x14 the place. In Dolphin Park's Warm Up: kind
6, the dolphin; kinds 7, 8 and 9, a school of fish by the pier. On Sunny
Beach: fish (7, 8, 9) and five seagulls (kind 3). Movement modes seen in its
code (+0x08): 11 circles a point (+0x80/+0x88) at a radius (+0x70); 5 and 10
follow another animal (+0x7C).

Each course loads its own animals' bank at segment 8 + 0x60000 (segment 8
is 0x316800 at Dolphin Park), listed in D_801CE780 (0x2C bytes an entry:
how many animations, the address of their list, then the parts' display
lists); Sunny Beach's bank has no dolphin. So the dolphin is drawn by the
port, as the Free Roam riders are: its model loaded from Dolphin Park's data
in the cartridge, its movement our own.

**The bank.** Dolphin Park's is the MIO0 chunk at cartridge 0x3E17A0
(mio0_119), 0xC21C bytes unpacked; its own addresses are segment 8 counted
from its start (the game adds 0x60000 as it loads it). The dolphin is its
first entry: animations listed at 0xC1FC (eight), part lists at 0x2AB8,
0x3008, 0x31A8 and 0x2E60 (the flipper, drawn twice).

**How it is drawn** (func_8009FCB0, for kind 6): the animals' setup list
(0x800E6CA0); the dolphin's matrix (func_8009D96C: its place, its angles at
+0x18, +0x1C, +0x20, scale 0.15); then for each part a matrix multiplied
and pushed, the part's list, a pop. The part's matrix (func_801E4C60) is
built from its pose in this frame: an animation is its frame count (a half),
its poses (12 bytes each: the part's place, three halves; the way its y
points, then a way across, three signed bytes each in 127ths: x is across
the second and y, z across y and x), and for each part a pose number per
frame. The animal's +0x74 picks the animation, +0x78 counts its frames.
Parts and where their pose numbers are in the animation: the body (+0x08,
list 0x2AB8), +0x18 (0x3008), +0x14 (0x31A8, the tail), and the flippers
(+0x0C and +0x10, list 0x2E60). The nose is along the model's +z (the tail
at z -886).

**Its eight animations**, from the body's movement in each:

| # | Frames | What |
|---|---|---|
| 0, 3 | 16 | swimming: bobbing 30 either way |
| 1 | 54 | a leap, 1100 up (165 in the world) |
| 2 | 59 | a high leap, 2000 up (300) |
| 4 | 85 | a trick (used as its call) |
| 5 | 85 | upright at its start and end: the dance |
| 6, 7 | 85 | a trick, nose down in the middle |

The leaps' heights are in the animation itself; it moves on over the water
as its own swimming does.

## The decisions journal

Everything the Free Roam riders and the dolphin decide is written to
`%LOCALAPPDATA%\WaveRace64Recomp\decisions.jsonl` (src/journal.cpp), one
JSON object a line, kept from one game to the next: for talking over an
outing afterwards, and for tuning the minds -- by hand, or later by
learning from what happened. The Debug tab's "Record riders' decisions"
turns it off (on by default); `WR64_DECISIONS=0` too.

Every line: `utc` (to the millisecond), `t` (seconds since the game
started), `session` (when the game started), `event`, and where everyone was
(`course`, `player`, `rider`, `pair` when out, `dolphin`). The events:

- `rider`: a rider's mind decided (`who`: rider or pair; `why`: started, chose, noticed the
  player's big air, the dolphin): its `personality`, what it is now `doing`
  and `for` how long, its `wants` and `fondness`; for a choice, every
  activity `weighed` (its `score` by wants and personality, then the
  `chance` added -- the highest total wins); for a notice, its `pull` and
  `roll` (it goes if pull + roll reaches 0.6).
- `dolphin`: the dolphin changed what it's doing (`why`, `phase`, `with`
  whom: the player, the rider or the pair; its `interest` and `bonus_left` on winning attention; how long
  until it `gives_up_in`).
- `dolphin_act`: a leap, dance or call, `for` whom, at what `distance`,
  whether they were `attending`; for a rider, how much it `rider_likes` the
  dolphin, the `roll` (taken with it if the roll is under that), its
  `response` (taken with it, enjoyed it, unmoved, ignored it) and the
  seconds of interest added; for the player, the `interest_added`.
- `dolphin_try`: how a try at someone ended (`ended`: gave up on them,
  bored of them, lost them), how long it `lasted`, how long until it won
  their attention (`to_attention`), how many times it `won` it, the seconds
  of `attention`, its `acts` and how many were `enjoyed`.
- `mark`: an F2 mark (its number, where, facing), to find the moment by.

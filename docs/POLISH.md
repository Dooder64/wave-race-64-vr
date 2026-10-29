# Polish stage

Everything known that's wrong or unfinished in the VR mod, gathered on
2026-10-28, grouped so each round of work stays inside one part of the game.
Rounds are in the suggested order; tick items off as they're fixed and tested
in the headset.

Status: **open** = known, not started · **check** = may already be fixed,
needs a headset look · **fixed** = done and confirmed by the user.

## Round 1 — quick checks (one headset session, no code first)

Things reported earlier that later fixes may have cured. Look at each, then
mark it fixed or move it into a round below.

| # | Item | Where | Status |
|---|------|-------|--------|
| 1.1 | Cyan strip along the bottom of each eye — not noticeable in play; struck by the user 2026-10-29 | races | won't fix |
| 1.2 | Black square covering the Time Trials menu | Time Trials menus | fixed |
| 1.3 | Initials entry: the letters to pick are 3D objects in the scene (fine); the box showing the entered initials ("_ _ _", filling left to right) floats with a SOLID CYAN background, in both eyes. Seen on the initials screen after a Hard Championship game over at Port Blue, NOT after a Port Blue time trial — so probably a game state other than 0x64 that the backdrop fix (rect after 0x0106F8A0, full_grid_screen) doesn't cover. Built (unverified — not reachable by script): any non-race screen that calls the backdrop list then the full grid (0x010082F0) is now learnt as a full-grid screen, its backdrop dropped from the next frame; the log names it ("state 0x.. draws over the full grid of waves"). User confirmed 2026-10-29 | Championship initials entry | fixed |
| 1.4 | Thin white line along the top edge — the mirror window's 8-pixel frame caught by OBS, not the game; OBS now crops it | recordings only | fixed |
| 1.5 | Text lower-resolution than the rest with HD textures on | menus | fixed |
| 1.6 | Sky flickers — the recording shows single fully black frames (~10 in 20 min, menus and riding): RT64 sometimes draws nothing for a headset frame and an empty frame was submitted. Now the last images are shown again instead; user saw no flicker 2026-10-29. (A milder kind, one frame of brighter washed-out sky at 1:56.7 in the recording, wasn't noticed in play — reopen if seen) | all courses | fixed |
| 1.7 | Missing buoys: often the last one or two of a lap; the arrow floats with no buoy under it, and a red ball sits at the finish line (Sunny Beach, recording ~1:54). Possibly in the original recomp too (no upstream issue). Findings so far: the game's buoy list (0x801AEE20) has the buoys AND other course markers; its draw loop skips any entry whose +0xC8 flag is 0. In a scripted Hard run the arrow's target (checkpoint 7, Sunset Bay) was such an entry, logged "NOT DRAWN" — but that rider had gone off course, so unconfirmed. Logger: WR64_BUOY_LOG=1 (temporary, src/haptics.cpp); test aid WR64_TEST_DIFFICULTY=n. User's played run (2026-10-28): the game marks Sunset Bay's last three buoys (35, 37, 38) and Sunny Beach's last (57) as DRAWN throughout — so they are lost after the game (renderer or rewriter), not culled by the game. The "not drawn" entries are invisible turn markers (kind 4, +0xC8 = 0), never meant to show. Buoy model = lists 0x0102C828 / 0x0102BFD8 under a matrix at segment 5 + 0x4140 + 64n, y = 0. CAUSE FOUND (frame trace via WR64_3D_TRACE_BUOY): the game keeps matrices for only 12 race buoys a player (and 32 camera-facing boundary buoys), handed out in buoy order; drawing all round the camera puts more in view, so the last buoys of the lap get no matrix (missing, or drawn with a stale one, e.g. the ball at the finish). FIX (built, flat-verified 15/15 buoys drawn): recomp/wr64.toml hooks in func_8006E674 move both sets into areas of their own with one player — race buoys 64 at 0x7D1000, facing 256 at 0x7D8000 (src/vr.cpp). User confirmed in the headset 2026-10-29 | various | fixed |
| 1.8 | (merged into 1.3) | | — |
| 1.9 | Just before that initials screen, the GAME OVER screen itself: a solid grey-green box behind "Points needed to advance / Your points" hides the water (recording 12:31–12:37, left eye). Game state 0x5A, drawn like the results screen but missing from full_grid_screen; added, and the log confirms its backdrop is now left out (scripted game over). User confirmed 2026-10-29 | Championship game over | fixed |

## Round 2 — screen transitions

Same machinery (frames with no 3D scene, fades, the flat panel), so fix them
together.

| # | Item | Where | Status |
|---|------|-------|--------|
| 2.1 | Black box flashes on screen as the course-intro card fades in (seen at every course change in the 2026-10-28 recording) | course intro / loading | open |
| 2.2 | Brief flat-screen flashes at transitions | menus ↔ races | open |
| 2.3 | Course card's box flashes in during the fade | course intro | open |
| 2.4 | Left eye flickers white on the initials-entry screen (Sunset Bay TT; not on Port Blue) | TT initials, state 0x64 | open |
| 2.5 | Remove the black screen-wipe transitions in VR — they fill the screen in flat but look odd on the floating panel (low priority) | menus, before/after races | open |

## Round 3 — falls and swimming

| # | Item | Where | Status |
|---|------|-------|--------|
| 3.1 | Swimmer passes into the beach and swims through the sand as if it were water (dive near the piers, onto the beach) | Dolphin Park, pier/beach | open |
| 3.2 | Companion rider's bail and spray never actually seen on screen — confirm they look right | Dolphin Park | check |

## Round 4 — results and HUD oddities

| # | Item | Where | Status |
|---|------|-------|--------|
| 4.1 | Faint seam lines where the results-screen water meets the flat outer ring | results screens | open |
| 4.2 | One frame shows the flat elements huge on a white background in the eye — seen in a race, and twice on the Sunny Beach results in the recording (2:09.4, 2:12.8): likely the HUD's over-white pass shown as the eye picture | races, results | open |

## Round 5 — Twilight City

A course of its own; needs the user's Hard run first.

| # | Item | Status |
|---|------|--------|
| 5.1 | Time Trials only offers Normal for Twilight City (Reverse greyed, Hard/Expert won't select) | open |
| 5.2 | Gaps in the walls, missing collision | open |
| 5.3 | Extra ramp near the shortcut; beach ramp and spikes missing (probably the Normal vs Hard layout — confirm on Hard) | open |
| 5.4 | Start line reported out of bounds; 3 laps driven, 2 counted | open |
| 5.5 | Seafloor: give the existing floor collision rather than building a new one | open |

## Round 6 — bigger polish

| # | Item | Status |
|---|------|--------|
| 6.1 | Course-intro card: freeze the track model at a full-course angle (the game orbits its camera, which looked bad in VR so it's hidden) | open |
| 6.2 | Smooth menus: the game redraws menus at 20 fps while the headset runs at 72; needs 2D interpolation in RT64 | open |
| 6.3 | Free roam far past the buoys: water physics get odd a long way out | open |
| 6.4 | Free roam (no COURSE OUT countdown) on courses other than Dolphin Park | open |

## Your additions

_Add anything else here — the user's own bug list goes in the right round._

## Not polish — later features

Kept here so they don't get lost; see the roadmap for order.

- Several Dolphin Park riders with their own personalities and smarter decisions.
- Target mode: bail on purpose so the empty jet ski launches at targets.
- Rare glimpse of a huge, harmless sea creature in deep water after a throw.
- Bigger ocean with rougher waves away from the course; more underwater detail.
- The rider's own arms and hands in first person.
- Handlebar motion controls (declined for now).
- Course builder → guest courses from the user's own ROMs → open world.

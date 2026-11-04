# Wave Race 64: Recompiled

[![Watch the Wave Race 64 showcase](https://i.ytimg.com/vi/3uaqfG4Wzcg/hqdefault.jpg)](https://youtu.be/3uaqfG4Wzcg)

[Watch the showcase on YouTube](https://youtu.be/3uaqfG4Wzcg).

> ## You need: Wave Race 64 (USA) (Rev A)
>
> Also called **v1.1**. Cartridge ID `WR`, region `E`, **revision `1`**,
> 8 MiB `.z64`, header CRC `0x492F4B61 0x04E5146A`,
> sha1 `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`.
>
> The original US release (v1.0, revision 0), the Japanese and European
> versions and the Shindou edition **will not work**: their code is laid out
> differently, and every address in this project is tied to Rev A. The
> launcher checks the file you pick; from a shell,
> `WaveRace64Recomp.exe --identify your.z64` prints what it is.
>
> Supply your own legally obtained dump. This project does not include one,
> and never will.

A native PC port of Wave Race 64, made by statically recompiling the game's
MIPS code to C with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and
running it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
with [RT64](https://github.com/rt64/rt64) as the renderer and
[RecompFrontend](https://github.com/N64Recomp/RecompFrontend) for the menus
and controller support. It is unofficial and not affiliated with Nintendo.

## PC VR edition

This fork adds **seated PC VR** to the port, plus a set of ways to ride the
game's courses that it never offered. VR is optional: the same download plays
on a monitor as before, with every new option available there too.

**What you need:** Windows 10 or 11 (x64), a Direct3D 12 graphics card, your
own USA Rev A dump (above), and a PC VR headset whose OpenXR runtime is running
before the game starts. It was built and played on a Meta Quest 3 through the
Meta Quest Link app; other OpenXR runtimes (SteamVR, Virtual Desktop) should
work but haven't been tried.

**Playing:** download the zip from
[this fork's releases](https://github.com/Dooder64/wave-race-64-vr/releases),
extract it, connect the headset, and run **`Play in VR.bat`**. Running
`WaveRace64Recomp.exe` directly starts on the monitor. Once your dump is set
up, the game starts straight away (hold Shift while starting to see the
launcher again). It checks GitHub once at start for a newer release and says
so on the title screen; both can be turned off in Settings → General.

### In the headset

- **Seated stereo VR**, with the head tracked in all six directions around
  the game's own chase camera. Menus, the HUD and the screens between races
  sit on a floating panel (its size is in Settings → VR).
- **First person**, riding from the rider's eyes, leaning and rolling with
  the body (how much: OPTIONS → EXTRAS → TILT).
- **Thrown off**, the view goes with you: tumbling, sinking under the water
  with bubbles and muffled, echoing sound, then swimming back to the jet ski
  or walking up the beach, and climbing back on.
- **Seafloors** under every course, so there is something below you: sand,
  rock, canals and shallows, no deeper than about 15 m.
- **People-watching in Free Roam:** eight riders and the dolphin on every
  course, each with a mind of its own: laps and ramp jumps, sightseeing,
  resting, riding along with you, watching your big air, or stopping to
  watch the dolphin perform. Port Blue's riders follow the helicopter.
- **The whole scene drawn all round you**, rather than only what the
  monitor's camera would show.

### In the game's own menus (VR and monitor)

Everything you'd want to change is set on game-style screens, no settings
menu needed:

- **FREE ROAM** on the main menu: a ride on any of the nine courses with no
  clock, no laps, no buoys or rings, no COURSE OUT, and the land free to
  explore; far from land a gentle current eases you back. After the course,
  its own page: **WAVES** (still water up to far past the game's roughest),
  **SKY** and water from any course, **DAY AND NIGHT** (a 24-minute day
  fading between the courses' own looks), **MUSIC** (each track plays as you
  scroll past it), **FOG**, the **RIDERS** (eight on every course, each
  with a personality of its own: laps, ramp jumps, sightseeing, following
  you, resting or watching the dolphin, as it feels like), and on Dolphin
  Park its **LAYOUT**: Stunt Mode's, or the championship's Warm Up with the
  dolphin.
- **WAVES** before every race: after the difficulty in Championship (once for
  the whole championship), Time Trials and 2P VS, after the course in Stunt
  Mode.
- **MISSES and LAPS** in Championship, next to WAVES: up to 10 of each, for
  Reverse too.
- **OPTIONS → EXTRAS**: speed in KPH or MPH, chase camera or first person,
  first-person tilt, controller feedback, textures, water quality and style,
  ripples, spray, and unlock all.

**First person on a monitor:** click either stick (or F5) to switch views;
the right stick looks around, up to 80 degrees either side and 30 up or down,
and the view returns when you let go.

Window, graphics, controls and volume are still under Settings (Esc).

### Touch controllers

| Control | Does |
|---|---|
| Left stick | steer (the N64 stick) |
| Right trigger or A | throttle (A) |
| B | B |
| Left trigger | Z |
| Left / right grip | L / R |
| Right stick | C buttons (camera) |
| Menu button | Start |
| Left stick click (F5) | first person on/off |
| Right stick click (F6) | recentre the view |
| Y (F4) | after a fall, straight back onto the jet ski |
| X (F3) | throw yourself off |

A gamepad works too, as in the flat game.

### Known issues

- Free Roam's riders sometimes clip through walls and ice, most of all on
  Glacier Coast and the walled courses.
- Free Roam's riders are off on Twilight City for now: in its narrow canals
  they kept riding into the walls. They'll be back once they can find their
  way round it.
- In the attract scene the title's logo sometimes shows over the sky for a
  few frames.
- Brief flashes at some transitions between menus and races, and the
  black screen-wipes look odd on the floating panel.
- Menus animate at the game's 20 frames a second on the panel.
- Climbing back on after a fall, the view briefly looks into the jet ski.
- At Dolphin Park, a swimmer diving by the piers can swim through the beach.
- In Free Roam, passing where a hidden buoy is can still give POWER UP or
  POWER DOWN; very far out, the water's physics get odd.
- On Glacier Coast, the game's own collision reaches into open water in two
  places Free Roam's lap passes: beside the ice ramp before the final turn,
  and by the arrow sign at the turn to the finish. It's the same in the
  original game, where the buoys keep you away from both.

The full list is in [docs/POLISH.md](docs/POLISH.md).

## Getting it

**Windows (x64).** Download `WaveRace64Recomp-0.4.0-windows-x64.zip` from
[the Windows and Mac release](https://github.com/elliotttate/wave-race-64-recomp/releases/tag/v0.4.0-macos.3),
extract the entire ZIP, and run `WaveRace64Recomp.exe`. Pick your USA Rev A dump
in the launcher. HD textures, the replacement soundtrack, adjustable High/Aqua
water, event haptics, and the required DLLs are bundled.
See [installation and controls](docs/WINDOWS_RELEASE.md).

**macOS (Apple Silicon).** Download the ZIP from
[this fork's latest release](https://github.com/elliotttate/wave-race-64-recomp/releases/latest),
extract it, and drag `WaveRace64Recomp.app` to Applications. Requires macOS 15
or later and your USA Rev A dump. Libraries are bundled; no build tools are
needed. See [installation and first-launch instructions](docs/MACOS_RELEASE.md).

The [original project's releases](https://github.com/danielgomesvieira2000/wave-race-64-recomp/releases)
are also available. This fork's packages include replacement artwork and music;
original cartridge data is loaded from your dump each time the game runs.
See *Licensing* below before redistributing it.

**Build it yourself.** [docs/BUILDING.md](docs/BUILDING.md) takes you from
installing the toolchain on Windows to the first race: clone with submodules,
check your dump, disassemble and recompile the game from it, build.
For a native Apple Silicon app using Metal, see [docs/MACOS.md](docs/MACOS.md).

Settings, controller profiles and saves live in
`%LOCALAPPDATA%\WaveRace64Recomp`. Started by double-click, the program writes
its log to `wr64.log` in that folder; attach that file to a bug report.

## What 0.3 is

The game boots, its menus work, and championship, time trial and two-player
races run with audio, at speed, with records saved to the emulated EEPROM.
Not every course has been played in both directions yet, and stunt mode and
the championship ceremony have had less testing than the rest; reports of
anything wrong there are welcome.

New in 0.3, all of it widescreen work on the 2D layer: the race HUD is laid
out across the frame rather than kept at 4:3 in the middle of it -- time
top-left, rank and lap centre, speed and the leaderboard right, MISS and power
along the bottom -- while menus keep the 4:3 layouts their frames are drawn
for. The game's full-screen overlays, the tint over a race and the pause
screen's dim, now reach the edges instead of leaving a brighter band down each
side. And the warm-up round is widescreen like every other race, truly so
rather than a stretched 4:3 picture.

From 0.2: the 3D models on the watercraft and rider select screens sit in
their frames in widescreen, and two-player split screen is presented in full.

All of it comes from a display-list rewriter in the port, which inserts RT64's
extended commands into the game's lists before RT64 sees them without touching
the game's code, and from four scripted patches to RT64 (see `docs/PLAN.md`,
phase 07, and `tools/patch_rt64.py`).

Beyond running natively, the port adds:

- **Widescreen** at the display's resolution and aspect ratio, with the game's
  own black overscan borders removed. **HUD Placement** in the Graphics tab
  chooses whether the race HUD is laid out across the frame or kept at 4:3 in
  the middle of it.
- **High frame rate.** The game keeps its own update rate (30 Hz in a race, 20
  in the menus and Time Trial, by its own choice) and RT64 interpolates each
  object's movement between game frames, so it presents at your display's
  refresh rate. Physics, camera and timers are untouched.
- A launcher with a ROM picker, a settings menu, and controller remapping with
  per-device profiles, from RecompFrontend.
- Audio through the recompiled RSP microcode.

## Controller haptics

**Settings → Haptics** adds distinct feedback for wave landings, collisions,
buoys, power gains, and race milestones, with a subtle engine and water feel.
Events only feedback is enabled by default. Adjust its strength, select Full,
or turn it off. Supported controllers can also use trigger vibration. Feedback
stops during pause/settings and when the game loses focus. See the
[haptics guide](docs/HAPTICS.md) for controls and implementation details.

## Replacement soundtrack

Source builds of this fork include nine replacement recordings: Bryan EL's
Main Theme and Dolphin Park, plus seven tracks from Retro Game Remix's
*Dolphin Park* album. The build bundles the recordings, loop settings, and
[artist credits](assets/music/CREDITS.md). They are available under
**Settings → Sound → Music → Custom**, with a separate custom-music volume
control. **Custom is enabled by default.** **Original** restores the cartridge
soundtrack.

All courses have replacements; Options and first-place results also have
assigned recordings. Effects and announcer commands keep using the original
audio. Local recordings can override individual bundled tracks. See
[the track mapping and setup guide](docs/MUSIC.md). These additions follow the
`v0.4.0-macos.1` binary release. Download `v0.4.0-macos.2` or newer for the
bundled soundtrack and HD textures with modern water enabled by default.

## Bundled HD textures

This fork includes the reviewed [HD texture pack](assets/textures/nano-banana-2),
with **1,828 replacement mappings**. **HD is enabled by default** under
**Settings → Graphics → Textures**; choose Original to use the cartridge artwork.
The build bundles the textures alongside the music, so no separate pack install
is needed.

The pack includes refined HUD fonts and signs, both Wave Race logos, eight
helmet portraits with dark visors, sharper rank and buoy icons, ramp wood,
island materials, and improved jet-ski side panels for the primary and opponent
models. Most textures are 4×; the focused portraits, icons, terrain, wood and
secondary craft panels use reviewed 8× replacements. World materials have
complete mip chains. Rejected generated candidates retain source-preserving
replacements, so coverage does not imply that every material gained new detail.
See [credits](assets/textures/nano-banana-2/CREDITS.md) and
[coverage and reconstruction details](docs/HD_TEXTURES.md).

## Modern water preview

[![Watch the modern water showcase on YouTube](https://i.ytimg.com/vi/ikUGbLmPbvA/hqdefault.jpg)](https://www.youtube.com/watch?v=ikUGbLmPbvA)

[Watch the water shader in motion on YouTube](https://www.youtube.com/watch?v=ikUGbLmPbvA).

The source build enables **High water with Aqua appearance by default**.
In **Graphics → Water**,
choose **Modern** for detailed ripples, sun/sky lighting, depth color,
refraction, shoreline wash and persistent wakes, or **High** to add screen-space reflections
and fine spray. Choose **Original** to restore original rendering. Press **F9** for a comparison
from the current camera; **F10** cycles the rendering diagnostics.

The **Water** tab adds **Modern / Classic / Aqua** appearance and **Soft / Normal /
Strong** surface ripples. Aqua offers lighter teal water and clearer shallows
using the full Modern renderer, including its lighting, refraction, reflections,
foam and wakes. Classic preserves the original course colors,
transparency and broad wave highlights while adding reflections, wakes and
spray. Modern with Normal ripples keeps the earlier appearance. All three styles
use irregular foam breakup to avoid a square pattern behind the craft. Wave
heights and reflection highlights interpolate at the selected display rate,
including when the original water grid recenters around the camera. **Spray
particles → Off** removes the added airborne spray while retaining surface foam
and wakes. Spray close to the camera also fades out before it can fill the screen. Fine
ripples use irregular wind-stretched patterns to avoid repeating crossed bands.

Selecting **Aqua** reveals **Water brightness**, **Aqua tint**, and **Water
clarity** sliders. Each defaults to **50%**, preserving the reviewed Aqua look.
Adjust them and press **Apply**; they are saved independently of the other water
styles. Existing explicit Modern or Classic preferences are preserved.

The original wave mesh and game physics remain authoritative. Water assets
are procedural and course settings are in `assets/water/profiles.json`.
Metal and Windows Direct3D 12 have been runtime tested; SPIR-V and DXIL shader
compilation also pass. See [Windows validation](docs/WINDOWS_RELEASE.md) and
[the implementation and evidence](docs/MODERN_WATER_PROGRESS.md)
for the acceptance matrix, scope and reproduction commands.

Known issues:

- In a 4:3 window the picture is letterboxed. The game draws a 303x199 region
  of its 320x240 framebuffer, which does not fit a 4:3 window without bars.
  Fullscreen on a widescreen display, the default, has no bars.
- The **results screen**'s layout is wrong when HUD Placement is set larger
  than Original.
- The **MAX POWER** banner loses its last letter while the HUD layout is on.
- The macOS download supports Apple Silicon; Intel Macs have not been validated.

## Layout

```
src/        the port: platform layer, renderer binding, input, audio, launcher wiring
include/    its headers
patches/    replacements and wrappers for individual game functions
recomp/     N64Recomp configuration
tools/      the build pipeline's scripts, and diagnostics (see tools/README.md)
assets/     launcher assets, replacement music, HD textures and water profiles
docs/       the build guide, the phase plan, and what each phase found
lib/        upstream submodules
```

ROM dumps, disassembly, `RecompiledFuncs/`, and the ELF are generated locally and
refused by `.gitignore`. So is the splat config derived from the reference
decompilation. The reviewed replacement artwork is included separately under
`assets/textures`; raw texture captures and generation work files remain local.

## Licensing

The project's own code and artwork are under the MIT License (`LICENSE`).
The separately credited recordings in `assets/music` and replacement artwork
in `assets/textures` are excluded from that license; the underlying recordings,
compositions and game artwork remain the work of their respective owners. See
[music credits](assets/music/CREDITS.md) and
[texture credits](assets/textures/nano-banana-2/CREDITS.md).

A built executable is another matter. It links N64ModernRuntime statically,
and N64ModernRuntime is **GPL-3.0**, so the executable as a whole is a
GPL-3.0 combined work: anyone who distributes one must provide its complete
source under the GPL's terms, and this repository at the tagged commit,
with the submodules it pins, is that source. RecompFrontend, which supplies
the menus, is GPL-3.0 too, and VR mode's OpenXR loader is Apache-2.0 or MIT.
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list.

## How this was made

The original upstream project was written by Claude, Anthropic's AI model, working in
[Claude Code](https://claude.com/claude-code) under the direction of Daniel
Gomes Vieira, who set the goals, made the decisions, played the builds and
reported what he saw. The launcher artwork and the executable's icon were
generated with Claude as well. The work was done in phases (see
[docs/PLAN.md](docs/PLAN.md)), and each phase's findings are written up under
`docs/`, in the same way: by Claude, as the work was done.

This fork adds native macOS support, modern water, replacement music and HD textures,
developed with Codex under Brian Tate's direction and validated through native
playtesting and deterministic replays.

The PC VR edition -- seated stereo VR through OpenXR, first person with the
swim back after a fall, seafloors, Dolphin Park's riders, Free Roam and its
course options -- was written by Claude in Claude Code under the direction of
Dooder, who set the goals, made the decisions and play-tested every build in
the headset and on the monitor.

## Credits

The recompilation toolchain and runtime are by Mr-Wiseguy and the N64Recomp
contributors; RT64 is by Darío and the RT64 contributors. The function names
this project leans on come from the Wave Race 64 decompilation by LLONSIT and
contributors. Earlier recompilation attempts by WACOMalt and chronic8000 showed
what to expect.

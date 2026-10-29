# A standalone Quest 3 build — feasibility and plan

Written 2026-10-29. The question: how faithfully could the VR mod run on a
Quest 3 by itself, as an APK, with no PC?

## Short answer

Feasible, and less of a leap than it first looks. The game logic and all of
our gameplay additions would be identical. The renderer has already been
shown working on the Quest's family of GPU by other projects. The real
unknowns are the Quest's own graphics driver and whether stereo at 72 Hz fits
in its power. Expect the look to sit between the original N64 and our PC
version: lower sharpness per eye and simpler water, the same 3D, first person,
seabeds, fish, HUD and companion.

## What the research found

- **Recompiled N64 games already run on Android with RT64 over Vulkan.**
  Community ports of *Goemon 64: Recompiled* and *Harvest Moon 64:
  Recompiled* ship as arm64 APKs: a Gradle module driving the native CMake
  build (NDK 27, CMake 3.22), arm64-v8a, Android 9+, Vulkan. Tested mainly on
  Snapdragon/Adreno handhelds — the AYN Thor uses the same Adreno 740 GPU
  family as the Quest 3's XR2 Gen 2.
  [Goemon64Recomp-Android](https://github.com/ogdanimal/Goemon64Recomp-Android),
  [HarvestMoon64Recomp](https://github.com/igawa6/HarvestMoon64Recomp).
- **Known driver trouble, with workarounds.** That port reports a Qualcomm
  Vulkan driver bug that closed the game seconds after starting (worked round
  by turning off framebuffer effects, or loading another driver), and a Mali
  fallback for dual-source blending. The Quest can't load custom drivers, so
  its stock driver has to work, or RT64 has to avoid what trips it.
- **No Quest build of any recompiled game was found.** We'd be first, as far
  as a search shows.

## What our project already has going for it

- **It is already cross-platform.** It builds for macOS (Metal) as well as
  Windows, and CMake has a Linux branch. The recompiled game is plain C.
- **RT64 can already use Vulkan on the PC** (`graphicsAPI` in rt64.json), so
  the renderer half can be proven on the PC first.
- **The modern water's shaders already compile to SPIR-V** (Vulkan) as well as
  DXIL; Metal is the one tested at run time.
- **Most of the VR renderer work is API-neutral.** The stereo renders, head
  pose, HUD layer passes and their combine go through RT64's own graphics
  layer (plume), which has a Vulkan backend.

## What is tied to Windows / Direct3D 12

Small and well contained:

- `src/vr_openxr.cpp` (about two dozen references): the OpenXR session's
  D3D12 binding, swapchain image types, graphics requirements. Needs a Vulkan
  path (`XR_KHR_vulkan_enable2`), which OpenXR supports on both PC and Quest.
- `tools/patch_rt64_vr.py` (about a dozen): headset images wrapped as
  `D3D12Texture` in RT64's present queue. Needs the Vulkan equivalent
  (plume's Vulkan texture wrapping an external `VkImage`).
- `src/renderer.cpp`, `src/main.cpp`, `src/crash_handler.cpp`, a few lines
  elsewhere: Windows window, crash and path handling, which Android replaces
  anyway.

## What a Quest build adds that the PC doesn't need

- **Android packaging:** a Gradle wrapper and manifest as the Goemon port has,
  plus the Quest's OpenXR loader and app lifecycle (pause/resume when the
  headset sleeps or the Meta menu opens).
- **Starting in VR:** a Quest app has no desktop window. The launcher and
  settings (today a desktop window) must show in the headset, on the flat
  quad layer we already use for menus.
- **The ROM:** the player still supplies their own; picked from the headset's
  storage (the Harvest Moon fork has an Android file picker to borrow from).
- **Performance work** (below).

## Performance — the real question

On the PC each frame draws the whole scene twice, once per eye, at about
1824×1968 each, 72 times a second, with High water. The Quest's GPU is several
times weaker than an RTX 5070. The levers, roughly in order of payoff:

1. **Per-eye resolution:** render well below the panel's native size (RT64's
   internal resolution) and let the compositor scale it up.
2. **Fixed foveated rendering** (Meta's `XR_FB_foveation`): full detail in the
   middle of each eye, less towards the edges, where the lens blurs anyway.
3. **Water quality:** original or Modern, not High.
4. **Our extras' cost:** fish schools, the all-round draw distance, the
   seafloor's draw radius — each can be scaled down on the Quest.
5. **Later, if needed:** drawing both eyes more cheaply (the second eye
   reusing work from the first) — a bigger RT64 change.

72 Hz at reduced quality looks plausible. 90 Hz is unlikely.

## Plan, smallest risk first

Each step answers a question before the next one spends effort.

1. **Vulkan VR on the PC.** Switch RT64 to Vulkan and give the OpenXR code a
   Vulkan path; play over Link exactly as today. Proves all the VR code works
   on Vulkan, with the PC's debugging tools, before touching Android. Useful
   even if the Quest build stalls (a second backend for PC players).
   *Size: medium.*
2. **Flat test APK on the Quest.** Build the game, not VR, for Android,
   modelled on the Goemon port, and run it as a 2D panel app on the Quest.
   Answers: does RT64 run on the Quest's stock driver, and how fast is one
   view? *Size: medium. This is the go/no-go point.*
3. **VR on the Quest.** The Android OpenXR loader and lifecycle, the Vulkan
   swapchain from step 1, Touch controllers (already mapped), the launcher on
   the menu layer, ROM picking. *Size: large.*
4. **Make it fast enough.** Resolution, foveation, water and extras presets
   until 72 Hz holds on every course, measured on the busiest scenes
   (Dolphin Park's fish, Twilight City's canals). *Size: medium, open-ended.*
5. **Release.** Sideloaded APK (SideQuest); the player supplies the ROM.
   Distribution follows whatever the upstream recompilation does for its own
   releases.

## Open questions

- Does the Quest's stock Adreno driver run RT64 without the crash other
  Adreno devices showed? (Step 2 answers it.)
- How much stereo can it afford at 72 Hz? (Steps 2 and 4.)
- Is the Meta PC runtime's Vulkan OpenXR path solid enough for step 1? It is
  supported; not yet tried here.

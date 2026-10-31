# Third-party notices

This project's own code (`src/`, `include/`, `patches/`, `tools/`, `recomp/`,
`assets/icons/`, `assets/recomp.rcss`) is under the MIT License in `LICENSE`.
A built executable also contains the following, under their own terms. License
texts are in the named files inside the submodules under `lib/`, and a
release's `licenses` folder carries a copy of each.

| Component | Role | License | Text |
|---|---|---|---|
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) (librecomp, ultramodern) | the runtime the recompiled game runs on | **GPL-3.0** | `lib/N64ModernRuntime/COPYING` |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp), RSPRecomp | the recompiler (build-time; its headers are compiled in) | MIT | `lib/N64ModernRuntime/N64Recomp/LICENSE` |
| [RT64](https://github.com/rt64/rt64) | the renderer | MIT | `lib/RT64/LICENSE` |
| [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) (recompui, recompinput) | menus, controller mapping | **GPL-3.0** (published 2026-10-16, commit 2ddbfc8, which this project pins) | `lib/RecompFrontend/COPYING` |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) loader (release-1.1.63) | VR mode: the link to the headset's OpenXR runtime, linked statically | Apache-2.0 OR MIT (the loader and headers); jsoncpp inside it, MIT | `lib/OpenXR-SDK/LICENSES/`, `lib/OpenXR-SDK/COPYING.adoc` |
| [RmlUi](https://github.com/mikke89/RmlUi) | the UI toolkit recompui is built on | MIT | `lib/RecompFrontend/recompui/lib/RmlUi/LICENSE.txt` |
| [lunasvg](https://github.com/sammycage/lunasvg) | SVG icons in the menus | MIT | `lib/RecompFrontend/recompui/lib/lunasvg/LICENSE` |
| GamepadMotionHelpers | gyro handling in recompinput | MIT | `lib/RecompFrontend/lib/GamepadMotionHelpers/LICENSE` |
| SDL2 | window, input, audio | zlib | `lib/RT64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3/` |
| DirectX Shader Compiler (dxcompiler.dll, dxil.dll) | shader compilation | University of Illinois/NCSA (LLVM) / Microsoft | not in RT64's copy; see [DirectXShaderCompiler's LICENSE.TXT](https://github.com/microsoft/DirectXShaderCompiler/blob/main/LICENSE.TXT) |
| plume, hlsl++, Dear ImGui, nativefiledialog-extended, xxHash, zstd, miniz, o1heap | RT64 and runtime dependencies | MIT / BSD / zlib | under `lib/RT64/src/contrib/` and `lib/N64ModernRuntime/thirdparty/` |
| Lato, Noto Emoji | menu fonts, copied from RmlUi's samples at build time | SIL Open Font License 1.1 | `lib/RecompFrontend/recompui/lib/RmlUi/Samples/assets/LICENSE.txt` |
| PromptFont | controller and keyboard glyphs in the menus | see `assets/promptfont/LICENSE.txt` | `assets/promptfont/LICENSE.txt` |

## What this means for a built executable

The runtime and the menus (RecompFrontend) are GPL-3.0 and are linked statically, so **any executable built from
this project is a combined work distributed under the GNU General Public
License, version 3**, whatever the license of the project's own files. Anyone
who distributes such a binary must make its complete corresponding source
available under the GPL's terms; this repository, at the commit the binary was
built from, together with the submodules it pins, is that source.

RecompFrontend published no license until 2026-10-16, when its authors added
the GPL-3.0 (commit 2ddbfc8, "Added GPL3 license"). This project pins that
commit, so the menus are distributed under the same GPL-3.0 as the runtime.

## The reference decompilation

The build pipeline uses the [Wave Race 64 decompilation](https://github.com/LLONSIT/Wave-Race-64)
by LLONSIT and contributors as its source of function names, segment layout
and splat configuration. That repository publishes no license. Nothing copied
from it is committed here: the builder clones it into `reference/`, which is
ignored, and the splat config this project derives from its config
(`recomp/wr64.us.rev1.asm.yaml`) is generated on the builder's machine and
ignored too. The function names that appear in this project's own sources and
documents are used as identifiers for the routines they name.

## Artwork

The launcher background (`assets/icons/Logo.svg`), the executable's icon
generated from it, and the menu icons in `assets/icons/` were created for this
project with Claude, Anthropic's AI model, at the direction of the project's
author, and are released under the project's MIT License to the extent that
rights in them exist. Whether purely AI-generated images attract copyright at
all is unsettled and varies by jurisdiction; no claim is made beyond what the
law allows, and nothing in them is taken from the game.

## Replacement recordings

`assets/music` contains two recordings supplied as Bryan EL remakes and seven
tracks from Retro Game Remix's *Dolphin Park* album. They are separate from the
project's MIT-licensed code and artwork. The recordings and compositions remain
the work of their respective owners; the project's license does not grant
rights to them. [CREDITS.md](assets/music/CREDITS.md) preserves attribution and
album credits, and [manifest.json](assets/music/manifest.json) records the
supplied filenames, conversion gains, checksums, and loop settings.

## What is not here

No Nintendo code or data. The recompiled game code in `RecompiledFuncs/` is
generated on the builder's machine from the builder's own dump and is never
committed; the built executable contains that generated code, and loads the
game's assets from the player's dump at run time.

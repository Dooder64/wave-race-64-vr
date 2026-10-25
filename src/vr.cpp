// VR, phase 2: prove the camera can be moved without the game noticing.
//
// The eyes of a stereo pair are the game's camera moved a little to each
// side, and head tracking is that camera moved and turned by the headset.
// Both go through the same hook: RT64 interpolates the camera between game
// frames and then, before applying the lens, multiplies on an offset the port
// sets (tools/patch_rt64_vr.py). The game's own camera, physics and timers
// never see it.
//
// F7 steps that offset through left, right and none, far enough to be obvious
// on a monitor. A working hook shows the 3D world sliding sideways while the
// HUD stays put and the race carries on as before.

#include "wr64/vr.h"

#include <cstdio>

extern "C" void RT64_SetViewOffset(float x, float y, float z);

namespace wr64::vr {

namespace {

// Thirty units is roughly a metre if the chase camera rides about two metres
// above the water, which is where the camera log puts it (55 units): plainly
// visible. A real eye separation would be one or two.
constexpr float kDebugOffset = 30.0f;

int g_debug_step = 0;  // 0 none, 1 left, 2 right

}  // namespace

void cycle_debug_offset() {
    g_debug_step = (g_debug_step + 1) % 3;
    const float x = g_debug_step == 1 ? -kDebugOffset : g_debug_step == 2 ? kDebugOffset : 0.0f;
    RT64_SetViewOffset(x, 0.0f, 0.0f);
    static const char* const kNames[] = { "none", "left", "right" };
    std::fprintf(stderr, "[wr64-vr] view offset: %s (%.0f units)\n", kNames[g_debug_step], x);
    std::fflush(stderr);
}

}  // namespace wr64::vr

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
//
// Phase 3 draws both eyes side by side on the monitor, the layout a headset
// uses: RT64 renders each displayed frame twice, the camera moved half the
// separation left for the left half and right for the right. F8 steps it
// through off, a human eye separation, and an exaggerated one.

#include "wr64/vr.h"

#include <cstdio>

extern "C" void RT64_SetViewOffset(float x, float y, float z);
extern "C" void RT64_SetStereo(int mode, float separation);

namespace wr64::vr {

namespace {

// Thirty units is roughly a metre if the chase camera rides about two metres
// above the water, which is where the camera log puts it (55 units): plainly
// visible. A real eye separation would be one or two.
constexpr float kDebugOffset = 30.0f;

int g_debug_step = 0;  // 0 none, 1 left, 2 right

// About a human eye separation, 6-7 cm, if a unit is the 3 cm or so the
// camera log suggests. On a monitor the two halves then differ only a
// little, so F8 also offers five times that, where the difference is plain.
constexpr float kStereoSeparation = 2.0f;
constexpr float kStereoExaggerated = 10.0f;

int g_stereo_step = 0;  // 0 off, 1 normal, 2 exaggerated

}  // namespace

void cycle_debug_offset() {
    g_debug_step = (g_debug_step + 1) % 3;
    const float x = g_debug_step == 1 ? -kDebugOffset : g_debug_step == 2 ? kDebugOffset : 0.0f;
    RT64_SetViewOffset(x, 0.0f, 0.0f);
    static const char* const kNames[] = { "none", "left", "right" };
    std::fprintf(stderr, "[wr64-vr] view offset: %s (%.0f units)\n", kNames[g_debug_step], x);
    std::fflush(stderr);
}

void cycle_stereo() {
    g_stereo_step = (g_stereo_step + 1) % 3;
    const float separation = g_stereo_step == 2 ? kStereoExaggerated : kStereoSeparation;
    RT64_SetStereo(g_stereo_step != 0 ? 1 : 0, separation);
    static const char* const kNames[] = { "off", "side by side", "side by side, exaggerated" };
    if (g_stereo_step == 0) {
        std::fprintf(stderr, "[wr64-vr] stereo: %s\n", kNames[g_stereo_step]);
    }
    else {
        std::fprintf(stderr, "[wr64-vr] stereo: %s (eyes %.0f units apart)\n", kNames[g_stereo_step], separation);
    }
    std::fflush(stderr);
}

#if !defined(_WIN32)
// OpenXR is wired up for Direct3D 12 only; elsewhere VR mode is never on.
bool enabled() { return false; }
void attach(void*, void*) {}
void shutdown() {}
#endif

}  // namespace wr64::vr

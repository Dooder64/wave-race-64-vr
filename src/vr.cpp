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
//
// A headset also looks where the flat game never needed to draw. The game
// leaves objects out that are off to the side of its camera or far away
// (func_8006E674; docs/VR-CULLING.md); recompiler hooks in recomp/wr64.toml
// ask this file whether to draw them all round, and how much farther.

#include "wr64/vr.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    // In VR mode the headset owns stereo: both eyes are always drawn, apart by
    // the headset's own eye positions.
    if (enabled()) {
        std::fprintf(stderr, "[wr64-vr] stereo follows the headset in VR mode; F8 does nothing\n");
        std::fflush(stderr);
        return;
    }
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
void recenter() {}
Controllers controllers() { return {}; }
void set_controller_rumble(float, float) {}
void set_panel_size(float) {}
#endif

bool all_round() {
    // VR mode, or WR64_CULL_ALL_ROUND=1 to see it without a headset; and in
    // first person on a monitor, whose view looks round with the right stick.
    static const bool on = [] {
        const char* value = std::getenv("WR64_CULL_ALL_ROUND");
        const bool forced = value != nullptr && std::strcmp(value, "1") == 0;
        const bool result = forced || enabled();
        if (result) {
            std::fprintf(stderr, "[wr64-vr] objects are drawn all round the camera\n");
            std::fflush(stderr);
        }
        return result;
    }();
    return on || first_person();
}

float draw_distance() {
    // How much farther than the course asks objects are drawn before they have
    // faded out; WR64_VR_DRAW_DISTANCE overrides it.
    static const float factor = [] {
        if (!all_round()) return 1.0f;
        const char* value = std::getenv("WR64_VR_DRAW_DISTANCE");
        const float parsed = value != nullptr ? float(std::atof(value)) : 0.0f;
        const float result = parsed > 0.0f ? parsed : 2.0f;
        std::fprintf(stderr, "[wr64-vr] objects are drawn %.1f times as far\n", result);
        std::fflush(stderr);
        return result;
    }();
    return factor;
}

}  // namespace wr64::vr

// Called from the recompiler hooks in recomp/wr64.toml, from generated C, so
// with C linkage and C types.
extern "C" int wr64_vr_all_round(void) {
    return wr64::vr::all_round() ? 1 : 0;
}

// The camera-facing matrices of the course's second object group (race and
// boundary buoys), moved out of the game's frame buffer, where one player has
// room for only 32, to an area of their own with room for 256, one per frame
// buffer: physical 0x7D8000 and 0x7DC000, above the first-person views and
// below the audio task's copy at 0x7E0000. Given the frame buffer's address
// (D_801AE948) and the number of players; 0 = leave the game's own.
namespace {
// Which of the game's two frame buffers this is (0 or 1), or -1.
int buffer_slot(uint32_t buffer) {
    static uint32_t buffers[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (buffers[i] == 0) buffers[i] = buffer;
        if (buffers[i] == buffer) return i;
    }
    return -1;
}
}  // namespace

extern "C" uint32_t wr64_vr_facing_area(uint32_t buffer, int players) {
    if (!wr64::vr::all_round() || players != 1) return 0;
    const int slot = buffer_slot(buffer);
    return slot < 0 ? 0 : 0x7D8000u + uint32_t(slot) * 0x4000u;
}

// The same for the race buoys (the first object group), whose matrices the
// game keeps at 0x95C0 (the buoy) and 0x9BC0 (its marker) of the frame's
// buffer, 12 a player: here 64 each, from physical 0x7D1000, 0x2000 a frame
// buffer, the buoys' first and their markers' 0x1000 on. `marker` picks.
extern "C" uint32_t wr64_vr_buoy_area(uint32_t buffer, int players, int marker) {
    if (!wr64::vr::all_round() || players != 1) return 0;
    const int slot = buffer_slot(buffer);
    return slot < 0 ? 0 : 0x7D1000u + uint32_t(slot) * 0x2000u + (marker ? 0x1000u : 0u);
}

extern "C" int wr64_vr_draw_distance(int value) {
    const float factor = wr64::vr::draw_distance();
    return factor == 1.0f ? value : int(float(value) * factor);
}

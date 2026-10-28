// Dolphin Park without its course-out.
//
// Each frame of a race, for each human player, the game asks func_80074264
// whether the jet ski at (x, z) -- f12, f14 -- is outside the course (on
// Dolphin Park, too far from its middle), and while it is, counts "COURSE
// OUT" down from 10 and then retires the player (the race overlay's code at
// 0x801F1E7C, measured from a memory diff of the countdown in the player's
// race record, +0x2E4 and +0x2E8). Nothing else calls it. On Dolphin Park
// (course 0) it is answered "inside" always, so the open water past the
// buoys can be roamed freely. Being stranded on land still counts as a
// course-out, as before.
//
// WR64_FREE_ROAM=0 leaves the game's course-out alone.

#include "recomp.h"

#include <cstdint>
#include <cstdlib>

extern "C" void func_80074264(uint8_t*, recomp_context*);

namespace wr64 {

// func_80074264: whether a point is outside the course.
void free_roam_course_out_hook(uint8_t* rdram, recomp_context* ctx) {
    static const bool off = [] {
        const char* value = std::getenv("WR64_FREE_ROAM");
        return value != nullptr && value[0] == '0';
    }();
    const int32_t course = *reinterpret_cast<const int32_t*>(rdram + 0x0D8170);
    if (!off && course == 0) {
        ctx->r2 = 0;
        return;
    }
    func_80074264(rdram, ctx);
}

}  // namespace wr64

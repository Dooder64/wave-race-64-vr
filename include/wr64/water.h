#pragma once
#include <atomic>
#include <cstdint>
#include "shared/rt64_water_params.h"

namespace wr64::water {
enum class Quality : uint32_t { Original, Modern, High, Ultra };
enum class Style : uint32_t { Modern, Classic, Aqua };
enum class RippleDetail : uint32_t { Soft, Normal, Strong };
// Publish on the original game's task submission thread; consume by list identity.
void publish_frame(const uint8_t *rdram, uint32_t display_list);
void reset_for_race();
void begin_frame(uint32_t display_list);
interop::WaterMaterial material(uint32_t view);
void toggle();
void cycle_debug();
Quality quality();
void set_quality(Quality value);
void set_style(Style value);
void set_aqua_brightness(float percent);
void set_aqua_tint(float percent);
void set_aqua_clarity(float percent);
void set_ripple_detail(RippleDetail value);
void set_spray_enabled(bool enabled);
// The highest standing surface of the course's collision at (x, z) no higher
// than `below`; false where there is none.
bool ground_height_at(float x, float z, float below, float* height);
// The thrown rider's body skidding on the water, for the spray: where it is,
// and whether it touches the water (wet 1) or is in the air (0). Set from the
// renderer's thread, read when the frame is published.
void set_body_spray(bool active, float x, float y, float z, float wet);
// The game states whose menus are drawn over a camera looking straight down
// on the "full grid" of waves: course select, the time trial and race
// results, the Championship's game over (0x5A), the initials entry for a new
// record (0x64), and the menu after a time trial. Each comes with its leaving
// state (the next odd number), which draws the same screen. Any other screen
// found drawing the same way -- the backdrop's list, then the full grid's --
// is learnt (learn_full_grid_screen, from the rewriter) and counts from then on.
inline std::atomic<bool> g_learnt_full_grid[128];
inline bool full_grid_screen(uint32_t state) {
    switch (state & ~1u) {
        case 0x14: case 0x32: case 0x34: case 0x50: case 0x5A: case 0x64: return true;
        default: return state < 256 && g_learnt_full_grid[(state & 0xFF) >> 1].load(std::memory_order_relaxed);
    }
}
// True the first time a state is learnt.
inline bool learn_full_grid_screen(uint32_t state) {
    return state < 256 && !full_grid_screen(state) &&
           !g_learnt_full_grid[state >> 1].exchange(true, std::memory_order_relaxed);
}
}

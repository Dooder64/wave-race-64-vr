#pragma once
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
// The game states whose menus are drawn over a camera looking straight down
// on the "full grid" of waves: course select, the time trial and race
// results, the initials entry for a new record (0x64), and the menu after a
// time trial. Each comes with its leaving state (the next odd number), which
// draws the same screen.
inline bool full_grid_screen(uint32_t state) {
    switch (state & ~1u) {
        case 0x14: case 0x32: case 0x34: case 0x50: case 0x64: return true;
        default: return false;
    }
}
}

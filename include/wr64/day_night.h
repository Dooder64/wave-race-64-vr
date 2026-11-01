#pragma once

#include <cstdint>

// Day and night in Free Roam (src/day_night.cpp, docs/DAY_NIGHT.md).
namespace wr64::day {

// Between which two courses' looks the hour falls, and how far from the
// first to the second (0-1). `from` == `to` while one look holds.
struct Blend {
    int from = 0, to = 0;
    float t = 0.0f;
};

// Whether the day turns in free roams (the Free Roam options; WR64_DAY=1 for
// scripted tests).
void set_enabled(bool on);
bool enabled();

// From patches/free_roam.cpp, once a frame: whether a free roam is being
// ridden (the clock runs only then, and not while paused), and its course.
// A new ride starts the clock at the course's own hour.
void frame(bool riding, bool paused, int course);
// Whether the clock is turning now, and the looks it falls between.
bool active();
Blend now();
// The hour, 0-24, for logs.
float hour();
// The look a free roam starting on `course` opens with.
int start_look(int course);

// From patches/free_roam.cpp, once a frame while active(): the sky's
// pictures, which the game loaded once for the look `opened`, blended between
// the hour's two looks in place. `lists`: each course's list of its sky's
// pictures (the table at 0x800DC0E4).
void sky_frame(uint8_t* rdram, const int32_t lists[9], int opened);

}  // namespace wr64::day

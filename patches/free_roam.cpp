// Free Roam: the courses without their race.
//
// Each frame of a race, for each human player, the game asks func_80074264
// whether the jet ski at (x, z) -- f12, f14 -- is outside the course, and
// while it is, counts "COURSE OUT" down from 10 and then retires the player
// (the race overlay's code at 0x801F1E7C, measured from a memory diff of the
// countdown in the player's race record, +0x2E4 and +0x2E8). Nothing else
// calls it. On Dolphin Park (course 0) it is always answered "inside", so the
// open water past the buoys can be roamed freely. Being stranded on land
// still counts as a course-out, as before.
//
// With the Free Roam switch on (settings, Free Roam tab), Time Trials and
// Stunt Mode on every course are answered "inside" too, and the race never
// ends: the laps it takes to finish (D_801CE728) are held out of reach, the
// buoys missed are held at none, and Stunt Mode's clock is held.
//
// Its waves can be chosen: the wave level (D_800D49B0) the game takes from
// each course (D_800EABEC: Drake Lake 10, the calmest, to Marine Fortress
// 40) or the options (calm 10, rough 40), from 0 -- flat as glass -- up past
// anything the game uses: 100 was tried, the swells about five times Drake
// Lake's, and nothing broke.
//
// Wherever COURSE OUT is answered "inside", a current carries the jet ski
// back once it is well past where the countdown would have started and far
// from any land, stronger the further out; the courses' land, modelled well
// past what a race shows, stays free to explore. Without it, riding on far enough ends the run (a Stunt
// Mode run straight out of Dolphin Park ended about 15000 units out), and
// the water gets odd long before that.
//
// Buoys off: each course buoy (kinds 0 and 1 in the buoy list, D_801AEE20,
// 0x104 bytes each; 3 is the start, 4 the invisible turn markers, 2 Stunt
// Mode's checkpoints) has its +0xC4 cleared, which the collision and bobbing
// loops (func_8007DF4C, func_8007DBC0) skip, and its lists' calls are left
// out. Its place and +0xC8 stay as they are: the
// chase camera follows the buoys, and clearing +0xC8 (not drawn) or moving
// them sends it off to film from somewhere else. D_801CE638 is not the
// arrow's switch but the play view's: 0 gives the replay cameras and no HUD.
// The arrows over the buoys go with them. Rings off: their lists' calls are left out.
//
// WR64_FREE_ROAM=0 leaves the game's course-out alone, even on Dolphin Park;
// WR64_FREE_ROAM_LOG=1 logs the current.

#include "recomp.h"
#include "wr64/music.h"
#include "wr64/testdrive.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void func_80074264(uint8_t*, recomp_context*);

namespace wr64 {

// src/seafloor.cpp: how far a point is from the course's nearest land.
float land_distance(uint32_t course, float x, float z);
// src/land_backs.cpp: how far a point is from the backs of the course's land.
float land_backs_distance(uint32_t course, float x, float z);
// src/land_backs.cpp: whether a point is on the backs of the course's land.
bool land_backs_hold(uint32_t course, float x, float z, float above, float* out_x, float* out_z, float* way_x,
                     float* way_z, float* height);

namespace {

constexpr uint32_t kCourse = 0x0D8170, kWaveLevel = 0x0D49B0, kGameMode = 0x1CE620, kLapsToFinish = 0x1CE728,
                   kHumanSlot = 0x0D48DC, kRiders = 0x192690, kRiderSize = 0x1718,
                   kBuoys = 0x1AEE20, kBuoySize = 0x104, kBuoyCount = 0x1BB120, kRaceRecords = 0x1C2938,
                   kRaceRecordSize = 0x378;
constexpr int32_t kTimeTrials = 0, kStunt = 11;

// The current starts once the jet ski is both this many units past the last
// place inside the course (about 60 m at the VR scale) and kLandReach from
// any land, and reaches full strength kRamp further out.
constexpr float kMargin = 2000.0f, kLandReach = 3000.0f, kRamp = 2500.0f;
// The game rebuilds the jet ski's velocity each physics step from its speed
// (+0x68; full throttle is about 12) and heading, so the current works on
// those: heading out at full strength, the speed loses kBrake of itself a
// frame, which the throttle can only hold at a crawl; and the jet ski drifts
// back by up to kDrift units a frame (full throttle covers about 60).
constexpr float kBrake = 0.12f, kDrift = 3.0f;

bool env_is(const char* name, char wanted) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == wanted;
}
bool env_on(const char* name) { return env_is(name, '1'); }

// The settings, overridden for scripted tests by WR64_FREE_ROAM_MODE=1,
// WR64_FREE_ROAM_BUOYS=0 and WR64_FREE_ROAM_RINGS=0.
std::atomic<bool> g_on{env_on("WR64_FREE_ROAM_MODE")};
std::atomic<bool> g_buoys{!env_is("WR64_FREE_ROAM_BUOYS", '0')};
std::atomic<bool> g_rings{!env_is("WR64_FREE_ROAM_RINGS", '0')};
// The course whose sky and water a free roam takes, or -1 for its own;
// WR64_FREE_ROAM_LOOK=<course> for scripted tests.
std::atomic<int> g_look{std::getenv("WR64_FREE_ROAM_LOOK") ? std::atoi(std::getenv("WR64_FREE_ROAM_LOOK")) : -1};
// The look in force, for the port's own water and haze: the chosen course
// while a free roam is set up with it, else -1.
std::atomic<int> g_look_now{-1};
// The wave level chosen (D_800D49B0), or -1 for the course's own;
// WR64_FREE_ROAM_WAVES=<level> for scripted tests.
std::atomic<int> g_waves{std::getenv("WR64_FREE_ROAM_WAVES") ? std::atoi(std::getenv("WR64_FREE_ROAM_WAVES")) : -1};

// Whether every race is ridden without fog (see free_roam_frame);
// WR64_FOG=0 for scripted tests.
std::atomic<bool> g_no_fog{env_is("WR64_FOG", '0')};

// The course whose music every race plays, or -1 for each course's own;
// WR64_RACE_MUSIC=<course> for scripted tests.
std::atomic<int> g_music{std::getenv("WR64_RACE_MUSIC") ? std::atoi(std::getenv("WR64_RACE_MUSIC")) : -1};

// The human's last place inside the course, and whether it was outside when
// the game last asked.
struct Edge {
    bool known = false;
    bool outside = false;
    float x = 0.0f, z = 0.0f;
    int32_t course = -1;
} g_edge;

// RDRAM words, kept native.
int32_t word(const uint8_t* rdram, uint32_t address) {
    int32_t v;
    std::memcpy(&v, rdram + (address & 0x7FFFFF), sizeof v);
    return v;
}
void set_word(uint8_t* rdram, uint32_t address, int32_t v) { std::memcpy(rdram + (address & 0x7FFFFF), &v, sizeof v); }
float real(const uint8_t* rdram, uint32_t address) {
    float v;
    std::memcpy(&v, rdram + (address & 0x7FFFFF), sizeof v);
    return v;
}
void set_real(uint8_t* rdram, uint32_t address, float v) { std::memcpy(rdram + (address & 0x7FFFFF), &v, sizeof v); }

// The current, wherever COURSE OUT is answered "inside" (Dolphin Park, and
// every Free Roam), and only once far from both the course and any land:
// toward the last place that was near one or the other. Run from the game's
// own course-out test, between its physics steps: from the display-list
// rewriter, on another thread, its writes landed at odd moments of the
// game's frame and the jet ski stalled.
void current(uint8_t* rdram, int32_t course, float x, float z) {
    const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
    const float past_course = !g_edge.outside ? 0.0f
                              : g_edge.known  ? std::hypot(g_edge.x - x, g_edge.z - z) - kMargin
                                              : 1e9f;
    const float past_land = std::fmin(land_distance(uint32_t(course), x, z),
                                      land_backs_distance(uint32_t(course), x, z)) - kLandReach;
    const float past = std::fmin(past_course, past_land);
    static struct { bool known = false; int32_t course = -1; float x = 0.0f, z = 0.0f; } allowed;
    if (allowed.course != course) allowed = {};
    allowed.course = course;
    if (past <= 0.0f) {
        allowed.known = true;
        allowed.x = x;
        allowed.z = z;
        return;
    }
    if (!allowed.known) return;
    const float dx = allowed.x - x, dz = allowed.z - z;
    const float back = std::hypot(dx, dz);
    if (back < 1.0f) return;
    const float ux = dx / back, uz = dz / back;
    const float strength = std::fmin(1.0f, past / kRamp);
    // Heading out (the velocity's direction, away from that place), the jet
    // ski is braked; heading back, it is left alone, so turning round brings
    // the usual pull of the throttle straight away.
    // The jet ski is twelve hull points (+0x90, 0x20 apart: place, then
    // velocity at +0xC), its centre and velocity (+0x44, +0x50) worked out
    // from them, so the points are what is braked and moved.
    const float vx = real(rdram, rider + 0x50), vz = real(rdram, rider + 0x58);
    const float v = std::hypot(vx, vz);
    const float outward = v > 0.01f ? -(vx * ux + vz * uz) / v : 0.0f;
    const float keep = outward > 0.0f ? 1.0f - kBrake * strength * outward : 1.0f;
    const float shift_x = ux * kDrift * strength, shift_z = uz * kDrift * strength;
    for (uint32_t i = 0; i < 12; ++i) {
        const uint32_t point = rider + 0x90 + i * 0x20;
        set_real(rdram, point + 0x0, real(rdram, point + 0x0) + shift_x);
        set_real(rdram, point + 0x8, real(rdram, point + 0x8) + shift_z);
        set_real(rdram, point + 0xC, real(rdram, point + 0xC) * keep);
        set_real(rdram, point + 0x14, real(rdram, point + 0x14) * keep);
    }
    static const bool log = env_on("WR64_FREE_ROAM_LOG");
    static int frames = 0;
    if (log && ++frames % 15 == 0) {
        std::fprintf(stderr, "[wr64-free-roam] %.0f units out, current %.0f%%, speed %.1f, heading out %.2f (at %.0f, %.0f) anim %d\n",
                     past, strength * 100.0f, real(rdram, rider + 0x68), outward, x, z, word(rdram, rider + 0xC54));
    }
}

}  // namespace

// From the settings' Free Roam tab.
void set_free_roam(bool on) { g_on.store(on || env_on("WR64_FREE_ROAM_MODE"), std::memory_order_relaxed); }
void set_free_roam_buoys(bool on) { g_buoys.store(on && !env_is("WR64_FREE_ROAM_BUOYS", '0'), std::memory_order_relaxed); }
void set_free_roam_rings(bool on) { g_rings.store(on && !env_is("WR64_FREE_ROAM_RINGS", '0'), std::memory_order_relaxed); }
void set_free_roam_look(int course) {
    if (std::getenv("WR64_FREE_ROAM_LOOK") == nullptr) g_look.store(course, std::memory_order_relaxed);
}
void set_fog(bool on) {
    if (std::getenv("WR64_FOG") == nullptr) g_no_fog.store(!on, std::memory_order_relaxed);
}
void set_race_music(int course) {
    if (std::getenv("WR64_RACE_MUSIC") == nullptr) g_music.store(course, std::memory_order_relaxed);
}
void set_free_roam_waves(int level) {
    if (std::getenv("WR64_FREE_ROAM_WAVES") == nullptr) g_waves.store(level, std::memory_order_relaxed);
}

// Whether this race is a free roam: the switch on, in Time Trials or Stunt
// Mode, racing (state 0x28: not the title's demo, a Time Trial of its own).
bool free_roam_active(const uint8_t* rdram) {
    if (!g_on.load(std::memory_order_relaxed) || current_game_state() != 0x28) return false;
    const int32_t mode = word(rdram, kGameMode);
    return mode == kTimeTrials || mode == kStunt;
}

// The wave level a free roam is ridden at, or -1 for the course's own.
// Asked as the course loads, before the race's state, so by the mode chosen
// in the menus (D_801CE608.gameMode), as the sky's swap is, and never for the
// title's demo or the menus' backdrops.
int free_roam_waves(const uint8_t* rdram) {
    const uint32_t state = current_game_state();
    if (!g_on.load(std::memory_order_relaxed) || state == 0x02 || state == 0x03 || state == 0x05 || state == 0x07) return -1;
    const int32_t mode = word(rdram, 0x1CE608);
    return mode == kTimeTrials || mode == kStunt ? g_waves.load(std::memory_order_relaxed) : -1;
}

// Whether the race is paused (its pause menu's choice, -1 when not).
bool paused(const uint8_t* rdram) {
    return *reinterpret_cast<const int16_t*>(rdram + (0x1CE624 ^ 2)) != -1;
}

// For the display-list rewriter: whether a rectangle of a race's HUD, at
// (left, top)-(right, bottom) in the game's 320x240 frame, is left out. In a
// free roam, not paused, the HUD is the speed alone: its label, digits and
// "km/h", top middle in Time Trials, top right in Stunt Mode (from frames of
// each); the time, laps, best times, score, misses and power go.
bool free_roam_hides_rect(const uint8_t* rdram, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    if (!free_roam_active(rdram) || paused(rdram)) return false;
    const bool stunt = word(rdram, kGameMode) == kStunt;
    const uint32_t speed_left = stunt ? 230 : 158, speed_right = stunt ? 300 : 236;
    const bool speed = left >= speed_left && right <= speed_right && top >= 18 && bottom <= 60;
    return !speed;
}

// For the display-list rewriter: whether a top-level call to `list` is left
// out. Stunt Mode's rings are drawn by two lists of the shared segment, one
// call per ring (found by leaving lists out of a scripted run's frames). In
// any free roam: the direction arrow's model (0x01010528, under a
// projection of its own), the start's countdown lights (four lists, also
// under their own), and the messages popping up mid-screen (0x01014820).
bool free_roam_hides(const uint8_t* rdram, uint32_t list) {
    switch (list) {
        case 0x01010528: case 0x010106C0: case 0x01010730: case 0x01010998: case 0x010109F8: case 0x01014820:
            return free_roam_active(rdram) && !paused(rdram);
        default: break;
    }
    if (list == 0x0101EA48 || list == 0x0101E620) {
        return !g_rings.load(std::memory_order_relaxed) && free_roam_active(rdram) && word(rdram, kGameMode) == kStunt;
    }
    // The buoys, one call each: the race buoys (0x0102CC58 and 0x0102CC70 on
    // some courses, 0x0102CCE8 and 0x0102CD00 on the others) and the rows
    // marking the course's edges (0x0102CD78 and 0x0102CD90, or 0x0102CE08 and
    // 0x0102CE20), from a frame's trace of each course; the race buoys' striped
    // tails and L or R letters (0x01029BE0 and 0x01029C88 for the buoys passed
    // on one side, 0x0102AD90 and 0x0102AE38 for the other); and the arrows
    // over the next buoys (0x0102CE78, a sprite each).
    switch (list) {
        case 0x0102CC58: case 0x0102CC70: case 0x0102CCE8: case 0x0102CD00:
        case 0x0102CD78: case 0x0102CD90: case 0x0102CE08: case 0x0102CE20: case 0x0102CE78:
        case 0x01029BE0: case 0x01029C88: case 0x0102AD90: case 0x0102AE38:
            return !g_buoys.load(std::memory_order_relaxed) && free_roam_active(rdram) && word(rdram, kGameMode) == kTimeTrials;
        default: return false;
    }
}

namespace {

// A free roam's race, once a frame: no finish, no disqualification, no
// clock, no wrong way, no stranding, and the buoys as switched.
void race_rules_off(uint8_t* rdram, uint32_t slot) {
    set_word(rdram, kLapsToFinish, 99);
    // The buoys missed, in the human's race record: +0x134 counts toward
    // gBuoyMisses and disqualification (func_80077080 and on), +0x130 with it.
    const uint32_t record = kRaceRecords + slot * kRaceRecordSize;
    set_word(rdram, record + 0x130, 0);
    set_word(rdram, record + 0x134, 0);
    // Stunt Mode's countdown, in the human's race record: milliseconds left,
    // 15000 at the start (Time Trials leave it there).
    set_word(rdram, record + 0x2C4, 15000);
    // The wave level, as chosen: the water is set up with it (the hook on
    // func_8004F9E0), and it is read again as the race goes on (the water's
    // update, at 0x80050C38), where the race overlay sets it too.
    const int waves = g_waves.load(std::memory_order_relaxed);
    if (waves >= 0) set_word(rdram, kWaveLevel, waves);
    // The frames ridden the wrong way (the race overlay's count from
    // 0x801EFF00, past 40 of which it beeps every 20, sound 0x35), and those
    // spent stranded on land (the rider's +0x16D0, 81 of which start COURSE
    // OUT): neither ends a free roam, its land being there to explore.
    set_word(rdram, record + 0x328, 0);
    set_word(rdram, kRiders + slot * kRiderSize + 0x16D0, 0);

    if (g_buoys.load(std::memory_order_relaxed)) return;
    const int32_t count = word(rdram, kBuoyCount);
    for (int32_t i = 0; i < count && i < 256; ++i) {
        const uint32_t b = kBuoys + uint32_t(i) * kBuoySize;
        const int32_t kind = word(rdram, b + 0x98);
        if (kind == 0 || kind == 1) set_word(rdram, b + 0xC4, 0);
    }
}

}  // namespace

// func_80074264: whether a point is outside the course. Asked each frame of
// a race for each human, on the game's own thread: where a free roam's
// rules are kept too.
void free_roam_course_out_hook(uint8_t* rdram, recomp_context* ctx) {
    static const bool off = [] {
        const char* value = std::getenv("WR64_FREE_ROAM");
        return value != nullptr && value[0] == '0';
    }();
    const int32_t course = word(rdram, kCourse);
    const bool roam = free_roam_active(rdram);
    if (roam) race_rules_off(rdram, uint32_t(word(rdram, kHumanSlot)));
    if (off || (course != 0 && !roam)) {
        func_80074264(rdram, ctx);
        return;
    }
    // The game's own answer, kept for the current.
    const float x = ctx->f12.fl, z = ctx->f14.fl;
    func_80074264(rdram, ctx);
    if (course != g_edge.course) g_edge = Edge{};
    g_edge.course = course;
    g_edge.outside = ctx->r2 != 0;
    if (!g_edge.outside) {
        g_edge.known = true;
        g_edge.x = x;
        g_edge.z = z;
    }
    ctx->r2 = 0;
    current(rdram, course, x, z);
    // The backs of the course's land (src/land_backs.cpp), unknown to the
    // game: a jet ski coming up one is braked over its foot (from 30 below
    // the water, its way up the slope losing an eighth of its points
    // velocities a frame) and held where it leaves the water, as if beached.
    // (Stopped dead from full speed, it threw its rider.)
    float out_x, out_z, way_x, way_z, height;
    const bool held = land_backs_hold(uint32_t(course), x, z, -5.0f, &out_x, &out_z, &way_x, &way_z, &height);
    if (!held && height > -30.0f) {
        const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
        for (uint32_t i = 0; i < 12; ++i) {
            const uint32_t p = rider + 0x90 + i * 0x20;
            const float vx = real(rdram, p + 0xC), vz = real(rdram, p + 0x14);
            const float in = -(vx * way_x + vz * way_z);
            if (in > 0.0f) {
                set_real(rdram, p + 0xC, vx + 0.125f * in * way_x);
                set_real(rdram, p + 0x14, vz + 0.125f * in * way_z);
            }
        }
    }
    if (held) {
        const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
        const float dx = out_x - x, dz = out_z - z;
        for (uint32_t i = 0; i < 12; ++i) {
            const uint32_t p = rider + 0x90 + i * 0x20;
            set_real(rdram, p, real(rdram, p) + dx);
            set_real(rdram, p + 8, real(rdram, p + 8) + dz);
            const float vx = real(rdram, p + 0xC), vz = real(rdram, p + 0x14);
            const float in = -(vx * way_x + vz * way_z);
            if (in > 0.0f) {
                set_real(rdram, p + 0xC, vx + in * way_x);
                set_real(rdram, p + 0x14, vz + in * way_z);
            }
        }
        set_real(rdram, rider + 0x44, real(rdram, rider + 0x44) + dx);
        set_real(rdram, rider + 0x4C, real(rdram, rider + 0x4C) + dz);
    }
}


// WR64_FREE_ROAM_TEST_AT, once a frame of a race (see below).
void test_place(uint8_t* rdram) {
    // WR64_FREE_ROAM_TEST_AT=<x>,<z>,<degrees>, in a scripted run only: the
    // jet ski moved there, turned to face that way (0 along +x, 90 along
    // +z), for the first frames of a free roam -- for seeing a course's far
    // parts in a test.
    static float test_at[3] = { 0.0f, 0.0f, 0.0f };
    static const bool test = [] {
        const char* value = std::getenv("WR64_FREE_ROAM_TEST_AT");
        return value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr &&
               std::sscanf(value, "%f,%f,%f", &test_at[0], &test_at[1], &test_at[2]) == 3;
    }();
    static int test_frames = 0;
    if (test && free_roam_active(rdram) && test_frames < 90) {
        ++test_frames;
        const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
        // The hull's heading from its points (the first three the bow's, the
        // next three the stern's).
        float bow_x = 0, bow_z = 0, stern_x = 0, stern_z = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            bow_x += real(rdram, rider + 0x90 + i * 0x20) / 3.0f;
            bow_z += real(rdram, rider + 0x98 + i * 0x20) / 3.0f;
            stern_x += real(rdram, rider + 0x90 + (i + 3) * 0x20) / 3.0f;
            stern_z += real(rdram, rider + 0x98 + (i + 3) * 0x20) / 3.0f;
        }
        const float cx = real(rdram, rider + 0x44), cz = real(rdram, rider + 0x4C);
        const float turn = test_at[2] * 0.0174533f - std::atan2(bow_z - stern_z, bow_x - stern_x);
        const float c = std::cos(turn), s = std::sin(turn);
        for (uint32_t i = 0; i < 12; ++i) {
            const uint32_t p = rider + 0x90 + i * 0x20;
            const float px = real(rdram, p) - cx, pz = real(rdram, p + 8) - cz;
            const float vx = real(rdram, p + 0xC), vz = real(rdram, p + 0x14);
            set_real(rdram, p, test_at[0] + px * c - pz * s);
            set_real(rdram, p + 8, test_at[1] + px * s + pz * c);
            set_real(rdram, p + 0xC, vx * c - vz * s);
            set_real(rdram, p + 0x14, vx * s + vz * c);
        }
        set_real(rdram, rider + 0x44, test_at[0]);
        set_real(rdram, rider + 0x4C, test_at[1]);
    }
}

// The sky and water another course's. Each course's look -- its light, fog
// and sky colours, fog distances and light's direction, 0x110 bytes -- sits
// in a table (0x800D9964), and as a course loads the game copies its own into
// the race's (0x801CB058, twice). Its sky's pictures -- the gradient, the
// clouds near and far, the floor under the water -- are a list of seven
// packed textures in the cartridge, unpacked as it loads into segment 8 at
// the same places for every course; each course's list is pointed to from
// another table (0x800DC0E4).
//
// With a look chosen, in Time Trials or Stunt Mode with Free Roam on, every
// course's entries are the chosen course's instead -- but for its draw
// distances (+0xA0, +0xA4) -- from the course select on (every one: the
// course is known only as it is confirmed, and loads straight away); otherwise, and on
// the title screen, the main menu and the demo (races of their own, Time
// Trials by default), their own. Both are read only as a course loads, so
// they are kept so from any thread, once a frame.
//
// The fog too (settings, Graphics tab): off, in any race but the front's,
// where a look's fog begins and where it is whole (+0x78, +0x7C, which the
// scene's fog is set up from through D_801C0C80, the race's look; +0x98,
// +0x9C for Drake Lake's water) are moved past the far plane (1000 on the
// depth scale the N64's fog runs on, 0 at the near plane). The game's fog
// factor (0x1F400 over the span) is a 16-bit number, so the span is kept
// wide.
void free_roam_frame(uint8_t* rdram) {
    constexpr uint32_t kLooks = 0x0D9964, kLookSize = 0x110, kSkies = 0x0DC0E4;
    constexpr int kCourses = 9;
    static uint8_t original[kCourses][kLookSize];
    static int32_t sky[kCourses];
    static bool kept = false;
    if (!kept) {
        std::memcpy(original, rdram + kLooks, sizeof original);
        for (int c = 0; c < kCourses; ++c) sky[c] = word(rdram, kSkies + uint32_t(c) * 4);
        kept = true;
    }
    const uint32_t state = current_game_state();
    const bool front = state == 0x02 || state == 0x03 || state == 0x05 || state == 0x07;
    const int32_t course = word(rdram, kCourse);
    // The mode chosen in the menus (D_801CE608.gameMode), set before the
    // course loads; gGameModes is set from it as it does.
    const int32_t mode = word(rdram, 0x1CE608);
    const int chosen = g_look.load(std::memory_order_relaxed);
    const bool swap = !front && g_on.load(std::memory_order_relaxed) && (mode == kTimeTrials || mode == kStunt) &&
                      chosen >= 0 && chosen < kCourses;
    for (int c = 0; c < kCourses; ++c) {
        uint8_t look[kLookSize];
        std::memcpy(look, original[swap ? chosen : c], kLookSize);
        std::memcpy(look + 0xA0, original[c] + 0xA0, 8);
        if (g_no_fog.load(std::memory_order_relaxed) && !front) {
            constexpr int32_t kPast[2] = { 1000, 1100 };
            std::memcpy(look + 0x78, kPast, sizeof kPast);
            std::memcpy(look + 0x98, kPast, sizeof kPast);
        }
        if (std::memcmp(rdram + kLooks + uint32_t(c) * kLookSize, look, kLookSize) != 0) {
            std::memcpy(rdram + kLooks + uint32_t(c) * kLookSize, look, kLookSize);
        }
        set_word(rdram, kSkies + uint32_t(c) * 4, sky[swap ? chosen : c]);
    }
    g_look_now.store(swap && course >= 0 && course < kCourses ? chosen : -1, std::memory_order_relaxed);
    if (state == 0x28) test_place(rdram);
}

// The course whose colours the port's own water and haze take for course
// `course`: the chosen look's in a free roam set up with one.
uint32_t look_course(uint32_t course) {
    const int now = g_look_now.load(std::memory_order_relaxed);
    return now >= 0 ? uint32_t(now) : course;
}

}  // namespace wr64

// From the hook on func_8004F9E0 (recomp/wr64.toml): the wave level a
// course's water is set up with -- in a free roam, the one chosen, if any.
extern "C" int wr64_wave_level(uint8_t* rdram, int level) {
    const int chosen = wr64::free_roam_waves(rdram);
    return chosen >= 0 ? chosen : level;
}
// From the hook on func_800C32A0 (recomp/wr64.toml): whether voice line
// `line` is left out. In a free roam, the announcer's: the countdown (0x0D,
// 0x0C, 0x0B), "go" (0x0A) and the rest below 0x60 (logged in a time trial);
// the riders' own shouts (from 0x60: the random ones are 0x6C to 0x6F,
// func_800C3548) stay.
extern "C" int wr64_voice_silenced(uint8_t* rdram, int line, int priority, uint32_t caller) {
    (void)priority;
    (void)caller;
    if (line >= 0x60 || !wr64::free_roam_active(rdram)) return 0;
    static const bool log = wr64::env_on("WR64_FREE_ROAM_LOG");
    if (log) std::fprintf(stderr, "[wr64-free-roam] voice line 0x%02X left out\n", line);
    return 1;
}

// From the hook on AudioGeneral_PlaySound (recomp/wr64.toml): whether sound
// `sound` is left out. In a free roam, the start's countdown beeps (0x22, once
// for each light) and the one for the go (0x23), the lights being hidden.
extern "C" int wr64_sound_silenced(uint8_t* rdram, int sound) {
    if ((sound != 0x22 && sound != 0x23) || !wr64::free_roam_active(rdram)) return 0;
    static const bool log = wr64::env_on("WR64_FREE_ROAM_LOG");
    if (log) std::fprintf(stderr, "[wr64-free-roam] sound 0x%02X left out\n", sound);
    // The go's sound also starts the course's music: in a race (the sound
    // driver's mode 7, D_800E7C94), with nothing holding it back
    // (D_800E7CB8), it sets D_800E7CD8 to 10, ten ticks after which the
    // music begins (func_800BF370), and the announcer's timers; set here as
    // the sound itself would have.
    if (sound == 0x23 && int8_t(rdram[0x0E7C94 ^ 3]) == 7 && wr64::word(rdram, 0x800E7CB8) == 0) {
        wr64::set_word(rdram, 0x800E7CD8, 10);
        wr64::set_word(rdram, 0x800E7CD4, 0xB4);
        wr64::set_word(rdram, 0x800E7CA0, 1);
        wr64::set_word(rdram, 0x800E7CE8, 0x140);
    }
    return 1;
}

// From the hook on func_800C1540 (recomp/wr64.toml), which starts a piece of
// music (a0, the sequence): in a race, a course's music (course + 6, as the
// game picks it) is heard as the chosen course's, if one is. The game still
// plays its own: another course's instruments are loaded into this course's
// room for them, and some don't fit (Twilight City's on Marine Fortress
// crashed in Audio_PatchBank). So the swap is the recordings' alone, and
// with the original music, or no recording of the chosen music, there is none.
extern "C" int wr64_race_music(uint8_t* rdram, int sequence) {
    (void)rdram;
    const int chosen = wr64::g_music.load(std::memory_order_relaxed);
    const bool swap = chosen >= 0 && sequence >= 6 && sequence <= 14 && wr64::current_game_state() == 0x28;
    wr64::music::set_race_sequence(-1);
    if (swap && wr64::music::replacement_available(uint8_t(chosen + 6))) wr64::music::set_race_sequence(chosen + 6);
    return sequence;
}

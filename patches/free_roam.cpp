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
// A free roam is chosen in the game's main menu (FREE ROAM, src/game_menus.cpp)
// and is a Stunt Mode run, on any of the nine courses. In one, every course
// is answered "inside" too, and the race never ends: the laps it takes to
// finish (D_801CE728) are held out of reach, the buoys missed are held at
// none, and Stunt Mode's clock is held.
//
// On Dolphin Park it can be the championship's Warm Up instead (the Free Roam
// options' LAYOUT): a Championship with one rider at Dolphin Park is the Warm
// Up, which starts by the beach with a dolphin to follow and never ends. As
// the free roam starts, the menus' choice (D_801CE608) is made that: mode 4,
// one player, one rider, Normal, the Warm Up not skipped, and the race's mode
// (gGameModes) with it; and Stunt Mode's
// again as the course select sets up, should its pause menu go back there.
// That pause menu (the table at 0x80226BD0: an action, an x offset and a
// word each) offers START THE RACE, the championship; in a free roam its row
// is COURSE SELECT, Stunt Mode's (action 5, word 0x2E), and the cursor starts
// on CONTINUE rather than on it. The help ticker along the bottom, which
// tells how to start racing, goes.
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
// A free roam has no buoys and no rings: they are what Time Trials and Stunt
// Mode are for. Each course buoy (kinds 0 and 1 in the buoy list, D_801AEE20,
// 0x104 bytes each; 3 is the start, 4 the invisible turn markers, 2 Stunt
// Mode's checkpoints) has its +0xC4 cleared, which the collision and bobbing
// loops (func_8007DF4C, func_8007DBC0) skip, and its lists' calls are left
// out. Its place and +0xC8 stay as they are: the
// chase camera follows the buoys, and clearing +0xC8 (not drawn) or moving
// them sends it off to film from somewhere else. D_801CE638 is not the
// arrow's switch but the play view's: 0 gives the replay cameras and no HUD.
// The arrows over the buoys go with them, and in Stunt Mode the CHECK POINT
// gates and the buoy beside each. The rings' lists' calls are left out too.
//
// WR64_FREE_ROAM=0 leaves the game's course-out alone, even on Dolphin Park;
// WR64_FREE_ROAM_LOG=1 logs the current.

#include "recomp.h"
#include "wr64/day_night.h"
#include "wr64/journal.h"
#include "wr64/music.h"
#include "wr64/testdrive.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

extern "C" void func_80074264(uint8_t*, recomp_context*);
extern "C" int32_t wr64_ramp_pieces(void);

namespace wr64 {

// src/seafloor.cpp: how far a point is from the course's nearest land.
float land_distance(uint32_t course, float x, float z);
// src/guest_course.cpp: a guest course in place of the base course.
namespace guest {
void frame(uint8_t* rdram, uint32_t state);
bool active(const uint8_t* rdram);
bool start(float now_x, float now_z, float& x, float& z, float& heading);
float past_bounds(float x, float z);
}
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
constexpr int32_t kTimeTrials = 0, kVersus = 1, kChampionship = 4, kStunt = 11;
// The menus' choice (D_801CE608: the mode, then the players and riders as
// halves), the difficulty, whether the Warm Up is skipped; the pause menu's
// row, the choice made in it (D_801CE628), and the Warm Up's pause rows.
constexpr uint32_t kChosenMode = 0x1CE608, kChosenPlayers = 0x1CE60C, kChosenRiders = 0x1CE60E,
                   kDifficulty = 0x1CB338, kSkipWarmUp = 0x1CB334, kPauseRow = 0x1CE624, kPauseChoice = 0x1CE628,
                   kWarmUpPauseRows = 0x226BD0;

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

// Whether the run is a free roam (set as the main menu ends; for scripted
// tests, WR64_FREE_ROAM_MODE=1).
std::atomic<bool> g_on{env_on("WR64_FREE_ROAM_MODE")};
// The difficulty Stunt Mode had before a Warm Up, given back after it.
int32_t g_stunt_difficulty = 0;

// Whether a free roam rides in mode `mode`: Time Trials or Stunt Mode, or a
// Championship, which in one is only ever Dolphin Park's Warm Up.
bool roam_mode(int32_t mode) { return mode == kTimeTrials || mode == kStunt || mode == kChampionship; }
// The course whose sky and water a free roam takes, or -1 for its own;
// WR64_FREE_ROAM_LOOK=<course> for scripted tests.
std::atomic<int> g_look{std::getenv("WR64_FREE_ROAM_LOOK") ? std::atoi(std::getenv("WR64_FREE_ROAM_LOOK")) : -1};
// The look in force, for the port's own water and haze: the chosen course
// while a free roam is set up with it, else -1.
std::atomic<int> g_look_now{-1};
// The wave level chosen (D_800D49B0), or -1 for the course's own;
// WR64_FREE_ROAM_WAVES=<level> for scripted tests.
std::atomic<int> g_waves{std::getenv("WR64_FREE_ROAM_WAVES") ? std::atoi(std::getenv("WR64_FREE_ROAM_WAVES")) : -1};

// The wave level chosen for Championship, Time Trials, Stunt Mode and 2P VS
// (their WAVES page, src/game_menus.cpp), or -1 for the course's own.
std::atomic<int> g_race_waves{-1};
// REVERSE's misses and laps (the Races tab), held in its races: the game
// sets 5 and 3 for it, whatever its CHANGE CONDITIONS say.
std::atomic<int> g_reverse_misses{5}, g_reverse_laps{3};
// The most laps a race may have: the game keeps nine laps' times (its own
// CHANGE CONDITIONS goes to 9), and a tenth ran its lap count ahead and
// ended a ten-lap race after seven or eight.
constexpr int kMostLaps = 9;

// Whether a free roam is ridden without fog (see free_roam_frame);
// WR64_FOG=0 for scripted tests.
std::atomic<bool> g_no_fog{env_is("WR64_FOG", '0')};

// The sound left out once, the next time it is asked for, or -1: the game's
// "start" chime as a menu page opens where the race would have started.
std::atomic<int> g_swallow{-1};

// The course whose music a free roam plays, or -1 for each course's own;
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
// (On a guest course, src/guest_course.cpp, how far past its bounds
// instead.)
void current(uint8_t* rdram, int32_t course, float x, float z) {
    const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
    const float past_course = !g_edge.outside ? 0.0f
                              : g_edge.known  ? std::hypot(g_edge.x - x, g_edge.z - z) - kMargin
                                              : 1e9f;
    const float past_land = std::fmin(land_distance(uint32_t(course), x, z),
                                      land_backs_distance(uint32_t(course), x, z)) - kLandReach;
    const float past = guest::active(rdram) ? guest::past_bounds(x, z) : std::fmin(past_course, past_land);
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

// From the game's main menu (src/game_menus.cpp): whether the run it starts
// is a free roam.
void set_free_roam(bool on) { g_on.store(on || env_on("WR64_FREE_ROAM_MODE"), std::memory_order_relaxed); }
bool free_roam_chosen() { return g_on.load(std::memory_order_relaxed); }

// F2 (src/callbacks.cpp): marks asked for, taken by the next race frame.
std::atomic<int> g_marks{ 0 };
void request_mark() { g_marks.fetch_add(1, std::memory_order_relaxed); }
// From src/game_menus.cpp: leaves `sound` out the next time it is asked for.
void swallow_sound_once(int sound) { g_swallow.store(sound, std::memory_order_relaxed); }
// From the settings (the WAVES pages and the Free Roam options).
void set_race_waves(int level) { g_race_waves.store(level, std::memory_order_relaxed); }
void set_reverse_conditions(int misses, int laps) {
    g_reverse_misses.store(misses, std::memory_order_relaxed);
    g_reverse_laps.store(std::min(laps, kMostLaps), std::memory_order_relaxed);
}
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
// From src/game_menus.cpp, as a free roam on Dolphin Park starts: its Warm Up.
void start_warm_up(uint8_t* rdram) {
    if (word(rdram, kChosenMode) != kStunt) return;
    g_stunt_difficulty = word(rdram, kDifficulty);
    set_word(rdram, kChosenMode, kChampionship);
    // And the race's own (gGameModes), which the championship's way in sets
    // from it but the course select's does not.
    set_word(rdram, kGameMode, kChampionship);
    *reinterpret_cast<int16_t*>(rdram + (kChosenPlayers ^ 2)) = 1;
    *reinterpret_cast<int16_t*>(rdram + (kChosenRiders ^ 2)) = 1;
    set_word(rdram, kDifficulty, 0);
    set_word(rdram, kSkipWarmUp, 0);
    std::fprintf(stderr, "[wr64] free roam: Dolphin Park's Warm Up\n");
}
// From src/game_menus.cpp, as the course select sets up: back from a Warm Up
// to Stunt Mode.
void leave_warm_up(uint8_t* rdram) {
    if (const char* d = std::getenv("WR64_TEST_DIFFICULTY")) set_word(rdram, kDifficulty, std::atoi(d));
    if (!g_on.load(std::memory_order_relaxed) || word(rdram, kChosenMode) != kChampionship) return;
    set_word(rdram, kChosenMode, kStunt);
    set_word(rdram, kGameMode, kStunt);
    set_word(rdram, kDifficulty, g_stunt_difficulty);
}

// Whether this race is a free roam: the switch on, in Time Trials or Stunt
// Mode, racing (state 0x28: not the title's demo, a Time Trial of its own).
bool free_roam_active(const uint8_t* rdram) {
    if (!g_on.load(std::memory_order_relaxed) || current_game_state() != 0x28) return false;
    return roam_mode(word(rdram, kGameMode));
}

// The wave level a run is ridden at, or -1 for the course's own: a free
// roam's from its options, any other mode's from its WAVES page. Asked as
// the course loads, before the race's state, so by the mode chosen in the
// menus (D_801CE608.gameMode), as the sky's swap is, and never for the
// title's demo or the menus' backdrops.
int free_roam_waves(const uint8_t* rdram) {
    const uint32_t state = current_game_state();
    if (state == 0x02 || state == 0x03 || state == 0x05 || state == 0x07) return -1;
    const int32_t mode = word(rdram, 0x1CE608);
    if (g_on.load(std::memory_order_relaxed) && roam_mode(mode)) {
        return g_waves.load(std::memory_order_relaxed);
    }
    return mode == kTimeTrials || mode == kStunt || mode == kVersus || mode == kChampionship
               ? g_race_waves.load(std::memory_order_relaxed) : -1;
}

// Whether the race is paused (its pause menu's choice, -1 when not).
bool paused(const uint8_t* rdram) {
    return *reinterpret_cast<const int16_t*>(rdram + (0x1CE624 ^ 2)) != -1;
}

// For the display-list rewriter: whether a rectangle of a race's HUD, at
// (left, top)-(right, bottom) in the game's 320x240 frame, is left out. In a
// free roam, not paused, the HUD is the speed alone: its label, digits and
// "km/h", top middle in Time Trials, top right in Stunt Mode (from frames of
// each); the time, laps, best times, score, misses and power go. The Warm Up
// shows only the speed already; its help ticker, along the bottom, goes,
// paused or not (the pause menu's box ends above it).
bool free_roam_hides_rect(const uint8_t* rdram, uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    if (!free_roam_active(rdram)) return false;
    if (word(rdram, kGameMode) == kChampionship) return top >= 188;
    if (paused(rdram)) return false;
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
    // WR64_CALL_LOG=1: the segment-1 lists called in a free roam, and how
    // often, now and then; WR64_HIDE_TEST=<hex>: that one left out.
    static const bool call_log = std::getenv("WR64_CALL_LOG") != nullptr;
    if (call_log && free_roam_active(rdram) && (list >> 24) == 0x01) {
        static std::map<uint32_t, int> counts;
        static int calls = 0;
        ++counts[list];
        if (++calls % 50000 == 0) {
            for (const auto& [l, n] : counts) std::fprintf(stderr, "[wr64-calls] %08X %d\n", l, n);
            std::fprintf(stderr, "[wr64-calls] --\n");
            counts.clear();
        }
    }
    static const uint32_t hide_test = [] {
        const char* value = std::getenv("WR64_HIDE_TEST");
        return value != nullptr ? uint32_t(std::strtoul(value, nullptr, 16)) : 0u;
    }();
    if (hide_test != 0 && list == hide_test) return free_roam_active(rdram);
    switch (list) {
        case 0x01010528: case 0x010106C0: case 0x01010730: case 0x01010998: case 0x010109F8: case 0x01014820:
            return free_roam_active(rdram) && !paused(rdram);
        default: break;
    }
    if (list == 0x0101EA48 || list == 0x0101E620) {
        return free_roam_active(rdram);
    }
    // On a guest course (src/guest_course.cpp) the base course's boundary
    // rows, checkpoint gates and their buoys too.
    switch (list) {
        case 0x0102CD78: case 0x0102CD90: case 0x0102CE08: case 0x0102CE20:
        case 0x01018AA8: case 0x01018D58: case 0x0102CB78:
            if (guest::active(rdram)) return true;
            break;
        default: break;
    }
    // The buoys, one call each: the race buoys (0x0102CC58 and 0x0102CC70 on
    // some courses, 0x0102CCE8 and 0x0102CD00 on the others), from a frame's
    // trace of each course; the rows marking the course's edges (0x0102CD78
    // and 0x0102CD90, or 0x0102CE08 and 0x0102CE20) are drawn. The race buoys' striped
    // tails and L or R letters (0x01029BE0 and 0x01029C88 for the buoys passed
    // on one side, 0x0102AD90 and 0x0102AE38 for the other); and the arrows
    // over the next buoys (0x0102CE78, a sprite each). Not Stunt Mode's CHECK
    // POINT gates (0x01018AA8 and 0x01018D58, the banner and its posts) or
    // the buoy by each (0x0102CB78): a free roam is a Stunt Mode run, its
    // checkpoints (kind 2) still stop a jet ski, and hidden they were
    // invisible walls (the user, 2026-11-02).
    switch (list) {
        case 0x0102CC58: case 0x0102CC70: case 0x0102CCE8: case 0x0102CD00:
        case 0x0102CE78:
        case 0x01029BE0: case 0x01029C88: case 0x0102AD90: case 0x0102AE38:
            return free_roam_active(rdram);
        default: return false;
    }
}

namespace {

// A free roam's race, once a frame: no finish, no disqualification, no
// clock, no wrong way, no stranding, and no buoys.
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

    const int32_t count = word(rdram, kBuoyCount);
    for (int32_t i = 0; i < count && i < 256; ++i) {
        const uint32_t b = kBuoys + uint32_t(i) * kBuoySize;
        const int32_t kind = word(rdram, b + 0x98);
        if (kind == 0 || kind == 1) set_word(rdram, b + 0xC4, 0);
    }
}

}  // namespace

// Whether a free roam's Warm Up has started its music yet. The championship's
// way in starts it from the course's overview, which a free roam passes by:
// so its first race frame starts it as the go's sound would (see
// wr64_sound_silenced), once the sound driver is racing.
std::atomic<bool> g_music_started{false};
void start_music(uint8_t* rdram) {
    if (g_music_started.load(std::memory_order_relaxed)) return;
    if (int8_t(rdram[0x0E7C94 ^ 3]) != 7 || word(rdram, 0x800E7CB8) != 0) return;
    set_word(rdram, 0x800E7CD8, 10);
    set_word(rdram, 0x800E7CD4, 0xB4);
    set_word(rdram, 0x800E7CA0, 1);
    set_word(rdram, 0x800E7CE8, 0x140);
    g_music_started.store(true, std::memory_order_relaxed);
}

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
    if (roam && word(rdram, kGameMode) == kChampionship) start_music(rdram);
    // A race's chosen waves, held as a free roam's are: the race overlay
    // sets the level again as the race goes on. Not in the title's demo.
    if (!roam && current_game_state() != 0x07) {
        const int waves = free_roam_waves(rdram);
        if (waves >= 0) set_word(rdram, kWaveLevel, waves);
    }
    // WR64_LAP_TEST=<lap>,<laps> (scripted runs): a race's first frames on
    // that lap of that many, the lap message shown.
    {
        static int test[2] = { 0, 0 };
        static const bool testing = std::getenv("WR64_LAP_TEST") && std::getenv("WR64_INPUT_SCRIPT") &&
                                    std::sscanf(std::getenv("WR64_LAP_TEST"), "%d,%d", &test[0], &test[1]) == 2;
        static int frames = 0;
        if (testing && current_game_state() == 0x28 && frames < 400) {
            ++frames;
            const uint32_t record = kRaceRecords + uint32_t(word(rdram, kHumanSlot)) * kRaceRecordSize;
            set_word(rdram, kLapsToFinish, test[1]);
            if (frames % 100 == 1) { set_word(rdram, record, test[0] + frames / 100); set_word(rdram, record + 0x308, 60); }
        }
    }
    // Any other race: never more than kMostLaps (a CHANGE CONDITIONS of 10
    // saved before they went to 9).
    if (!roam && current_game_state() == 0x28 && word(rdram, kLapsToFinish) > kMostLaps) set_word(rdram, kLapsToFinish, kMostLaps);
    // A REVERSE championship's race (the flag D_800DAB68): its chosen misses
    // allowed (gBuoyMisses, 0x800D8174) and laps to finish.
    if (!roam && current_game_state() == 0x28 && word(rdram, kGameMode) == kChampionship &&
        *reinterpret_cast<const int16_t*>(rdram + (0x0DAB68 ^ 2)) != 0) {
        set_word(rdram, 0x0D8174, g_reverse_misses.load(std::memory_order_relaxed));
        set_word(rdram, kLapsToFinish, g_reverse_laps.load(std::memory_order_relaxed));
    }
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
    // (On a guest course, by its own bounds; nothing else here is the base
    // course's to do there: its land is not there.)
    current(rdram, course, x, z);
    if (guest::active(rdram)) return;
    // The backs of the course's land (src/land_backs.cpp), unknown to the
    // game: a jet ski coming up one is braked over its foot (from 30 below
    // the water, its way up the slope losing an eighth of its points
    // velocities a frame) and held where it leaves the water, as if beached.
    // (Stopped dead from full speed, it threw its rider.)
    float out_x, out_z, way_x, way_z, height;
    const bool held = land_backs_hold(uint32_t(course), x, z, -5.0f, &out_x, &out_z, &way_x, &way_z, &height);
    static const bool hold_log = env_on("WR64_LAND_HOLD_LOG");
    if (hold_log && height > -30.0f) {
        std::fprintf(stderr, "[wr64] land hold: asked at %.0f %.0f, rider at %.0f %.0f, slope %.0f, %s to %.0f %.0f\n", x, z,
                     real(rdram, kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize + 0x44),
                     real(rdram, kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize + 0x4C), height,
                     held ? "held" : "braked", out_x, out_z);
    }
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


// WR64_TEST_RAMP=<x>,<z>,<degrees>[;<x>,<z>,<degrees>...], in a scripted
// run only: ramps added there, each ridden that way (as the course's own:
// 270 along -x, 7 nearly along +z; the way over is (sin, cos) of it). Ramps
// are made as the course sets up (func_80071E70, see docs/COURSE_CREATOR.md)
// from a table per course and mode -- D_800D66CC for Stunt Mode, D_800D66A4
// otherwise -- of five s16s each (x, z, two zeros, the angle; ending at an
// angle of -1), into the course's objects (D_801BC940), with all that goes
// with them. Adding one to the objects afterwards gives neither a ramp drawn
// nor one ridden, so instead Dolphin Park's two tables are pointed, before it
// loads, at copies with the ramps added, in memory nothing else uses
// (kRampTables, above the game's 8 MB; patches/ramp_pieces.cpp). The jet
// ski's height is logged while it is well up, its place every half second.
// A negative angle first adds none; WR64_TEST_RAMP_INSTEAD=<n> leaves the
// course's ramp n out.
void test_ramp(uint8_t* rdram) {
    static std::vector<std::array<float, 3>> ramps;
    static const bool test = [] {
        const char* value = std::getenv("WR64_TEST_RAMP");
        if (value == nullptr || std::getenv("WR64_INPUT_SCRIPT") == nullptr) return false;
        for (const char* p = value; p != nullptr && *p != '\0';) {
            std::array<float, 3> r{};
            if (std::sscanf(p, "%f,%f,%f", &r[0], &r[1], &r[2]) != 3) break;
            ramps.push_back(r);
            p = std::strchr(p, ';');
            if (p != nullptr) ++p;
        }
        return !ramps.empty();
    }();
    if (!test) return;
    constexpr uint32_t kTables[2] = { 0x0D66A4, 0x0D66CC }, kRampTables = 0x80804000u, kTableRoom = 0x200;
    static bool pointed = false;
    if (!pointed && ramps[0][2] >= 0.0f) {
        pointed = true;
        // Above 8 MB: past the 0x7FFFFF the port's own helpers keep to.
        auto set_half = [&](uint32_t a, int16_t v) { *reinterpret_cast<int16_t*>(rdram + (((a - 0x80000000u)) ^ 2)) = v; };
        // Dolphin Park's tables only: its race table has no ramps, its Stunt
        // Mode table the four (ROM 0x2A9C18), which are written out again
        // here (the course's data, loaded with it, isn't there to copy yet).
        constexpr int16_t kStuntRamps[4][5] = {
            { 2500, -4952, 0, 0, 270 }, { 92, -4952, 0, 0, 270 }, { -2407, -4952, 0, 0, 270 }, { -6065, 664, 0, 0, 7 },
        };
        static const int instead = std::getenv("WR64_TEST_RAMP_INSTEAD") ? std::atoi(std::getenv("WR64_TEST_RAMP_INSTEAD")) : -1;
        for (uint32_t t = 0; t < 2; ++t) {
            const uint32_t to = kRampTables + t * kTableRoom;
            uint32_t n = 0;
            auto put = [&](const int16_t (&e)[5]) {
                for (uint32_t k = 0; k < 5; ++k) set_half(to + n * 10 + k * 2, e[k]);
                ++n;
            };
            if (t == 1) {
                for (int i = 0; i < 4; ++i) if (i != instead) put(kStuntRamps[i]);
            }
            for (const auto& r : ramps) {
                const int16_t e[5] = { int16_t(r[0]), int16_t(r[1]), 0, 0, int16_t(r[2]) };
                put(e);
            }
            const int16_t end[5] = { -1, -1, -1, -1, -1 };
            put(end);
            set_word(rdram, kTables[t], int32_t(to));
        }
        std::fprintf(stderr, "[wr64] test ramp: Dolphin Park's ramp tables point at copies with %zu ramp(s) added\n", ramps.size());
    }
    if (current_game_state() != 0x28) return;
    const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
    const float x = real(rdram, rider + 0x44), y = real(rdram, rider + 0x48), z = real(rdram, rider + 0x4C);
    static int frames = 0;
    ++frames;
    if ((y > 20.0f && frames % 3 == 0) || frames % 30 == 0) std::fprintf(stderr, "[wr64] test ramp: the jet ski at %.0f %.0f %.0f\n", x, y, z);
    static bool listed = false;
    if (!listed && frames > 30) {
        listed = true;
        constexpr uint32_t kObjects = 0x1BC940, kObjectSize = 0xC4;
        const int32_t count = word(rdram, 0x1BFA40);
        std::fprintf(stderr, "[wr64] test ramp: %d slope pieces built\n", word(rdram, 0x1C0578));
        // Each piece, from wherever the game keeps them (patches/ramp_pieces.cpp).
        for (int32_t i = 0; i < word(rdram, 0x1C0578); ++i) {
            const uint8_t* p = rdram + (uint32_t(wr64_ramp_pieces()) - 0x80000000u) + uint32_t(i) * 64;
            std::fprintf(stderr, "[wr64] test ramp: piece %d", i);
            for (int k = 0; k < 16; ++k) {
                float v;
                std::memcpy(&v, p + k * 4, 4);
                std::fprintf(stderr, " %.2f", v);
            }
            std::fprintf(stderr, "\n");
        }
        for (int32_t i = 0; i < count; ++i) {
            const uint32_t o = kObjects + uint32_t(i) * kObjectSize;
            if (word(rdram, o + 0x4C) != 1) continue;
            std::fprintf(stderr, "[wr64] test ramp: object %d, a ramp at %.0f %.0f, %.0f degrees, slot %d\n", i, real(rdram, o),
                         real(rdram, o + 8), real(rdram, o + 0x48), word(rdram, o + 0x64));
        }
    }
}

// WR64_FREE_ROAM_TEST_AT, once a frame of a race (see below).
void test_place(uint8_t* rdram) {
    // WR64_FREE_ROAM_TEST_AT=<x>,<z>,<degrees>, in a scripted run only: the
    // jet ski moved there, turned to face that way (0 along +x, 90 along
    // +z), for the first frames of a race or free roam -- for seeing a
    // course's far parts, or riding the same stretch in both, in a test.
    static float test_at[3] = { 0.0f, 0.0f, 0.0f };
    static const bool test = [] {
        const char* value = std::getenv("WR64_FREE_ROAM_TEST_AT");
        return value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr &&
               std::sscanf(value, "%f,%f,%f", &test_at[0], &test_at[1], &test_at[2]) == 3;
    }();
    static int test_frames = 0;
    // (A guest course's own start, likewise: src/guest_course.cpp.)
    float guest_at[3];
    const uint32_t ridden = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
    const bool guest = guest::start(real(rdram, ridden + 0x44), real(rdram, ridden + 0x4C), guest_at[0], guest_at[1], guest_at[2]);
    if (guest) std::copy(std::begin(guest_at), std::end(guest_at), std::begin(test_at));
    if ((guest || (test && test_frames < 90)) && current_game_state() == 0x28) {
        if (!guest) ++test_frames;
        const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
        // The hull's heading from its points (the first three the stern's,
        // the next three the bow's, as for a mark below).
        float bow_x = 0, bow_z = 0, stern_x = 0, stern_z = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            stern_x += real(rdram, rider + 0x90 + i * 0x20) / 3.0f;
            stern_z += real(rdram, rider + 0x98 + i * 0x20) / 3.0f;
            bow_x += real(rdram, rider + 0x90 + (i + 3) * 0x20) / 3.0f;
            bow_z += real(rdram, rider + 0x98 + (i + 3) * 0x20) / 3.0f;
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

// A free roam's Warm Up's pause menu (see the top): COURSE SELECT for START
// THE RACE, the cursor on CONTINUE as it opens; the game's own otherwise.
void warm_up_pause(uint8_t* rdram) {
    const bool roam = free_roam_active(rdram) && word(rdram, kGameMode) == kChampionship;
    const uint32_t row = kWarmUpPauseRows + 8;
    const uint32_t want[2] = { roam ? 0x0005FFEAu : 0x0006FFE8u, roam ? 0x2Eu : 0x45u };
    for (uint32_t i = 0; i < 2; ++i) {
        if (uint32_t(word(rdram, row + i * 4)) != want[i]) set_word(rdram, row + i * 4, int32_t(want[i]));
    }
    int16_t& at = *reinterpret_cast<int16_t*>(rdram + (kPauseRow ^ 2));
    const int16_t choice = *reinterpret_cast<const int16_t*>(rdram + (kPauseChoice ^ 2));
    static int16_t last = -1;
    if (roam && last == -1 && at == 1 && choice == 0) at = 0;
    last = at;
}

// Day and night (src/day_night.cpp): the race's look (0x801CB058, one for
// each player's view), which the game reads every frame, blended between the
// two looks either side of the hour: colours and fog distances mixed,
// the two lights' directions turned from one to the other. The course's own
// draw distances (+0xA0, +0xA4) are kept, and what isn't known yet (+0xA8,
// +0xC4, +0xC8, +0x108, +0x10C) is the look's that the ride opened with,
// whose sky was loaded.
constexpr uint32_t kLookSize = 0x110;
void day_frame(uint8_t* rdram, const uint8_t* original, int32_t course) {
    constexpr uint32_t kRaceLook = 0x1CB058;
    const day::Blend b = day::now();
    const uint8_t* a = original + b.from * kLookSize;
    const uint8_t* z = original + b.to * kLookSize;
    const uint8_t* opened = original + day::start_look(course) * kLookSize;
    uint8_t look[kLookSize];
    for (uint32_t at = 0; at < kLookSize; at += 4) {
        const bool real_number = (at >= 0xAC && at <= 0xB4) || (at >= 0xFC && at <= 0x104);
        if (real_number) {
            float p, q;
            std::memcpy(&p, a + at, 4);
            std::memcpy(&q, z + at, 4);
            const float v = p + (q - p) * b.t;
            std::memcpy(look + at, &v, 4);
        } else {
            int32_t p, q;
            std::memcpy(&p, a + at, 4);
            std::memcpy(&q, z + at, 4);
            const int32_t v = int32_t(std::lround(p + (q - p) * double(b.t)));
            std::memcpy(look + at, &v, 4);
        }
    }
    std::memcpy(look + 0xA0, original + course * kLookSize + 0xA0, 8);
    for (uint32_t at : { 0xA8u, 0xC4u, 0xC8u, 0x108u, 0x10Cu }) std::memcpy(look + at, opened + at, 4);
    if (g_no_fog.load(std::memory_order_relaxed)) {
        constexpr int32_t kPast[2] = { 1000, 1100 };
        std::memcpy(look + 0x78, kPast, sizeof kPast);
        std::memcpy(look + 0x98, kPast, sizeof kPast);
    }
    for (uint32_t view = 0; view < 2; ++view) std::memcpy(rdram + kRaceLook + view * kLookSize, look, kLookSize);
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
// The fog too (the Free Roam options): off, in a free roam's race,
// where a look's fog begins and where it is whole (+0x78, +0x7C, which the
// scene's fog is set up from through D_801C0C80, the race's look; +0x98,
// +0x9C for Drake Lake's water) are moved past the far plane (1000 on the
// depth scale the N64's fog runs on, 0 at the near plane). The game's fog
// factor (0x1F400 over the span) is a 16-bit number, so the span is kept
// wide.
void free_roam_frame(uint8_t* rdram) {
    constexpr uint32_t kLooks = 0x0D9964, kSkies = 0x0DC0E4;
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
    const bool roaming = !front && g_on.load(std::memory_order_relaxed) && roam_mode(mode);
    const bool swap = roaming && chosen >= 0 && chosen < kCourses;
    // Day and night: each course opens with its hour's look (and that look's
    // sky), and the race's own copy is blended each frame (day_frame).
    const bool day = roaming && day::enabled();
    for (int c = 0; c < kCourses; ++c) {
        uint8_t look[kLookSize];
        const int from = day ? day::start_look(c) : swap ? chosen : c;
        std::memcpy(look, original[from], kLookSize);
        std::memcpy(look + 0xA0, original[c] + 0xA0, 8);
        if (g_no_fog.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) && !front) {
            constexpr int32_t kPast[2] = { 1000, 1100 };
            std::memcpy(look + 0x78, kPast, sizeof kPast);
            std::memcpy(look + 0x98, kPast, sizeof kPast);
        }
        if (std::memcmp(rdram + kLooks + uint32_t(c) * kLookSize, look, kLookSize) != 0) {
            std::memcpy(rdram + kLooks + uint32_t(c) * kLookSize, look, kLookSize);
        }
        set_word(rdram, kSkies + uint32_t(c) * 4, sky[from]);
    }
    g_look_now.store(swap && !day && course >= 0 && course < kCourses ? chosen : -1, std::memory_order_relaxed);
    const bool paused = *reinterpret_cast<const int16_t*>(rdram + ((kPauseRow ^ 2) & 0x7FFFFF)) != -1;
    day::frame(day && state == 0x28 && course >= 0 && course < kCourses, paused, course);
    if (day::active()) {
        day_frame(rdram, &original[0][0], course);
        day::sky_frame(rdram, sky, day::start_look(course));
    }
    guest::frame(rdram, state);
    if (state == 0x28) test_place(rdram);
    test_ramp(rdram);
    // WR64_LAP_LOG=1: each rider's lap (race record +0) and the HUD's lap
    // (+0x320) whenever either changes, with the laps to finish, the buoy the
    // arrow points at next (+0x0C), the line-side flag (+0x324) and where the
    // rider is.
    static const bool lap_log = env_on("WR64_LAP_LOG");
    static bool lap_log_racing = false;
    static int lap_log_frames = 0;
    if (lap_log && state != 0x28 && lap_log_racing) {
        lap_log_racing = false;
        lap_log_frames = 0;
        std::fprintf(stderr, "[wr64] laps: the race is over\n");
    }
    if (lap_log && state == 0x28) {
        lap_log_racing = true;
        // Once a race: how laps are counted (D_800D8178 chooses between the
        // game's two ways), the mode and course; then each time the human's
        // line-side flag (+0x324) flips, where.
        const uint32_t human = kRaceRecords + uint32_t(word(rdram, kHumanSlot)) * kRaceRecordSize;
        static int32_t flag = -2;
        if (lap_log_frames++ == 0) {
            flag = -2;
            std::fprintf(stderr, "[wr64] laps: a race on course %d, mode %d, difficulty %d, counting %d, laps %d\n", word(rdram, kCourse),
                         word(rdram, kGameMode), word(rdram, kDifficulty), word(rdram, 0x0D8178), word(rdram, kLapsToFinish));
            std::fprintf(stderr, "[wr64] laps: conditions %d %d %d / %d %d %d / %d %d %d\n", word(rdram, 0x1CB308),
                         word(rdram, 0x1CB30C), word(rdram, 0x1CB310), word(rdram, 0x1CB314), word(rdram, 0x1CB318),
                         word(rdram, 0x1CB31C), word(rdram, 0x1CB320), word(rdram, 0x1CB324), word(rdram, 0x1CB328));
        }
        if (word(rdram, human + 0x324) != flag) {
            flag = word(rdram, human + 0x324);
            const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
            std::fprintf(stderr, "[wr64] laps: the human's line flag %d at %.0f %.0f, lap %d, HUD %d, next buoy %d\n", flag,
                         real(rdram, rider + 0x44), real(rdram, rider + 0x4C), word(rdram, human), word(rdram, human + 0x320),
                         word(rdram, human + 0x0C));
        }
        static int32_t seen[4][2] = {};
        for (uint32_t slot = 0; slot < 4; ++slot) {
            const uint32_t record = kRaceRecords + slot * kRaceRecordSize;
            const int32_t lap = word(rdram, record), hud = word(rdram, record + 0x320);
            if (lap != seen[slot][0] || hud != seen[slot][1]) {
                seen[slot][0] = lap;
                seen[slot][1] = hud;
                const uint32_t rider = kRiders + slot * kRiderSize;
                std::fprintf(stderr, "[wr64] laps: rider %u lap %d, HUD %d, of %d, next buoy %d, flag %d, at %.0f %.0f (human slot %d)\n",
                             slot, lap, hud, word(rdram, kLapsToFinish), word(rdram, record + 0x0C),
                             word(rdram, record + 0x324), real(rdram, rider + 0x44), real(rdram, rider + 0x4C),
                             word(rdram, kHumanSlot));
            }
        }
    }
    // WR64_POSITION_LOG=1: the human's jet ski five times a second in a race
    // -- the clock time (to the millisecond, to match screenshots by), the
    // course, where, how high, its speed (+0x68) and which way its velocity
    // goes -- for finding, from a play-test, what it ran into.
    // F2, in a race: a mark -- the time (UTC, to match a screenshot by), the
    // course, where the human's jet ski is and which way its hull faces
    // (degrees, 0 along +x, 90 along +z) -- added to marks.txt beside the log,
    // to the log and to the decisions journal (src/journal.cpp); for marking a
    // course's sights from a ride, or a moment worth talking over.
    if (g_marks.exchange(0, std::memory_order_relaxed) > 0 && state == 0x28) {
        const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
        // The hull's six points: the stern's three, then the bow's (the
        // other way round put the first Sunny Beach marks' facings 180
        // degrees out, against the way the jet ski had been going).
        float bow_x = 0, bow_z = 0, stern_x = 0, stern_z = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            stern_x += real(rdram, rider + 0x90 + i * 0x20);
            stern_z += real(rdram, rider + 0x98 + i * 0x20);
            bow_x += real(rdram, rider + 0x90 + (i + 3) * 0x20);
            bow_z += real(rdram, rider + 0x98 + (i + 3) * 0x20);
        }
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        gmtime_s(&local, &now);
        char when[32];
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &local);
        static int count = 0;
        char line[200];
        std::snprintf(line, sizeof line, "mark %d: %s course %d at %.0f %.0f %.0f, facing %.0f\n", ++count, when,
                      word(rdram, kCourse), real(rdram, rider + 0x44), real(rdram, rider + 0x48), real(rdram, rider + 0x4C),
                      std::atan2(bow_z - stern_z, bow_x - stern_x) * 57.29578f);
        std::fprintf(stderr, "[wr64] %s", line);
        journal::Fields f;
        f.add("mark", count).add("course", word(rdram, kCourse)).add("x", double(real(rdram, rider + 0x44)))
            .add("y", double(real(rdram, rider + 0x48))).add("z", double(real(rdram, rider + 0x4C)))
            .add("facing", double(std::atan2(bow_z - stern_z, bow_x - stern_x) * 57.29578f));
        journal::write("mark", f);
        if (const char* local_app = std::getenv("LOCALAPPDATA")) {
            if (std::FILE* f = std::fopen((std::string(local_app) + "\\WaveRace64Recomp\\marks.txt").c_str(), "a")) {
                std::fputs(line, f);
                std::fclose(f);
            }
        }
    }
    static const bool position_log = env_on("WR64_POSITION_LOG");
    if (position_log && state == 0x28) {
        static int frames = 0;
        if (frames++ % 6 == 0) {
            const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
            const float vx = real(rdram, rider + 0x50), vz = real(rdram, rider + 0x58);
            std::fprintf(stderr, "[wr64] position: %lld course %d at %.0f %.0f %.0f, speed %.1f, heading %.0f\n", ms,
                         word(rdram, kCourse), real(rdram, rider + 0x44), real(rdram, rider + 0x48), real(rdram, rider + 0x4C),
                         real(rdram, rider + 0x68), std::atan2(vz, vx) * 57.29578f);
        }
    }
    // WR64_TEST_AUTOPILOT=1, in a scripted run only: the human's rider driven
    // by the game's computer (its +0x1446 negative, as a computer's rider's:
    // func_80061708), everything else about it still the human's.
    static const bool autopilot = env_on("WR64_TEST_AUTOPILOT") && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
    if (autopilot && state == 0x28) {
        const uint32_t rider = kRiders + uint32_t(word(rdram, kHumanSlot)) * kRiderSize;
        *reinterpret_cast<int16_t*>(rdram + ((rider + 0x1446) ^ 2)) = -1;
    }
    warm_up_pause(rdram);
    if (state != 0x28) g_music_started.store(false, std::memory_order_relaxed);
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
    if (chosen >= 0 && chosen != level) {
        std::fprintf(stderr, "[wr64] waves: level %d chosen (the course's own %d)\n", chosen, level);
    }
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
    int swallowed = sound;
    if (sound >= 0 && wr64::g_swallow.compare_exchange_strong(swallowed, -1, std::memory_order_relaxed)) return 1;
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
// music (a0, the sequence): in a free roam's race, a course's music (course
// + 6, as the game picks it) is heard as the chosen course's, if one is. The game still
// plays its own: another course's instruments are loaded into this course's
// room for them, and some don't fit (Twilight City's on Marine Fortress
// crashed in Audio_PatchBank). So the swap is the recordings' alone, and
// with the original music, or no recording of the chosen music, there is none.
extern "C" int wr64_race_music(uint8_t* rdram, int sequence) {
    (void)rdram;
    const int chosen = wr64::g_music.load(std::memory_order_relaxed);
    const bool swap = chosen >= 0 && wr64::g_on.load(std::memory_order_relaxed) && sequence >= 6 && sequence <= 14 &&
                      wr64::current_game_state() == 0x28;
    wr64::music::set_race_sequence(-1);
    if (swap && wr64::music::replacement_available(uint8_t(chosen + 6))) wr64::music::set_race_sequence(chosen + 6);
    return sequence;
}

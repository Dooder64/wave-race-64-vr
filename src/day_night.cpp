// Day and night in Free Roam.
//
// A day is 24 minutes, a game hour a real minute, counted only while a free
// roam is ridden and not paused. Each time of day is one course's look (its
// light, fog and sky colours and the sun's direction, the table at
// 0x800D9964): held for a while, then faded into the next one's -- see
// kDay. patches/free_roam.cpp writes the blend into the race's look each
// frame (the game reads it every frame: the fog colour and range into the
// frame's list, the sun into the water's highlight), and src/water.cpp and
// src/first_person.cpp blend the port's own water colours the same way.
//
// A ride starts at its course's own hour (kStart), so it opens looking as the
// course does; courses that aren't one of the day's looks start in the
// morning.
//
// The sky's pictures are loaded only as the course loads, so they are blended
// in place (sky_frame): each course's seven (the list its entry in the table
// at 0x800DC0E4 points to: a cartridge start and end, a kind, and where it
// goes, 16 bytes each) are unpacked once from the cartridge's MIO0 chunks;
// the opened look's are found in memory by their contents; and while the
// hour's two looks are another pair, each is rewritten as their blend:
//
//   0, 1  the sky's gradient, 16x16 RGBA16 (twice)
//   2, 3  clouds, 32x64 IA16 (twice)
//   4     the water's texture, 32x64 RGBA16
//   5     the sun, 32x32 or 32x64 IA16 (Dolphin Park's smaller) -- or,
//         on Sunny Beach, Drake Lake, Marine Fortress, Port Blue and
//         Southern Island, the water's texture again (its chunk is picture
//         4's), so it is blended only with its own kind: a sun with suns
//         (and kept, where the other look has none), water with water
//   6     the far clouds, 32x64 IA16
//
// (measured by unpacking each and looking.) RGBA16 is blended channel by
// channel, IA16 byte by byte. The HD pack's replacements are keyed by a
// picture's contents, so a blend between looks shows the originals.
//
// WR64_DAY=1 turns it on for scripted tests; WR64_DAY_SPEED=<n> runs the
// clock n times as fast; WR64_DAY_HOUR=<h> starts every ride at that hour;
// WR64_DAY_LOG=1 logs the hour every ten seconds.

#include "wr64/day_night.h"

#include <librecomp/game.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <vector>

namespace wr64::day {
namespace {

enum Course { kDolphinPark, kSunnyBeach, kSunsetBay, kDrakeLake, kMarineFortress, kPortBlue, kTwilightCity,
              kGlacierCoast, kSouthernIsland };

// The day: each look held from `from` to `until` (hours), then faded into
// the next over the gap to its `from`, smoothly. The last fades into the
// first across midnight.
struct Hold { float from, until; int course; };
constexpr Hold kDay[] = {
    { 5.5f, 6.5f, kDrakeLake },        // dawn: mist, soft light
    { 7.5f, 10.0f, kDolphinPark },     // morning
    { 11.0f, 14.0f, kSouthernIsland }, // noon
    { 15.0f, 17.0f, kPortBlue },       // afternoon
    { 18.0f, 19.5f, kSunsetBay },      // sunset
    { 21.0f, 28.0f, kTwilightCity },   // night, to 4 in the morning
};
constexpr int kHolds = sizeof(kDay) / sizeof(kDay[0]);

// Where each course's ride starts on the clock: in the middle of its own
// look's hold, or the morning's.
float start_hour(int course) {
    for (const Hold& h : kDay)
        if (h.course == course) return std::fmod((h.from + h.until) / 2.0f, 24.0f);
    return 8.0f;
}

double env_number(const char* name, double fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::atof(text) : fallback;
}

std::atomic<bool> g_enabled{std::getenv("WR64_DAY") != nullptr && std::getenv("WR64_DAY")[0] == '1'};
std::mutex g_mutex;
bool g_active = false;
int g_course = -1;
double g_hour = 8.0;
std::chrono::steady_clock::time_point g_last;

Blend blend_at(double hour) {
    hour = std::fmod(hour, 24.0);
    if (hour < 0) hour += 24.0;
    for (int i = 0; i < kHolds; ++i) {
        const Hold& h = kDay[i];
        const Hold& next = kDay[(i + 1) % kHolds];
        // This hold and the fade after it, unwrapped onto [h.from, h.from + 24).
        double at = hour;
        while (at < h.from) at += 24.0;
        if (at <= h.until) return { h.course, h.course, 0.0f };
        double next_from = next.from;
        while (next_from < h.until) next_from += 24.0;
        if (at < next_from) {
            const double f = (at - h.until) / (next_from - h.until);
            return { h.course, next.course, float(f * f * (3.0 - 2.0 * f)) };
        }
    }
    return { kDay[0].course, kDay[0].course, 0.0f };
}

}  // namespace

void set_enabled(bool on) {
    if (std::getenv("WR64_DAY") == nullptr) g_enabled.store(on, std::memory_order_relaxed);
}
bool enabled() { return g_enabled.load(std::memory_order_relaxed); }

int start_look(int course) { return blend_at(env_number("WR64_DAY_HOUR", start_hour(course))).from; }

void frame(bool riding, bool paused, int course) {
    static const double speed = env_number("WR64_DAY_SPEED", 1.0);
    static const bool log = std::getenv("WR64_DAY_LOG") != nullptr;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_mutex);
    if (!enabled() || !riding) {
        g_active = false;
        g_course = -1;
        return;
    }
    if (!g_active || course != g_course) {
        g_hour = env_number("WR64_DAY_HOUR", start_hour(course));
        g_last = now;
        g_active = true;
        g_course = course;
        std::fprintf(stderr, "[wr64] day: a ride on course %d starts at %.1f h\n", course, g_hour);
        return;
    }
    const double seconds = std::chrono::duration<double>(now - g_last).count();
    g_last = now;
    if (paused || seconds > 1.0) return;
    const double before = g_hour;
    g_hour = std::fmod(g_hour + seconds * speed / 60.0, 24.0);
    if (log && std::floor(before * 6.0 / speed) != std::floor(g_hour * 6.0 / speed)) {
        const Blend b = blend_at(g_hour);
        std::fprintf(stderr, "[wr64] day: %.2f h, look %d -> %d at %.2f\n", g_hour, b.from, b.to, b.t);
    }
}

bool active() {
    std::lock_guard lock(g_mutex);
    return g_active;
}

Blend now() {
    std::lock_guard lock(g_mutex);
    return blend_at(g_hour);
}

float hour() {
    std::lock_guard lock(g_mutex);
    return float(g_hour);
}

namespace {

constexpr int kCourses = 9, kPictures = 7;

// RDRAM, kept in native words.
uint32_t ram_word(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const uint32_t*>(rdram + (address & 0x7FFFFC));
}
uint8_t ram_byte(const uint8_t* rdram, uint32_t address) { return rdram[(address & 0x7FFFFF) ^ 3]; }
void set_ram_byte(uint8_t* rdram, uint32_t address, uint8_t v) { rdram[(address & 0x7FFFFF) ^ 3] = v; }

// MIO0 chunk `at` of the cartridge, unpacked; empty if it isn't one.
std::vector<uint8_t> unpack(std::span<const uint8_t> rom, uint32_t at) {
    auto be = [&](uint32_t a) -> uint32_t {
        if (size_t(a) + 4 > rom.size()) return 0;
        return uint32_t(rom[a]) << 24 | uint32_t(rom[a + 1]) << 16 | uint32_t(rom[a + 2]) << 8 | rom[a + 3];
    };
    std::vector<uint8_t> out;
    if (be(at) != 0x4D494F30u) return out;    // "MIO0"
    const uint32_t size = be(at + 4);
    uint32_t pairs = at + be(at + 8), raw = at + be(at + 12), bits = at + 16, word = 0, mask = 0;
    if (size > 0x10000) return out;
    out.reserve(size);
    while (out.size() < size) {
        if (mask == 0) {
            word = be(bits);
            bits += 4;
            mask = 0x80000000u;
        }
        if (word & mask) {
            if (raw >= rom.size()) return {};
            out.push_back(rom[raw++]);
        } else {
            if (size_t(pairs) + 1 >= rom.size()) return {};
            const uint32_t pair = uint32_t(rom[pairs]) << 8 | rom[pairs + 1];
            pairs += 2;
            const uint32_t length = (pair >> 12) + 3, back = (pair & 0xFFF) + 1;
            if (back > out.size()) return {};
            for (uint32_t i = 0; i < length && out.size() < size; ++i) out.push_back(out[out.size() - back]);
        }
        mask >>= 1;
    }
    return out;
}

struct Skies {
    bool unpacked = false;
    std::vector<uint8_t> pictures[kCourses][kPictures];
    // Where the opened look's pictures lie, and the blend they were last
    // written with.
    int opened = -1;
    std::vector<uint32_t> places[kPictures];
    int written_from = -1, written_to = -1, written_step = -1;
} g_skies;

// Whether course `c`'s picture 5 is the water's texture again, not a sun.
bool water_twice(const Skies& g, int c) { return g.pictures[c][5] == g.pictures[c][4]; }
bool rgba16(const Skies& g, int opened, int picture) {
    return picture == 0 || picture == 1 || picture == 4 || (picture == 5 && water_twice(g, opened));
}
// Course `c`'s picture `p` as it blends into the opened look's: picture 5
// only with its own kind.
const std::vector<uint8_t>& like(const Skies& g, int c, int p, int opened) {
    if (p != 5 || water_twice(g, c) == water_twice(g, opened)) return g.pictures[c][p];
    return water_twice(g, opened) ? g.pictures[c][4] : g.pictures[opened][5];
}

// Every place in the game's 4 MB holding `picture`, 8-aligned.
std::vector<uint32_t> find(const uint8_t* rdram, const std::vector<uint8_t>& picture) {
    std::vector<uint32_t> found;
    if (picture.size() < 16) return found;
    for (uint32_t at = 0; at + picture.size() <= 0x400000; at += 8) {
        if (ram_byte(rdram, at) != picture[0]) continue;
        bool same = true;
        for (uint32_t i = 1; i < picture.size() && same; ++i) same = ram_byte(rdram, at + i) == picture[i];
        if (same) found.push_back(at);
    }
    return found;
}

}  // namespace

void sky_frame(uint8_t* rdram, const int32_t lists[9], int opened) {
    Skies& g = g_skies;
    if (!g.unpacked) {
        g.unpacked = true;
        const std::span<const uint8_t> rom = recomp::get_rom();
        if (rom.size() < 0x1000 || rom[0] != 0x80) {
            std::fprintf(stderr, "[wr64] day: the cartridge can't be read as it is; the sky won't change\n");
            return;
        }
        for (int c = 0; c < kCourses; ++c) {
            for (int p = 0; p < kPictures; ++p) {
                const uint32_t entry = uint32_t(lists[c]) + uint32_t(p) * 16;
                g.pictures[c][p] = unpack(rom, ram_word(rdram, entry));
            }
        }
    }
    if (opened < 0 || opened >= kCourses || g.pictures[opened][0].empty()) return;
    if (g.opened != opened) {
        g.opened = opened;
        g.written_from = g.written_to = g.written_step = -1;
        int found = 0;
        for (int p = 0; p < kPictures; ++p) {
            // A picture given twice is found twice; find each place once.
            g.places[p].clear();
            bool earlier = false;
            for (int q = 0; q < p; ++q) earlier |= g.pictures[opened][q] == g.pictures[opened][p];
            if (!earlier) g.places[p] = find(rdram, g.pictures[opened][p]);
            found += int(g.places[p].size());
        }
        std::fprintf(stderr, "[wr64] day: found %d places of the sky's pictures in memory (look %d):", found, opened);
        for (int p = 0; p < kPictures; ++p) std::fprintf(stderr, " %d", int(g.places[p].size()));
        std::fprintf(stderr, "\n");
    }
    const Blend b = blend_at(hour());
    const int step = b.from == b.to ? 0 : int(b.t * 64.0f + 0.5f);
    if (b.from == g.written_from && b.to == g.written_to && step == g.written_step) return;
    g.written_from = b.from;
    g.written_to = b.to;
    g.written_step = step;
    const float t = float(step) / 64.0f;
    for (int p = 0; p < kPictures; ++p) {
        const std::vector<uint8_t>& own = g.pictures[opened][p];
        const std::vector<uint8_t>& a = like(g, b.from, p, opened);
        const std::vector<uint8_t>& z = like(g, b.to, p, opened);
        // Past the end of a smaller picture (the sun), the opened look's own.
        auto at = [&](const std::vector<uint8_t>& v, size_t i) { return i < v.size() ? v[i] : own[i]; };
        for (uint32_t place : g.places[p]) {
            if (rgba16(g, opened, p)) {
                for (size_t i = 0; i + 1 < own.size(); i += 2) {
                    const uint32_t u = uint32_t(at(a, i)) << 8 | at(a, i + 1);
                    const uint32_t v = uint32_t(at(z, i)) << 8 | at(z, i + 1);
                    uint32_t out = (t < 0.5f ? u : v) & 1;
                    for (int shift : { 11, 6, 1 }) {
                        const float x = float((u >> shift) & 31), y = float((v >> shift) & 31);
                        out |= uint32_t(std::lround(x + (y - x) * t)) << shift;
                    }
                    set_ram_byte(rdram, place + uint32_t(i), uint8_t(out >> 8));
                    set_ram_byte(rdram, place + uint32_t(i) + 1, uint8_t(out));
                }
            } else {
                for (size_t i = 0; i < own.size(); ++i) {
                    const float x = at(a, i), y = at(z, i);
                    set_ram_byte(rdram, place + uint32_t(i), uint8_t(std::lround(x + (y - x) * t)));
                }
            }
        }
    }
}

}  // namespace wr64::day

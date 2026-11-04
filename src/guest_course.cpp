// Guest courses: places from other games ridden in Free Roam
// (docs/GUEST_COURSES.md). The first: Majora's Mask's Great Bay Coast,
// from a pack tools/guest/greatbay_pack.py builds out of the player's own
// Majora's Mask ROM (nothing of that game is in this repository).
//
// While one is on (WR64_GUEST=greatbay, for now), a free roam on Dolphin
// Park (the base course) becomes Great Bay:
//
//   - the pack is copied to physical kBase (the seafloor's vertex area: our
//     seafloor is off on a guest course, and is rebuilt after);
//   - the base course's scenery (its top-level segment 13 lists) is left out
//     of the frame, and the pack's display list drawn in the seafloor's
//     place (src/dlrewrite.cpp), under the pack's matrix (Majora's Mask
//     units scaled to Wave Race's);
//   - the course's collision is the pack's: one of the game's bounded planes
//     per triangle (as func_8007F448 tests them: the plane through its first
//     corner, two edges' planes facing out, a band to the third edge), in a
//     grid of its own, the game's pointers (D_801C3B40-84) set to them each
//     frame; the base course's ramps' slope pieces are counted out, and its
//     walls (a grid of upright lines of their own) emptied;
//   - the base course's markers are gone: its buoys and checkpoint gates
//     (the count D_801BB120 held at 0) and the floating things after them
//     (Dolphin Park's boundary buoys, from 0x801BB138, 0x18 apart) moved far
//     out of the way, so neither drawn nor met;
//   - the jet ski starts where the pack says, the Free Roam current, our
//     riders, the dolphin and the fish are off.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wr64 {
bool free_roam_active(const uint8_t* rdram);

namespace guest {
namespace people {
void load(const std::vector<uint8_t>& data, uint32_t people, uint32_t people_at, uint32_t models, uint32_t models_at, float water_x0,
          float water_z0, float water_cell, uint32_t water_w, uint32_t water_h, uint32_t water_at, float world_scale);
void frame(uint8_t* rdram, double now, float dt, float player_x, float player_z, float player_speed, bool player_known);
}
namespace {

constexpr uint32_t kBase = 0x400000;          // the pack (tools/guest/greatbay_pack.py BASE)
constexpr uint32_t kPlanes = 0x4C0000;        // the collision's planes, 64 bytes each
constexpr uint32_t kList = 0x4EE000;          // its cells' lists of planes (u16): room for 2944 planes before them
constexpr uint32_t kListEnd = 0x4F8000;
constexpr uint32_t kGrid = 0x4F8000;          // its grid (s16 into the list)
constexpr uint32_t kGridEnd = 0x500000;
constexpr uint32_t kCourse = 0x0D8170;
constexpr uint32_t kSlopePieceCount = 0x1C0578;

struct Header {
    char magic[8];
    uint32_t version, size, list, matrix, collision, triangles;
    float water, start_x, start_z, start_heading, scale;
    // Its people (version 3, src/guest_people.cpp): how many and where
    // their table is, the same of their models.
    uint32_t people, people_table, models, models_table;
    // Where it may be roamed (Wave Race units): past it the Free Roam
    // current carries the jet ski back (patches/free_roam.cpp).
    float bounds_x0, bounds_x1, bounds_z0, bounds_z1;
    // The open water its people may swim in: a byte a square (Majora's
    // Mask units).
    float water_x0, water_z0, water_cell;
    uint32_t water_w, water_h, water_grid;
};

struct Pack {
    bool tried = false, ok = false;
    std::vector<uint8_t> data;
    Header header{};
} g_pack;

bool g_loaded = false;      // in memory this ride
bool g_took = false;        // the vertex area had a pack in it (src/seafloor.cpp gives it back)
int g_frames = 0;           // frames of this ride (for the start)
uint32_t g_columns_log = 0, g_rows = 0, g_origin_x = 0, g_origin_z = 0, g_extent_x = 0, g_extent_z = 0;
constexpr uint32_t kShift = 10;               // 1024-unit cells

void put_word(uint8_t* rdram, uint32_t address, uint32_t value) { std::memcpy(rdram + (address & 0x7FFFFC), &value, 4); }
void put_real(uint8_t* rdram, uint32_t address, float value) { std::memcpy(rdram + (address & 0x7FFFFC), &value, 4); }
void put_half(uint8_t* rdram, uint32_t address, uint16_t value) {
    std::memcpy(rdram + ((address & 0x7FFFFE) ^ 2), &value, 2);
}

bool load_pack() {
    if (g_pack.tried) return g_pack.ok;
    g_pack.tried = true;
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr) return false;
    const std::string path = std::string(local) + "\\WaveRace64Recomp\\guest\\greatbay\\greatbay.pack";
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "[wr64-guest] no pack at %s (tools/guest/greatbay_pack.py makes it)\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    g_pack.data.resize(size_t(std::max(0L, size)));
    const size_t read = std::fread(g_pack.data.data(), 1, g_pack.data.size(), f);
    std::fclose(f);
    if (read != g_pack.data.size() || read < sizeof(Header)) return false;
    std::memcpy(&g_pack.header, g_pack.data.data(), sizeof(Header));
    if (std::memcmp(g_pack.header.magic, "WR64GBAY", 8) != 0 || g_pack.header.version != 3 ||
        g_pack.header.size != g_pack.data.size() || kBase + g_pack.header.size > kPlanes) {
        std::fprintf(stderr, "[wr64-guest] %s is not a pack this build reads\n", path.c_str());
        return false;
    }
    g_pack.ok = true;
    std::fprintf(stderr, "[wr64-guest] Great Bay Coast's pack: %u bytes, %u collision triangles\n", g_pack.header.size,
                 g_pack.header.triangles);
    return true;
}

struct V { float x, y, z; };
V sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V unit(V a) { const float l = std::sqrt(dot(a, a)); return l > 1e-6f ? V{ a.x / l, a.y / l, a.z / l } : V{ 0, 0, 0 }; }
V neg(V a) { return { -a.x, -a.y, -a.z }; }

// The pack copied in (an N64 image, big-endian: each word swapped into the
// host's order the recompiled memory keeps), and the collision built.
void install(uint8_t* rdram) {
    g_took = true;
    const std::vector<uint8_t>& d = g_pack.data;
    for (size_t i = 0; i + 4 <= d.size(); i += 4) {
        const uint32_t w = (uint32_t(d[i]) << 24) | (uint32_t(d[i + 1]) << 16) | (uint32_t(d[i + 2]) << 8) | d[i + 3];
        put_word(rdram, kBase + uint32_t(i), w);
    }
    // The planes.
    const Header& h = g_pack.header;
    std::vector<std::array<V, 3>> tris;
    float x0 = 1e9f, z0 = 1e9f, x1 = -1e9f, z1 = -1e9f;
    for (uint32_t t = 0; t < h.triangles; ++t) {
        std::array<V, 3> tri;
        std::memcpy(tri.data(), d.data() + h.collision + t * 36, 36);
        tris.push_back(tri);
        for (const V& v : tri) { x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); z0 = std::min(z0, v.z); z1 = std::max(z1, v.z); }
    }
    uint32_t planes = 0;
    std::vector<uint32_t> ids;            // plane id (1-based) per kept triangle
    std::vector<std::array<float, 4>> boxes;
    for (const auto& t : tris) {
        const V a = t[0], b = t[1], c = t[2];
        const V n = unit(cross(sub(b, a), sub(c, a)));
        if (dot(n, n) < 0.5f) continue;   // degenerate
        V e1 = unit(cross(sub(b, a), n));
        if (dot(sub(c, a), e1) > 0) e1 = neg(e1);
        V e2 = unit(cross(sub(c, a), n));
        if (dot(sub(b, a), e2) > 0) e2 = neg(e2);
        V e3 = unit(cross(sub(c, b), n));
        if (dot(sub(b, a), e3) < 0) e3 = neg(e3);
        const float height = dot(sub(b, a), e3);
        if (height < 1.0f) continue;
        if (kPlanes + (planes + 1) * 64 > kList) break;
        const uint32_t p = kPlanes + planes * 64;
        const float values[16] = { a.x, a.y, a.z, n.x, n.y, n.z, e1.x, e1.y, e1.z, e2.x, e2.y, e2.z, e3.x, e3.y, e3.z, -height };
        for (int i = 0; i < 16; ++i) put_real(rdram, p + uint32_t(i) * 4, values[i]);
        ++planes;
        boxes.push_back({ std::min({ a.x, b.x, c.x }) - 32, std::max({ a.x, b.x, c.x }) + 32,
                          std::min({ a.z, b.z, c.z }) - 32, std::max({ a.z, b.z, c.z }) + 32 });
    }
    // The grid: 1024-unit cells over the planes' extent, a power of two
    // columns wide; each cell's list a leading slot, its planes, then 0.
    g_origin_x = uint32_t(int32_t(std::floor(x0)) - 64);
    g_origin_z = uint32_t(int32_t(std::floor(z0)) - 64);
    g_extent_x = uint32_t(int32_t(x1 - x0) + 128);
    g_extent_z = uint32_t(int32_t(z1 - z0) + 128);
    const uint32_t columns = (g_extent_x >> kShift) + 1;
    g_columns_log = 0;
    while ((1u << g_columns_log) < columns) ++g_columns_log;
    g_rows = (g_extent_z >> kShift) + 1;
    uint32_t at = kList;
    put_half(rdram, at, 0); put_half(rdram, at + 2, 0);       // the empty cell's list
    const uint32_t empty = 0;
    at += 4;
    for (uint32_t row = 0; row < g_rows; ++row) {
        for (uint32_t col = 0; col < (1u << g_columns_log); ++col) {
            const uint32_t cell = col | (row << g_columns_log);
            if (kGrid + (cell + 1) * 2 > kGridEnd) continue;
            const float cx0 = float(int32_t(g_origin_x) + int32_t(col << kShift)), cz0 = float(int32_t(g_origin_z) + int32_t(row << kShift));
            const float cx1 = cx0 + float(1u << kShift), cz1 = cz0 + float(1u << kShift);
            const uint32_t start = at;
            bool any = false;
            for (uint32_t i = 0; i < planes; ++i) {
                const auto& bx = boxes[i];
                if (bx[1] < cx0 || bx[0] > cx1 || bx[3] < cz0 || bx[2] > cz1) continue;
                if (!any) { put_half(rdram, at, 0); at += 2; any = true; }
                if (at + 4 > kListEnd) break;
                put_half(rdram, at, uint16_t(i + 1));
                at += 2;
            }
            if (any) { put_half(rdram, at, 0); at += 2; }
            put_half(rdram, kGrid + cell * 2, uint16_t(any ? (start - kList) / 2 : empty));
        }
    }
    std::fprintf(stderr, "[wr64-guest] collision: %u planes, %u by %u cells of %u, lists %u bytes\n", planes,
                 1u << g_columns_log, g_rows, 1u << kShift, at - kList);
}

// The game's collision pointers, at the pack's (each frame: the course's
// loader set them, and may again).
void point_collision(uint8_t* rdram) {
    put_half(rdram, 0x1C3B40, 0);                             // not transposed
    put_word(rdram, 0x1C3B44, g_origin_x);
    put_word(rdram, 0x1C3B4C, g_extent_x);
    put_word(rdram, 0x1C3B50, g_origin_z);
    put_word(rdram, 0x1C3B58, g_extent_z);
    put_word(rdram, 0x1C3B60, g_columns_log);
    put_word(rdram, 0x1C3B68, 0);
    put_word(rdram, 0x1C3B70, kShift);
    put_word(rdram, 0x1C3B78, kShift);
    put_word(rdram, 0x1C3B7C, 0x80000000u | kGrid);
    put_word(rdram, 0x1C3B80, 0x80000000u | kList);
    put_word(rdram, 0x1C3B84, 0x80000000u | kPlanes);
    put_word(rdram, kSlopePieceCount, 0);                     // the base course's ramps' slopes
    // Its walls: a grid of its own (D_801924F0-38, set by the course's
    // loader, func_80071E70) of upright walls as lines on the map, which
    // func_80051740 pushes the jet ski's hull points off and stops them
    // going into -- Dolphin Park's shore, standing in Great Bay's open
    // water (the user's marks 1 and 8, 2026-11-03: stopped dead in open
    // water). Its extents (D_801924FC, D_80192508) at 0: nothing is in it.
    put_word(rdram, 0x1924FC, 0);
    put_word(rdram, 0x192508, 0);
    put_word(rdram, 0x1BB120, 0);                             // its buoys and gates
    // Its animals (D_801CF060, 30 of 0xBC: the kind +0 and +4, which picks
    // how each is drawn, none for 0): Dolphin Park's dolphin and fish know
    // only its water, and swam through Great Bay's beach (the user,
    // 2026-11-03).
    for (uint32_t i = 0; i < 30; ++i) {
        put_word(rdram, 0x1CF060 + i * 0xBC, 0);
        put_word(rdram, 0x1CF060 + i * 0xBC + 4, 0);
    }
    for (uint32_t i = 0; i < 256; ++i) {                      // its floating things
        const uint32_t o = 0x1BB138 + i * 0x18;
        float x, y, z;
        std::memcpy(&x, rdram + o, 4); std::memcpy(&y, rdram + o + 4, 4); std::memcpy(&z, rdram + o + 8, 4);
        if (!(std::fabs(y) < 100.0f) || (x == 0.0f && z == 0.0f)) break;
        put_real(rdram, o, 90000.0f);
        put_real(rdram, o + 8, 90000.0f + float(i) * 100.0f);
    }
}

}  // namespace

// Chosen on the course select's GUEST COURSES page (src/game_menus.cpp) for
// the next free roam, or WR64_GUEST=greatbay for every one.
std::atomic<bool> g_chosen{ false };
bool wanted() {
    static const bool on = [] {
        const char* v = std::getenv("WR64_GUEST");
        return v != nullptr && std::strcmp(v, "greatbay") == 0;
    }();
    return on || g_chosen.load(std::memory_order_relaxed);
}

bool active(const uint8_t* rdram) {
    if (!wanted() || !g_loaded) return false;
    uint32_t course;
    std::memcpy(&course, rdram + kCourse, 4);
    return course == 0 && free_roam_active(rdram);
}

// Great Bay for the next free roam on Dolphin Park, or not (the course
// select's GUEST COURSES page, src/game_menus.cpp).
void choose(bool on) { g_chosen.store(on, std::memory_order_relaxed); }
bool chosen() { return g_chosen.load(std::memory_order_relaxed); }

// Once a frame of a race (patches/free_roam.cpp, the game's thread).
void frame(uint8_t* rdram, uint32_t state) {
    uint32_t course;
    std::memcpy(&course, rdram + kCourse, 4);
    const bool on = wanted() && course == 0 && free_roam_active(rdram) && state == 0x28;
    if (!on) {
        g_loaded = false;
        g_frames = 0;
        return;
    }
    if (!g_loaded) {
        if (!load_pack()) return;
        install(rdram);
        const Header& h = g_pack.header;
        people::load(g_pack.data, h.people, h.people_table, h.models, h.models_table, h.water_x0, h.water_z0, h.water_cell, h.water_w,
                     h.water_h, h.water_grid, h.scale);
        g_loaded = true;
    }
    point_collision(rdram);
    // Its people, posed and on with what they are doing (src/guest_people.cpp).
    {
        uint32_t slot;
        std::memcpy(&slot, rdram + 0x0D48DC, 4);
        const uint32_t rider = 0x192690 + slot * 0x1718;
        float x, z;
        std::memcpy(&x, rdram + rider + 0x44, 4);
        std::memcpy(&z, rdram + rider + 0x4C, 4);
        static float last_x = x, last_z = z;
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        const float speed = dt > 0.0f ? std::hypot(x - last_x, z - last_z) / dt : 0.0f;
        last_x = x; last_z = z;
        people::frame(rdram, std::chrono::duration<double>(now.time_since_epoch()).count(), dt, x, z, speed,
                      std::isfinite(x) && std::isfinite(z));
    }
    // WR64_GUEST_STOPS=1: where the jet ski is brought up short (from over
    // 300 a second to under a third of that within half a second), and what
    // was near it then.
    static const bool stops = std::getenv("WR64_GUEST_STOPS") != nullptr;
    if (stops) {
        uint32_t slot;
        std::memcpy(&slot, rdram + 0x0D48DC, 4);
        const uint32_t rider = 0x192690 + slot * 0x1718;
        float x, y, z;
        std::memcpy(&x, rdram + rider + 0x44, 4); std::memcpy(&y, rdram + rider + 0x48, 4); std::memcpy(&z, rdram + rider + 0x4C, 4);
        static float px = 0, pz = 0, history[16] = {};
        static int at = 0, quiet = 0;
        const float speed = std::hypot(x - px, z - pz) * 30.0f;
        px = x; pz = z;
        history[at++ % 16] = speed;
        if (at % 15 == 0) std::fprintf(stderr, "[wr64-guest] ride at %.0f %.0f %.0f\n", x, y, z);
        if (quiet > 0) --quiet;
        float before = 0;
        for (int k = 0; k < 8; ++k) before = std::max(before, history[(at + k) % 16]);
        if (quiet == 0 && before > 300.0f && before < 3000.0f && speed < before / 3.0f) {
            quiet = 60;
            std::fprintf(stderr, "[wr64-guest] stopped at %.0f %.0f %.0f (from %.0f to %.0f)\n", x, y, z, before, speed);
        }
    }
}

// The start (patches/free_roam.cpp places the jet ski there for the first
// frames of the ride, the jet ski at `now_x`, `now_z`): false once placed
// and ridden off, or off.
bool start(float now_x, float now_z, float& x, float& z, float& heading) {
    // (Counted here, as the placing goes: the game may call the frame hook
    // more often than it races a frame, and holds the jet ski at the line
    // through the ride's first seconds.) Moved off the start by the player,
    // it is let go: held there all the while, it couldn't get going (the
    // user, 2026-11-03). Put back on the line by the game, far off, it is
    // placed again.
    if (!g_loaded || g_frames > 240) return false;
    ++g_frames;
    const float moved = std::hypot(now_x - g_pack.header.start_x, now_z - g_pack.header.start_z);
    if (g_frames > 1 && moved > 3.0f && moved < 1000.0f) {
        g_frames = 241;
        return false;
    }
    x = g_pack.header.start_x;
    z = g_pack.header.start_z;
    heading = g_pack.header.start_heading;
    // WR64_GUEST_START=<x>,<z>,<degrees>, in a scripted run only: elsewhere.
    static float test[3];
    static const bool testing = std::getenv("WR64_GUEST_START") != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr &&
                                std::sscanf(std::getenv("WR64_GUEST_START"), "%f,%f,%f", &test[0], &test[1], &test[2]) == 3;
    if (testing) { x = test[0]; z = test[1]; heading = test[2]; }
    return true;
}

// How far (x, z) is past where the guest course may be roamed (Wave Race
// units; 0 or less inside), for the Free Roam current.
float past_bounds(float x, float z) {
    if (!g_loaded) return -1.0f;
    const Header& h = g_pack.header;
    const float dx = std::max(h.bounds_x0 - x, x - h.bounds_x1), dz = std::max(h.bounds_z0 - z, z - h.bounds_z1);
    return std::max(dx, dz);
}

// Whether a pack was ever copied into the seafloor's vertex area.
bool took_vertex_area() { return g_took; }

// The scenery's list and the matrix it is drawn under (KSEG0), or 0.
uint32_t scenery_list() { return g_loaded ? 0x80000000u | (kBase + g_pack.header.list) : 0; }
uint32_t scenery_matrix() { return g_loaded ? 0x80000000u | (kBase + g_pack.header.matrix) : 0; }

}  // namespace guest
}  // namespace wr64

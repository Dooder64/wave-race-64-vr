// Sand seafloors under Dolphin Park and Sunny Beach, and the water's fog
// round an eye below the waves.
//
// The game draws nothing under its water but a flat textured floor at the
// water's own level, so the water looks bottomless everywhere and a view
// from below the surface has nothing to see. Dolphin Park (course 0) is
// given a floor of its own instead: the course's sand, shallow beside the
// palm island and the piers in the middle, deepening quickly away from them
// (some 4 m a hundred and fifty metres out) to about 10 m by the buoy
// barrier (a ring of radius 7700 about the origin), then on past it, sloping
// gently down to some 30 m at three times the barrier's radius, and there
// falling away steeply into deep water. It is built once when the course is
// loaded, in world space, and drawn in place of the game's flat floor, before
// the water, so the water's depth tint (RT64's WaterPS: the deeper what lies
// under the surface, the more of the water's own colour) shades it from above.
// Only the part round the camera is drawn each frame, as far as the waves
// reach.
//
// The island and the piers were measured from the course itself: the island
// from where its collision meets the waterline (WR64_SHORE_DUMP), the piers
// from the bottoms of their pillars (WR64_3D_TRACE_VERTS).
//
// The swim back after a fall (src/first_person.cpp) goes round what stands
// out of the water here: the two ramps, the three rocks of the stone arches,
// the piers' pillars and the stone wall by them, and the island's cliff and
// the pillar beside it, measured the same way. (The island's collision
// slopes gently up where its cliff rises: walked on, it led through it.)
//
// Sunny Beach (course 1) is a long sandbar with the mainland's beach a
// channel's width (some 1900 units) away on one side, and the open sea, past
// a straight row of buoys, on the other. The game draws its own floor there,
// flat and only 50 units down; its pieces are moved far out of sight
// (hide_game_floor), and the course is given a floor like Dolphin Park's
// instead, in the game's own seabed sand: just under the game's beaches,
// which carry on under the water to 50 units down some 400 units out, then
// dropping steeply into the channel (some 10 m in its middle), and on the
// seaward side sloping as Dolphin Park's does, on past the buoys to three
// times their distance from the shore, and there falling away. Its shores
// are where the beaches' triangles cross the water's level (measured with
// WR64_3D_TRACE_VERTS and WR64_3D_TRACE_RAW), each edge turned to have the
// land on its left; the swim back walks on their sand, and goes round the
// mainland's hills.
//
// Memory. Above the game's 4 MB, below the rewriter's scratch list at
// 0x700000: the floor's vertices at 0x400000, its lists at 0x500000, the fog
// at 0x5C0000. The cartridge never reads osMemSize, so nothing of the game's
// is there; build() still checks the floor's area is empty, or its own,
// before it writes.
//
// WR64_SEAFLOOR=0 leaves the game's floor alone.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wr64 {

// patches/water_all_round.cpp: the camera the frame's waves were merged round.
bool water_built_for(uint32_t segment3, float eye[3], float* centre_x, float* centre_z, float* radius);

namespace {

// The marker and the identity matrix; the cells' triangles, the same for
// every chunk; the drawing state; then for each frame buffer the list
// drawing the chunks round the camera. The vertices, a chunk after another.
constexpr uint32_t kBase = 0x500000;
constexpr uint32_t kTriangles = kBase + 0x100;
constexpr uint32_t kState = kBase + 0x400;
constexpr uint32_t kFrameLists = kBase + 0x1000, kFrameListSize = 0x8000;
constexpr uint32_t kBaseEnd = kFrameLists + 2 * kFrameListSize;
constexpr uint32_t kVertices = 0x400000;
constexpr uint32_t kVerticesEnd = 0x500000;
constexpr uint32_t kMarker = 0x53454146u;       // "SEAF", at kBase
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };

// The grid: chunks of 1024 units, each 4 cells of 256 a side, so a chunk's
// 25 vertices are one load and its texture coordinates start again at 0.
constexpr int kChunk = 1024, kCells = 4;
constexpr float kCell = float(kChunk) / kCells;
constexpr uint32_t kPerChunk = (kCells + 1) * (kCells + 1);
// The sand repeats every 128 units: 32 texels, 10.5 fixed point.
constexpr float kTexelsPerUnit = 32.0f / 128.0f;
// Chunks this near the camera are drawn: past the waves' flat ring (6000).
constexpr float kDrawRadius = 7000.0f;

// The barrier; the shelf beyond it, sloping down at about the floor's
// average slope inside it, to three times its radius; and there the drop.
constexpr float kBarrier = 7700.0f;
constexpr float kShelfSlope = 0.045f, kShelfEnd = 3.0f * kBarrier;

struct Anchor { float x, z, radius; };
// The palm island, and the piers' pillars (to 100 units).
constexpr Anchor kShore[] = {
    { -200, 5300, 320 },
    { -1500, 1600, 60 }, { -1500, 1800, 60 }, { -1200, 1600, 60 }, { -1200, 1800, 60 },
    { -1200, 2300, 60 }, { -1100, 700, 60 }, { -1100, 1950, 60 }, { -1000, 1100, 60 },
    { -1000, 2300, 60 }, { -700, 350, 60 }, { -700, 1100, 60 }, { -600, 0, 60 },
    { -600, 250, 60 }, { -400, -100, 60 }, { -400, 300, 60 }, { -300, 0, 60 },
    { -300, 250, 60 }, { 200, 350, 60 }, { 400, 100, 60 }, { 500, -200, 60 },
    { 500, 100, 60 }, { 700, -200, 60 }, { 900, -200, 60 },
};

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The shelf past a boundary `boundary` (a radius, or a distance from the
// shore): sloping down at kShelfSlope, eased in over 2000 units about the
// boundary so the floor does not kink there, to `end`; and there the drop,
// steep and then on down for good. `at` is the same kind of measure.
float shelf_and_drop(float at, float boundary, float end) {
    constexpr float kEase = 2000.0f;
    const float past = std::clamp(at - (boundary - kEase / 2.0f), 0.0f, kEase);
    return kShelfSlope * (past * past / (2.0f * kEase) + std::max(0.0f, at - (boundary + kEase / 2.0f))) +
           5200.0f * smoothstep(end + 150.0f, end + 1600.0f, at) + std::max(0.0f, at - (end + 1600.0f)) * 0.6f;
}

// Broad, low swells in the sand. Far longer than the grid's cells: shorter
// ripples sampled at 256 units showed as stripes.
float swells(float x, float z) {
    return 8.0f * std::sin(x * 0.0021f + z * 0.0008f) * std::sin(z * 0.0017f - x * 0.0006f);
}

// How far below the water Dolphin Park's floor lies at (x, z), in game units.
float dolphin_depth(float x, float z) {
    float shore = 1e9f;
    for (const Anchor& a : kShore) shore = std::min(shore, std::hypot(x - a.x, z - a.z) - a.radius);
    shore = std::max(shore, 0.0f);
    const float r = std::hypot(x, z);
    // Shallow by the shore, deepening quickly at first and levelling off at
    // about 10 m: deep enough for ramps and stunts to land in.
    float depth = 25.0f + 315.0f * (1.0f - std::exp(-shore / 1100.0f));
    // Swells, flattening out towards the drop; the shelf and the drop past
    // the barrier.
    depth += swells(x, z) * (1.0f - smoothstep(kShelfEnd, kShelfEnd + 500.0f, r));
    return depth + shelf_and_drop(r, kBarrier, kShelfEnd);
}

// Sunny Beach's shores at the water's level, each edge a -> b with the land
// on its left: the sandbar's and the mainland's.
struct Edge { float ax, az, bx, bz; };
constexpr Edge kSandbar[] = {
    { -5893, -84, -5857, -120 }, { -5893, -48, -5893, -84 }, { -5893, 84, -5893, -48 },
    { -5857, -120, -5785, -192 }, { -5820, 157, -5893, 84 }, { -5785, -192, -5713, -228 },
    { -5785, 192, -5820, 157 }, { -5721, 224, -5785, 192 }, { -5713, -228, -5650, -260 },
    { -5650, -260, -5579, -278 }, { -5650, 260, -5721, 224 }, { -5598, 273, -5650, 260 },
    { -5579, -278, -5526, -291 }, { -5526, -291, -5383, -308 }, { -5526, 291, -5598, 273 },
    { -5458, 299, -5526, 291 }, { -5383, -308, -5315, -317 }, { -5315, -317, -5101, -335 },
    { -5315, 317, -5458, 299 }, { -5213, 326, -5315, 317 }, { -5101, -335, -5006, -343 },
    { -5006, -343, -3577, -271 }, { -4999, 343, -5213, 326 }, { -4429, 314, -4999, 343 },
    { -3577, -271, -2993, -243 }, { -3000, 243, -4429, 314 }, { -2993, -243, -1064, -386 },
    { -2393, 252, -3000, 243 }, { -1064, -386, -310, -443 }, { -856, 276, -2393, 252 },
    { -310, -443, 261, -371 }, { 107, 324, -856, 276 }, { 261, -371, 521, -341 },
    { 493, 343, 107, 324 }, { 521, -341, 1950, -199 }, { 1079, 371, 493, 343 },
    { 1950, -199, 2507, -143 }, { 2507, -143, 3056, -143 }, { 2507, 443, 1079, 371 },
    { 3056, -143, 4485, -143 }, { 3074, 414, 2507, 443 }, { 4485, -143, 5056, -268 },
    { 4502, 343, 3074, 414 }, { 4740, 335, 4502, 343 }, { 5056, -268, 5293, -320 },
    { 5293, -320, 5436, -302 }, { 5311, 318, 4740, 335 }, { 5383, 308, 5311, 318 },
    { 5436, -302, 5526, -291 }, { 5526, -291, 5598, -273 }, { 5526, 291, 5383, 308 },
    { 5579, 278, 5526, 291 }, { 5598, -273, 5650, -260 }, { 5650, -260, 5721, -224 },
    { 5650, 260, 5579, 278 }, { 5713, 228, 5650, 260 }, { 5721, -224, 5785, -192 },
    { 5785, -192, 5820, -157 }, { 5785, 192, 5713, 228 }, { 5820, -157, 5893, -84 },
    { 5857, 120, 5785, 192 }, { 5893, -84, 5893, 48 }, { 5893, 48, 5893, 84 },
    { 5893, 84, 5857, 120 },
};
constexpr Edge kMainland[] = {
    { -14874, 3632, -14417, 2992 }, { -14417, 2992, -13917, 2292 }, { -13917, 2292, -13447, 1978 },
    { -13447, 1978, -12847, 1578 }, { -12847, 1578, -11800, 1388 }, { -11800, 1388, -10700, 1188 },
    { -10700, 1188, -9984, 950 }, { -9984, 950, -9384, 750 }, { -9384, 750, -9096, 461 },
    { -9096, 461, -8796, 161 }, { -8796, 161, -8551, 100 }, { -8551, 100, -8151, 0 },
    { -8151, 0, -7928, 26 }, { -7928, 26, -7470, 104 }, { -7470, 104, -7388, 179 },
    { -7388, 179, -7146, 401 }, { -7146, 401, -7046, 601 }, { -7046, 601, -6806, 1083 },
    { -6806, 1083, -6706, 1283 }, { -6706, 1283, -6504, 1404 }, { -6504, 1404, -5930, 1748 },
    { -5930, 1748, -5728, 1870 }, { -5728, 1870, -5555, 1908 }, { -5555, 1908, -4889, 2048 },
    { -4889, 2048, -4530, 2128 }, { -4530, 2128, -3590, 2082 }, { -3590, 2082, -2654, 2035 },
    { -2654, 2035, -1827, 2106 }, { -1827, 2106, -975, 2181 }, { -975, 2181, -244, 2190 },
    { -244, 2190, 488, 2200 }, { 488, 2200, 1454, 2303 }, { 1454, 2303, 2397, 2403 },
    { 2397, 2403, 3371, 2294 }, { 3371, 2294, 4271, 2194 }, { 4271, 2194, 5124, 1982 },
    { 5124, 1982, 5924, 1782 }, { 5924, 1782, 6621, 1502 }, { 6621, 1502, 7121, 1302 },
    { 6622, -2327, 6776, -2584 }, { 6722, -1827, 6622, -2327 }, { 6774, -1568, 6722, -1827 },
    { 6776, -2584, 7076, -3084 }, { 7074, -1168, 6774, -1568 }, { 7076, -3084, 7582, -3300 },
    { 7121, 1302, 7300, 765 }, { 7300, 765, 7400, 465 }, { 7400, -734, 7074, -1168 },
    { 7400, -35, 7400, -734 }, { 7400, 465, 7400, -35 }, { 7582, -3300, 8282, -3600 },
    { 8282, -3600, 8835, -3600 }, { 8835, -3600, 9535, -3600 }, { 9535, -3600, 10284, -3350 },
    { 10284, -3350, 11184, -3050 }, { 11184, -3050, 11672, -2562 }, { 11672, -2562, 12172, -2062 },
    { 12172, -2062, 13018, -1700 }, { 13018, -1700, 13718, -1400 }, { 13718, -1400, 14700, -1400 },
    { 14700, -1400, 15600, -1400 },
};

// Where the beaches reach 50 units down, and their slope; how far the buoys
// lie from the sandbar's shore, and the shelf beyond them to three times as
// far; the sandbar's length, for where the channel is.
constexpr float kBeachReach = 400.0f, kSunnyBeachSlope = 50.0f / 400.0f;
constexpr float kSunnyBuoys = 1700.0f, kSunnyShelfEnd = 3.0f * kSunnyBuoys;
constexpr float kSandbarEnd = 5900.0f;
// How far the mainland's beach goes up from the water before its hills.
constexpr float kMainlandBeach = 500.0f;

// How far (x, z) is from Sunny Beach's nearest shore, positive in the
// water and negative on land; and whether that shore is the sandbar's.
struct Shore { float distance; bool sandbar; };
Shore sunny_shore(float x, float z) {
    Shore nearest{ 1e9f, false };
    float best = 1e18f;
    const auto look = [&](const Edge* edges, size_t count, bool sandbar) {
        for (size_t i = 0; i < count; ++i) {
            const Edge& e = edges[i];
            const float ex = e.bx - e.ax, ez = e.bz - e.az;
            const float t = std::clamp(((x - e.ax) * ex + (z - e.az) * ez) / std::max(ex * ex + ez * ez, 1e-3f), 0.0f, 1.0f);
            const float dx = x - e.ax - t * ex, dz = z - e.az - t * ez;
            const float squared = dx * dx + dz * dz;
            if (squared >= best) continue;
            best = squared;
            const bool land = ex * (z - e.az) - ez * (x - e.ax) > 0.0f;
            nearest = { land ? -std::sqrt(squared) : std::sqrt(squared), sandbar };
        }
    };
    look(kSandbar, sizeof(kSandbar) / sizeof(kSandbar[0]), true);
    look(kMainland, sizeof(kMainland) / sizeof(kMainland[0]), false);
    return nearest;
}

// How far below the water Sunny Beach's floor lies at (x, z).
float sunny_depth(float x, float z) {
    const float d = sunny_shore(x, z).distance;
    // Under the land, below anything the game draws there.
    if (d <= 0.0f) return 60.0f;
    // Just under the game's beaches as they go on under the water...
    float depth = 10.0f + kSunnyBeachSlope * std::min(d, kBeachReach);
    // ...and past them: steeply into the channel between the sandbar and
    // the mainland, or as Dolphin Park slopes on the seaward side, on past
    // the buoys to the drop.
    const float past = std::max(0.0f, d - kBeachReach);
    const float channel = 330.0f * (1.0f - std::exp(-past / 300.0f));
    const float sea = 300.0f * (1.0f - std::exp(-past / 1100.0f)) + shelf_and_drop(d, kSunnyBuoys, kSunnyShelfEnd);
    const float in_channel = smoothstep(150.0f, 600.0f, z) * (1.0f - smoothstep(kSandbarEnd, kSandbarEnd + 1600.0f, std::fabs(x)));
    depth += in_channel * channel + (1.0f - in_channel) * sea;
    return depth + swells(x, z) * smoothstep(kBeachReach, kBeachReach + 500.0f, d) *
                       (1.0f - smoothstep(kSunnyShelfEnd, kSunnyShelfEnd + 500.0f, d));
}

// A course's floor: where its grid lies (its corner and its chunks each
// way), its sand (a 32x32 RGBA16 block in one of the course's segments) and
// how it repeats (its render tile's settings: wrapped, or mirrored for a
// texture whose edges do not meet -- wrapped, Dolphin Park's lined up into
// stripes), and its depth.
constexpr uint32_t kWrapped = 0x00014050u, kMirrored = 0x00054150u;  // 32x32, each way
struct Floor {
    uint32_t course;
    float x0, z0;
    int across, down;
    int sand_segment;
    uint32_t sand, tile;
    float (*depth)(float x, float z);
};
constexpr Floor kFloors[] = {
    // The top 32 rows of Dolphin Park's 32x64 beach strip, whose lower rows
    // fade into water.
    { 0, -25600.0f, -25600.0f, 50, 50, 13, 0xA5B0, kMirrored, dolphin_depth },
    // The sand of Sunny Beach's sandbar top, wrapped as the game wraps it
    // there. (The game's own floor is painted the water's turquoise, being
    // meant to be seen through it.)
    { 1, -20480.0f, -12288.0f, 40, 20, 14, 0x11800, kWrapped, sunny_depth },
};

const Floor* floor_for(uint32_t course) {
    for (const Floor& f : kFloors) {
        if (f.course == course) return &f;
    }
    return nullptr;
}

// What the swimmer goes round, each as its outline at the waterline (a
// convex polygon in x, z, in order round it) or a circle.
struct Outline { int count; float x[12], z[12]; };
constexpr Outline kSolid[] = {
    // The ramps.
    { 4, { -1716, -1294, -568, -990 }, { 3915, 4821, 4483, 3577 } },
    { 4, { 1033, 1049, 1849, 1833 }, { 3126, 4126, 4112, 3112 } },
    // The rocks of the stone arches, north to south.
    { 7, { 3053, 3200, 3300, 3303, 3203, 3051, 2988 }, { 3053, 3034, 3134, 3303, 3403, 3376, 3185 } },
    { 4, { 3212, 3395, 3495, 3046 }, { 3798, 3924, 4224, 4060 } },
    { 5, { 3021, 3540, 3440, 3257, 3104 }, { 4542, 4800, 5100, 5100, 4930 } },
    // The stone wall by the piers.
    { 4, { -149, -70, -70, -149 }, { -963, -963, -144, -144 } },
    // The island's cliff, from the beach (y 20) up to its top (y 130), at
    // its foot; and the pillar beside it, holding up the platform above.
    { 10, { 300, 214, 96, -14, -103, -125, -34, 116, 270, 325 },
          { 5350, 5399, 5403, 5352, 5254, 5125, 4987, 4957, 5014, 5169 } },
    { 4, { -172, -90, -90, -172 }, { 5106, 5106, 5295, 5295 } },
};
constexpr float kPillar = 55.0f;
// The island's cliff (in kSolid), and the beach drawn round it: sand going
// on down from the cliff's foot, 20 units up, as far as 60 units under the
// water some 620 units out -- well past where the course's collision ends.
constexpr int kCliff = 6;
constexpr float kCliffFoot = 20.0f, kBeachSlope = 80.0f / 620.0f;

// How far (x, z) is outside `o`'s outline: negative inside.
float outside(const Outline& o, float x, float z) {
    float edge = 1e9f;
    bool left = true, right = true;
    for (int i = 0; i < o.count; ++i) {
        const int j = (i + 1) % o.count;
        const float ex = o.x[j] - o.x[i], ez = o.z[j] - o.z[i];
        const float side = ex * (z - o.z[i]) - ez * (x - o.x[i]);
        left = left && side >= 0.0f;
        right = right && side <= 0.0f;
        const float t = std::clamp(((x - o.x[i]) * ex + (z - o.z[i]) * ez) / std::max(ex * ex + ez * ez, 1e-3f), 0.0f, 1.0f);
        edge = std::min(edge, std::hypot(x - o.x[i] - t * ex, z - o.z[i] - t * ez));
    }
    // Inside: on the same side of every edge, whichever way round it runs.
    return left || right ? -edge : edge;
}

void put_half(uint8_t* rdram, uint32_t address, float value) {
    *reinterpret_cast<int16_t*>(rdram + (address ^ 2)) = int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
}

struct Builder {
    uint8_t* rdram;
    uint32_t at, end;
    bool full = false;
    void command(uint32_t w0, uint32_t w1) {
        if (at + 8 > end) { full = true; return; }
        reinterpret_cast<uint32_t*>(rdram + at)[0] = w0;
        reinterpret_cast<uint32_t*>(rdram + at)[1] = w1;
        at += 8;
    }
};

// Writes `floor`, its sand at physical `sand`; returns whether it fits and
// the area was free.
bool build(uint8_t* rdram, const Floor& floor, uint32_t sand) {
    uint32_t* marker = reinterpret_cast<uint32_t*>(rdram + kBase);
    if (*marker != kMarker) {
        for (const auto& [from, to] : { std::pair{ kBase, kBaseEnd }, std::pair{ kVertices, kVerticesEnd } }) {
            for (uint32_t a = from; a < to; a += 4) {
                if (*reinterpret_cast<uint32_t*>(rdram + a) != 0) {
                    std::fprintf(stderr, "[seafloor] memory at 0x%06X is in use; no seafloor\n", a);
                    return false;
                }
            }
        }
        *marker = kMarker;
    }
    // The identity matrix it is drawn under, after the marker.
    for (int i = 0; i < 16; ++i) {
        put_half(rdram, kBase + 0x40 + uint32_t(i) * 2, (i % 5) == 0 ? 1.0f : 0.0f);
        put_half(rdram, kBase + 0x60 + uint32_t(i) * 2, 0.0f);
    }

    // A chunk's cells, two triangles each, called after its vertices' load.
    Builder triangles{ rdram, kTriangles, kState };
    for (int j = 0; j < kCells; ++j) {
        for (int i = 0; i < kCells; ++i) {
            const uint32_t a = uint32_t(j * (kCells + 1) + i), b = a + 1;
            const uint32_t c = a + (kCells + 1), d = c + 1;
            triangles.command(0xBFu << 24, ((a * 5) << 16) | ((c * 5) << 8) | (d * 5));
            triangles.command(0xBFu << 24, ((a * 5) << 16) | ((d * 5) << 8) | (b * 5));
        }
    }
    triangles.command(0xB8000000u, 0);

    Builder list{ rdram, kState, kFrameLists };
    // Its state: one cycle, the sand filtered and in perspective, opaque,
    // tested against depth and writing it, texture times shade, unlit,
    // unfogged and not culled.
    list.command(0xE7000000u, 0);                          // pipe sync
    list.command(0xBA001402u, 0x00000000u);                // one cycle
    list.command(0xBA000C02u, 0x00002000u);                // bilinear
    list.command(0xBA001301u, 0x00080000u);                // perspective
    list.command(0xBA000903u, 0x00000C00u);                // convert: filter
    list.command(0xBA001001u, 0x00000000u);                // LOD: tile
    list.command(0xBA000E02u, 0x00000000u);                // no palette
    list.command(0xBA001102u, 0x00000000u);                // detail: clamp
    list.command(0xB9000002u, 0x00000000u);                // no alpha compare
    list.command(0xB9000201u, 0x00000000u);                // depth from the pixel
    list.command(0xB900031Du, 0x00552078u);                // opaque surface, depth tested and written
    list.command(0xFC121824u, 0xFF33FFFFu);                // texture times shade
    list.command(0xB6000000u, 0x000F3001u);                // clear depth, cull, fog, lighting, texgen
    list.command(0xB7000000u, 0x00000205u);                // set depth, shade, smooth
    list.command(0xBB000001u, 0xFFFFFFFFu);                // texture on, full scale
    list.command(0xFD100000u, sand);                       // 32x32 RGBA16 sand, loaded as a block
    list.command(0xF5100000u, 0x07014050u);
    list.command(0xE6000000u, 0);
    list.command(0xF3000000u, 0x073FF100u);
    list.command(0xE7000000u, 0);
    list.command(0xF5101000u, floor.tile);                 // render tile: 32x32, repeating
    list.command(0xF2000000u, 0x0007C07Cu);
    list.command(0xB8000000u, 0);
    if (list.full || triangles.full) return false;

    // The sun, high and off to one side, gives the ripples and the drop
    // some relief.
    const float lx = 0.3f, ly = 0.9f, lz = 0.3f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    uint32_t vertex = kVertices;
    for (int cz = 0; cz < floor.down; ++cz) {
        for (int cx = 0; cx < floor.across; ++cx) {
            if (vertex + kPerChunk * 16 > kVerticesEnd) return false;
            const float x0 = floor.x0 + float(cx * kChunk), z0 = floor.z0 + float(cz * kChunk);
            for (int j = 0; j <= kCells; ++j) {
                for (int i = 0; i <= kCells; ++i) {
                    const float x = x0 + float(i) * kCell, z = z0 + float(j) * kCell;
                    const float y = -floor.depth(x, z);
                    // The normal from the slope either way.
                    const float e = 40.0f;
                    const float dx = (floor.depth(x + e, z) - floor.depth(x - e, z)) / (2 * e);
                    const float dz = (floor.depth(x, z + e) - floor.depth(x, z - e)) / (2 * e);
                    const float nl = 1.0f / std::sqrt(dx * dx + 1.0f + dz * dz);
                    const float light = std::max(0.0f, (dx * lx + ly + dz * lz) * nl / ll);
                    const float shade = std::clamp(0.45f + 0.55f * light, 0.0f, 1.0f) * 235.0f;
                    const uint32_t v = vertex + uint32_t(j * (kCells + 1) + i) * 16;
                    put_half(rdram, v + 0, x);
                    put_half(rdram, v + 2, y);
                    put_half(rdram, v + 4, z);
                    put_half(rdram, v + 6, 0.0f);
                    put_half(rdram, v + 8, float(i) * kCell * kTexelsPerUnit * 32.0f);
                    put_half(rdram, v + 10, float(j) * kCell * kTexelsPerUnit * 32.0f);
                    const uint32_t c = uint32_t(shade);
                    *reinterpret_cast<uint32_t*>(rdram + v + 12) = (c << 24) | (c << 16) | (c << 8) | 0xFFu;
                }
            }
            vertex += kPerChunk * 16;
        }
    }
    std::fprintf(stderr, "[seafloor] course %u built: %u vertices, %d chunks of %d units\n", floor.course,
                 (vertex - kVertices) / 16, floor.across * floor.down, kChunk);
    return true;
}

// Writes frame buffer `b`'s list drawing `floor`'s chunks within
// kDrawRadius of (eye_x, eye_z); returns its address.
uint32_t frame_list(uint8_t* rdram, const Floor& floor, int b, float eye_x, float eye_z) {
    Builder list{ rdram, kFrameLists + uint32_t(b) * kFrameListSize, kFrameLists + uint32_t(b + 1) * kFrameListSize - 8 };
    list.command(0x06000000u, 0x80000000u | kState);
    const auto first = [](float at, float origin, int chunks) {
        return std::clamp(int(std::floor((at - origin) / kChunk)), 0, chunks - 1);
    };
    for (int cz = first(eye_z - kDrawRadius, floor.z0, floor.down); cz <= first(eye_z + kDrawRadius, floor.z0, floor.down); ++cz) {
        for (int cx = first(eye_x - kDrawRadius, floor.x0, floor.across); cx <= first(eye_x + kDrawRadius, floor.x0, floor.across); ++cx) {
            const float x0 = floor.x0 + float(cx * kChunk), z0 = floor.z0 + float(cz * kChunk);
            const float dx = std::max({ x0 - eye_x, 0.0f, eye_x - (x0 + kChunk) });
            const float dz = std::max({ z0 - eye_z, 0.0f, eye_z - (z0 + kChunk) });
            if (dx * dx + dz * dz > kDrawRadius * kDrawRadius) continue;
            const uint32_t vertex = kVertices + uint32_t(cz * floor.across + cx) * kPerChunk * 16;
            list.command((0x04u << 24) | (kPerChunk << 9) | (kPerChunk * 16 - 1), 0x80000000u | vertex);
            list.command(0x06000000u, 0x80000000u | kTriangles);
        }
    }
    list.full = false;
    list.end += 8;
    list.command(0xB8000000u, 0);
    return 0x80000000u | (kFrameLists + uint32_t(b) * kFrameListSize);
}

// Sunny Beach's own floor, flat and 50 units down, moved far below sight:
// the list drawing the course's scenery (segment 13) is walked, and each
// vertex load made while the floor's two textures are set has its vertices
// at that depth moved down. Once moved they are passed over, so it is cheap
// to do every frame, as it must be: the course's data is loaded afresh for
// each race.
constexpr uint32_t kSunnyScenery = 0x0D0154D0;
constexpr uint32_t kSunnyFloorTextures[] = { 0x0D000000, 0x0D000800 };
constexpr int16_t kGameFloor = -50, kOutOfSight = -30000;

void hide_game_floor(uint8_t* rdram, uint32_t segment13, uint32_t list, int depth, uint32_t& texture) {
    if (depth > 6 || (list >> 24) != 0x0D) return;
    uint32_t at = (segment13 + (list & 0xFFFFFF)) & 0x7FFFFF;
    for (int i = 0; i < 4096; ++i, at += 8) {
        const uint32_t w0 = *reinterpret_cast<const uint32_t*>(rdram + at);
        const uint32_t w1 = *reinterpret_cast<const uint32_t*>(rdram + at + 4);
        const uint8_t op = uint8_t(w0 >> 24);
        if (op == 0xB8) return;
        if (op == 0xFD) texture = w1;
        if (op == 0x06) {
            hide_game_floor(rdram, segment13, w1, depth + 1, texture);
            if (((w0 >> 16) & 0xFF) == 1) return;             // a branch: no return
        }
        if (op == 0x04 && (w1 >> 24) == 0x0D &&
            std::find(std::begin(kSunnyFloorTextures), std::end(kSunnyFloorTextures), texture) != std::end(kSunnyFloorTextures)) {
            const uint32_t count = (w0 >> 9) & 0x7F;
            const uint32_t vertices = (segment13 + (w1 & 0xFFFFFF)) & 0x7FFFFF;
            for (uint32_t v = 0; v < count; ++v) {
                int16_t* y = reinterpret_cast<int16_t*>(rdram + ((vertices + v * 16 + 2) ^ 2));
                if (*y == kGameFloor) *y = kOutOfSight;
            }
        }
    }
}

bool seafloor_off() {
    static const bool off = [] {
        const char* value = std::getenv("WR64_SEAFLOOR");
        return value != nullptr && value[0] == '0';
    }();
    return off;
}

}  // namespace

// The seafloor's height at (x, z) on course `course`, in *height -- or a
// beach, where that is higher; false for a course without one (or with
// WR64_SEAFLOOR=0).
bool seafloor_height_at(uint32_t course, float x, float z, float* height) {
    const Floor* floor = floor_for(course);
    if (seafloor_off() || floor == nullptr) return false;
    if (course == 0) {
        const float beach = kCliffFoot - kBeachSlope * std::max(0.0f, outside(kSolid[kCliff], x, z));
        *height = std::max(-dolphin_depth(x, z), beach);
        return true;
    }
    // Sunny Beach: the beaches' sand, rising from the water at their slope
    // to the sandbar's top or the mainland's, and going on under the water
    // as far as they reach; past them the floor.
    const Shore shore = sunny_shore(x, z);
    if (shore.distance <= 0.0f) {
        *height = std::min(-shore.distance * kSunnyBeachSlope, shore.sandbar ? 20.0f : 50.0f);
    } else {
        *height = -sunny_depth(x, z);
        if (shore.distance < kBeachReach) *height = std::max(*height, -shore.distance * kSunnyBeachSlope);
    }
    return true;
}

// How far (x, z) is from anything solid standing out of the water on course
// `course` (negative inside it), at the waterline: for the swim back to go
// round. Very far on a course without any.
float course_clearance(uint32_t course, float x, float z) {
    float nearest = 1e9f;
    if (course == 1) {
        // Sunny Beach: the mainland past its beach, where its hills begin.
        // The sandbar is sand all through, walked over rather than gone
        // round -- too long to go round in any case.
        const Shore shore = sunny_shore(x, z);
        return shore.sandbar ? nearest : shore.distance + kMainlandBeach;
    }
    if (course != 0) return nearest;
    for (const Outline& o : kSolid) nearest = std::min(nearest, outside(o, x, z));
    for (size_t i = 1; i < sizeof(kShore) / sizeof(kShore[0]); ++i) {
        nearest = std::min(nearest, std::hypot(x - kShore[i].x, z - kShore[i].z) - kPillar);
    }
    return nearest;
}

// The list drawing the seafloor round the frame's camera for the course
// loaded, as an address to call, or 0 for a course without one; `identity`
// is given an identity matrix to draw it under. `course` is the game's course
// ID and `segments` the frame's (13 and 14 the course's data, 3 the
// frame's buffer). Without waves merged round the camera, the floor round the
// course's middle.
uint32_t seafloor_list(uint8_t* rdram, uint32_t course, const uint32_t segments[16], uint32_t* identity) {
    *identity = 0x80000000u | (kBase + 0x40);
    const Floor* floor = floor_for(course);
    const uint32_t segment13 = segments[13];
    if (seafloor_off() || floor == nullptr || segment13 == 0 || segments[floor->sand_segment] == 0) return 0;
    const uint32_t sand = segments[floor->sand_segment] + floor->sand;
    static uint32_t built_course = ~0u, built_sand = 0;
    static bool ok = false;
    if (built_course != course || built_sand != sand) {
        built_course = course;
        built_sand = sand;
        ok = build(rdram, *floor, sand);
    }
    if (!ok) return 0;
    // WR64_SEAFLOOR_TEXDUMP=<prefix>: the course's candidate sands, once, as
    // raw 8-bit RGBA (<prefix>_<segment>_<offset>_<width>x<height>.rgba).
    static const char* dump = std::getenv("WR64_SEAFLOOR_TEXDUMP");
    static uint32_t dumped = ~0u;
    if (dump != nullptr && dumped != course) {
        dumped = course;
        struct Candidate { int segment; uint32_t offset; int width, height; };
        for (const Candidate c : { Candidate{ 13, 0x0000, 32, 32 }, Candidate{ 13, 0x0800, 32, 32 },
                                   Candidate{ 13, 0xA5B0, 32, 64 }, Candidate{ 14, 0x10800, 32, 64 },
                                   Candidate{ 14, 0x11800, 32, 32 }, Candidate{ 14, 0x12000, 32, 32 } }) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s_%d_%05X_%dx%d.rgba", dump, c.segment, c.offset, c.width, c.height);
            if (FILE* f = std::fopen(path, "wb")) {
                const uint32_t base = (segments[c.segment] + c.offset) & 0x7FFFFF;
                for (int i = 0; i < c.width * c.height; ++i) {
                    const uint16_t p = *reinterpret_cast<const uint16_t*>(rdram + ((base + uint32_t(i) * 2) ^ 2));
                    const uint8_t rgba[4] = { uint8_t((p >> 11) << 3), uint8_t(((p >> 6) & 31) << 3), uint8_t(((p >> 1) & 31) << 3),
                                              uint8_t((p & 1) ? 255 : 0) };
                    std::fwrite(rgba, 1, 4, f);
                }
                std::fclose(f);
            }
        }
    }
    if (course == 1) {
        uint32_t texture = 0;
        hide_game_floor(rdram, segment13, kSunnyScenery, 0, texture);
    }
    const int b = segments[3] == kFrameBuffers[1] ? 1 : 0;
    float eye[3] = { 0.0f, 0.0f, 0.0f }, centre_x, centre_z, radius;
    if (!water_built_for(segments[3], eye, &centre_x, &centre_z, &radius)) eye[0] = eye[2] = 0.0f;
    return frame_list(rdram, *floor, b, eye[0], eye[2]);
}

}  // namespace wr64

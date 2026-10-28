// Seafloors under Dolphin Park, Sunny Beach, Sunset Bay (sand), Drake Lake
// (mud), Marine Fortress (rock), Port Blue (silt), Southern Island (sand) and
// Glacier Coast (snowy rock), and the water's fog round an eye below the
// waves. Sunset Bay and Drake Lake are floored the
// same way as Sunny Beach, from shores measured the same way; Marine Fortress
// and Port Blue by their walls; see their depths below.
//
// The game draws nothing under its water but a flat textured floor at the
// water's own level, so the water looks bottomless everywhere and a view
// from below the surface has nothing to see. Dolphin Park (course 0) is
// given a floor of its own instead: the course's sand, shallow beside the
// palm island and the piers in the middle, deepening quickly away from them
// (some 4 m a hundred and fifty metres out) to about 10 m by the buoy
// barrier (a ring of radius 7700 about the origin), then on past it, sloping
// gently down towards 15 m -- the deepest any floor here goes, anywhere, by
// rule: deep water underfoot, let alone an abyss, unsettled more than it
// thrilled (kMaxDepth). It is built once when the course is
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
// seaward side sloping as Dolphin Park's does, on past the buoys towards
// 15 m. Its shores
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
// every chunk; the drawing state; a sand made here, for a course without one
// of its own; then for each frame buffer the list drawing the chunks round
// the camera. The vertices, a chunk after another.
constexpr uint32_t kBase = 0x500000;
constexpr uint32_t kTriangles = kBase + 0x100;
constexpr uint32_t kState = kBase + 0x400;
constexpr uint32_t kMadeSand = kBase + 0x800;
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
// average slope inside it. The deepest any floor goes: 15 m.
constexpr float kBarrier = 7700.0f;
constexpr float kShelfSlope = 0.045f, kShelfEnd = 3.0f * kBarrier;
constexpr float kMaxDepth = 500.0f;

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
// boundary so the floor does not kink there. `at` is the same kind of
// measure. (It once ended in a drop to an abyss; no floor goes below
// kMaxDepth now.)
float shelf(float at, float boundary) {
    constexpr float kEase = 2000.0f;
    const float past = std::clamp(at - (boundary - kEase / 2.0f), 0.0f, kEase);
    return kShelfSlope * (past * past / (2.0f * kEase) + std::max(0.0f, at - (boundary + kEase / 2.0f)));
}

// A depth held to kMaxDepth, eased into it over the last 100 units so the
// floor levels off without a crease.
float capped(float depth) {
    constexpr float kEase = 100.0f;
    return depth <= kMaxDepth - kEase ? depth : kMaxDepth - kEase + kEase * std::tanh((depth - (kMaxDepth - kEase)) / kEase);
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
    // Swells, flattening out far out; the shelf past the barrier.
    depth += swells(x, z) * (1.0f - smoothstep(kShelfEnd, kShelfEnd + 500.0f, r));
    return depth + shelf(r, kBarrier);
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
// How far the mainland's beach goes up from the water before its hills;
// how far elsewhere the land is walked on before it counts as solid.
constexpr float kMainlandBeach = 500.0f, kLandEdge = 100.0f, kDrakeLandEdge = 600.0f;

// Whether one of `count` shore edges is nearer (x, z) than `best` (squared,
// 1e18 to begin with); if so `best` and `distance` become its -- positive in
// the water, negative on land. Nearest to an edge's middle, the side is the
// edge's; nearest to a corner, where edges meet, it is judged by all of them
// (the sum of their normals towards the water): off the tip of a narrow
// islet one edge's side alone put patches of open water on land.
bool nearer_shore(const Edge* edges, size_t count, float x, float z, float& best, float& distance) {
    bool nearer = false, corner = false;
    float px = 0.0f, pz = 0.0f, side = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const Edge& e = edges[i];
        const float ex = e.bx - e.ax, ez = e.bz - e.az;
        const float raw = ((x - e.ax) * ex + (z - e.az) * ez) / std::max(ex * ex + ez * ez, 1e-3f);
        const float t = std::clamp(raw, 0.0f, 1.0f);
        const float dx = x - e.ax - t * ex, dz = z - e.az - t * ez;
        const float squared = dx * dx + dz * dz;
        if (squared >= best) continue;
        best = squared;
        nearer = true;
        corner = raw <= 0.0f || raw >= 1.0f;
        px = e.ax + t * ex;
        pz = e.az + t * ez;
        side = ex * (z - e.az) - ez * (x - e.ax);
    }
    if (!nearer) return false;
    if (corner) {
        float nx = 0.0f, nz = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            const Edge& e = edges[i];
            const bool touches = (std::fabs(e.ax - px) < 2.0f && std::fabs(e.az - pz) < 2.0f) ||
                                 (std::fabs(e.bx - px) < 2.0f && std::fabs(e.bz - pz) < 2.0f);
            if (!touches) continue;
            const float ex = e.bx - e.ax, ez = e.bz - e.az, length = std::max(std::hypot(ex, ez), 1e-3f);
            nx += ez / length;
            nz -= ex / length;
        }
        if (nx != 0.0f || nz != 0.0f) side = -((x - px) * nx + (z - pz) * nz);
    }
    distance = side > 0.0f ? -std::sqrt(best) : std::sqrt(best);
    return true;
}

// How far (x, z) is from Sunny Beach's nearest shore, positive in the
// water and negative on land; and whether that shore is the sandbar's.
struct Shore { float distance; bool sandbar; };
Shore sunny_shore(float x, float z) {
    Shore nearest{ 1e9f, false };
    float best = 1e18f;
    nearest.sandbar = nearer_shore(kSandbar, sizeof(kSandbar) / sizeof(kSandbar[0]), x, z, best, nearest.distance);
    if (nearer_shore(kMainland, sizeof(kMainland) / sizeof(kMainland[0]), x, z, best, nearest.distance)) nearest.sandbar = false;
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
    // the buoys.
    const float past = std::max(0.0f, d - kBeachReach);
    const float channel = 330.0f * (1.0f - std::exp(-past / 300.0f));
    const float sea = 300.0f * (1.0f - std::exp(-past / 1100.0f)) + shelf(d, kSunnyBuoys);
    const float in_channel = smoothstep(150.0f, 600.0f, z) * (1.0f - smoothstep(kSandbarEnd, kSandbarEnd + 1600.0f, std::fabs(x)));
    depth += in_channel * channel + (1.0f - in_channel) * sea;
    return depth + swells(x, z) * smoothstep(kBeachReach, kBeachReach + 500.0f, d) *
                       (1.0f - smoothstep(kSunnyShelfEnd, kSunnyShelfEnd + 500.0f, d));
}

// Sunset Bay (course 2): a long island with the town on it and a pier out
// to an islet at either side, the open sea all round, past a row of buoys
// some 1600 units off. Its banks are steep, 60 units down within 30 of the
// water's edge; past them the floor slopes as Dolphin Park's does, on past
// the buoys.
constexpr Edge kSunsetShore[] = {
    { -3798, -2658, -3758, -2758 }, { -3798, -2518, -3798, -2658 }, { -3798, -2308, -3798, -2518 },
    { -3798, 542, -3758, 442 }, { -3798, 682, -3798, 542 }, { -3798, 892, -3798, 682 },
    { -3780, -2248, -3798, -2308 }, { -3780, 952, -3798, 892 }, { -3760, -2188, -3780, -2248 },
    { -3760, 1012, -3780, 952 }, { -3758, -2758, -3725, -2833 }, { -3758, 442, -3725, 367 },
    { -3725, -2833, -3685, -2733 }, { -3725, 367, -3685, 467 }, { -3720, -2188, -3760, -2188 },
    { -3720, 1012, -3760, 1012 }, { -3690, -2188, -3720, -2188 }, { -3690, 1012, -3720, 1012 },
    { -3685, -2733, -3652, -2658 }, { -3685, 467, -3652, 542 }, { -3672, -2248, -3690, -2188 },
    { -3672, 952, -3690, 1012 }, { -3652, -2658, -3652, -2448 }, { -3652, -2448, -3652, -2308 },
    { -3652, -2308, -3672, -2248 }, { -3652, 542, -3652, 752 }, { -3652, 752, -3652, 892 },
    { -3652, 892, -3672, 952 }, { -2154, 1920, -2121, 1484 }, { -2136, 2070, -2154, 1920 },
    { -2121, 1484, -2114, 1320 }, { -2118, -1680, -2068, -2055 }, { -2118, -1500, -2118, -1680 },
    { -2114, 1320, -2048, 900 }, { -2107, 120, -2037, -300 }, { -2100, -1080, -2118, -1500 },
    { -2080, 345, -2107, 120 }, { -2068, -2055, -2039, -2280 }, { -2065, 2520, -2136, 2070 },
    { -2062, -880, -2100, -1080 }, { -2048, 900, -2018, 720 }, { -2039, -2280, -2025, -2700 },
    { -2037, -300, -2012, -480 }, { -2028, 2970, -2065, 2520 }, { -2025, -4110, -2010, -4290 },
    { -2025, -3750, -2025, -4110 }, { -2025, -3510, -2025, -3750 }, { -2025, -3132, -2025, -3510 },
    { -2025, -2880, -2025, -3132 }, { -2025, -2700, -2025, -2880 }, { -2018, 720, -2080, 345 },
    { -2018, 3645, -2014, 3120 }, { -2014, 3120, -2028, 2970 }, { -2012, -480, -2062, -880 },
    { -2010, -4290, -2000, -4410 }, { -2004, 3849, -2018, 3645 }, { -2000, -4410, -1985, -4512 },
    { -1989, 3888, -2004, 3849 }, { -1985, -4512, -1975, -4580 }, { -1975, -4580, -1945, -4652 },
    { -1954, 3979, -1989, 3888 }, { -1945, -4652, -1925, -4700 }, { -1925, -4700, -1895, -4730 },
    { -1909, 4000, -1954, 3979 }, { -1895, -4730, -1875, -4750 }, { -1875, -4750, -1815, -4750 },
    { -1815, -4750, -1775, -4750 }, { -1804, 4049, -1909, 4000 }, { -1775, -4750, -1745, -4720 },
    { -1745, -4720, -1725, -4700 }, { -1725, -4700, -1695, -4628 }, { -1709, -3412, -1491, -3130 },
    { -1695, -4628, -1675, -4580 }, { -1685, -3071, -1822, -3071 }, { -1675, -4580, -1660, -4478 },
    { -1667, -3254, -1685, -3071 }, { -1660, -4478, -1650, -4410 }, { -1650, -4410, -1635, -4230 },
    { -1640, -3520, -1667, -3254 }, { -1635, -4230, -1625, -4110 }, { -1634, -3756, -1640, -3520 },
    { -1625, -4110, -1634, -3756 }, { -1491, -3130, -1176, -2935 }, { -1384, 4032, -1804, 4049 },
    { -1225, 4034, -1384, 4032 }, { -1176, -2935, -1045, -2554 }, { -1075, 4054, -1225, 4034 },
    { -1045, -2554, -945, -2341 }, { -945, -2341, -692, -2018 }, { -725, 4089, -1075, 4054 },
    { -692, -2018, -422, -1678 }, { -678, 1588, -393, 1597 }, { -532, -390, -454, -121 },
    { -472, -740, -532, -390 }, { -454, -121, -382, 299 }, { -422, -1678, -392, -1448 },
    { -408, 635, -408, 1009 }, { -408, 1009, -383, 1231 }, { -393, 1597, -308, 1884 },
    { -392, -1448, -392, -1091 }, { -392, -1091, -472, -740 }, { -383, 1231, -559, 1607 },
    { -382, 299, -408, 635 }, { -375, 4082, -725, 4089 }, { -308, 1884, -223, 2172 },
    { -223, 2172, -8, 2382 }, { -25, 4070, -375, 4082 }, { -8, 2382, 207, 2591 },
    { 207, 2591, 422, 2801 }, { 275, 4070, -25, 4070 }, { 422, 2801, 636, 3011 },
    { 575, 4070, 275, 4070 }, { 636, 3011, 850, 3220 }, { 850, 3220, 1065, 3430 },
    { 875, 4070, 575, 4070 }, { 1065, 3430, 1485, 3550 }, { 1175, 4070, 875, 4070 },
    { 1475, 4070, 1175, 4070 }, { 1485, 3550, 1905, 3670 }, { 1775, 4070, 1475, 4070 },
    { 1905, 3670, 2205, 3670 }, { 2075, 4070, 1775, 4070 }, { 2205, 3670, 2505, 3670 },
    { 2375, 4070, 2075, 4070 }, { 2505, 3670, 2715, 3670 }, { 2675, 4070, 2375, 4070 },
    { 2715, 3670, 2925, 3670 }, { 2925, 3670, 3075, 3670 }, { 2975, 4070, 2675, 4070 },
    { 3075, 3670, 3225, 3670 }, { 3095, 4070, 2975, 4070 }, { 3215, 4070, 3095, 4070 },
    { 3225, 3670, 3274, 3692 }, { 3270, 4045, 3215, 4070 }, { 3274, 3692, 3323, 3715 },
    { 3323, 3715, 3349, 3769 }, { 3325, 4020, 3270, 4045 }, { 3349, 3769, 3375, 3823 },
    { 3352, 3966, 3325, 4020 }, { 3375, 3823, 3377, 3868 }, { 3377, 3868, 3379, 3913 },
    { 3379, 3913, 3352, 3966 },
};
constexpr float kSunsetBank = 30.0f, kSunsetBankSlope = 60.0f / 30.0f;
constexpr float kSunsetBuoys = 1600.0f, kSunsetShelfEnd = 3.0f * kSunsetBuoys;

float sunset_shore(float x, float z) {
    float best = 1e18f, distance = 1e9f;
    nearer_shore(kSunsetShore, sizeof(kSunsetShore) / sizeof(kSunsetShore[0]), x, z, best, distance);
    return distance;
}

// The floor's points are 256 units apart, far wider than the banks, so the
// floor between one under the land and the next out in the water must not
// pass below the banks' foot (60 down): under the land it lies 40 down, and
// from the water's edge goes on from 45, meeting the bank's face instead.
float sunset_depth(float x, float z) {
    const float d = sunset_shore(x, z);
    if (d <= 0.0f) return 40.0f;
    const float past = std::max(0.0f, d - kSunsetBank);
    return 45.0f + 300.0f * (1.0f - std::exp(-past / 1100.0f)) +
           shelf(d, kSunsetBuoys) +
           swells(x, z) * smoothstep(kSunsetBank, kSunsetBank + 500.0f, d) * (1.0f - smoothstep(kSunsetShelfEnd, kSunsetShelfEnd + 500.0f, d));
}

// Drake Lake (course 4): a lake, a ring of water between its outer shore and
// a long inner bank with more water inside it, and a small island. Its
// banks are walls going straight down to some 450 units. The whole of it is
// floored like Sunny Beach's channel: a little under the water at the banks'
// feet, then dropping steeply to some 11 m away from them.
constexpr Edge kDrakeShore[] = {
    { -5031, -543, -4976, -443 }, { -4997, -1114, -5031, -543 }, { -4983, -1342, -4997, -1114 },
    { -4976, -443, -4643, 157 }, { -4870, -1507, -4983, -1342 }, { -4838, 1324, -4816, 2035 },
    { -4816, 2035, -4813, 2124 }, { -4813, 2124, -4756, 2223 }, { -4756, 2223, -4414, 2817 },
    { -4660, 710, -4838, 1324 }, { -4643, 157, -4623, 456 }, { -4623, 456, -4616, 556 },
    { -4616, -2797, -4559, -2267 }, { -4616, 556, -4660, 710 }, { -4570, -3252, -4616, -2797 },
    { -4559, -2267, -4530, -2002 }, { -4536, -3593, -4570, -3252 }, { -4530, -2002, -4870, -1507 },
    { -4451, -3727, -4536, -3593 }, { -4414, 2817, -4373, 3114 }, { -4373, 3114, -4292, 3709 },
    { -4292, 3709, -4279, 3808 }, { -4279, 3808, -4198, 4402 }, { -4224, -4084, -4451, -3727 },
    { -4198, 4402, -3874, 4908 }, { -4120, -4247, -4224, -4084 }, { -3913, -4572, -4120, -4247 },
    { -3883, 1306, -3858, 1210 }, { -3874, 4908, -3766, 5076 }, { -3858, 1210, -3759, 828 },
    { -3809, -1002, -3741, -1154 }, { -3807, -902, -3809, -1002 }, { -3800, -502, -3807, -902 },
    { -3766, 5076, -3621, 5301 }, { -3759, 828, -3555, 484 }, { -3741, -1154, -3403, -1917 },
    { -3700, -369, -3800, -502 }, { -3649, 1665, -3883, 1306 }, { -3621, 5301, -3332, 5752 },
    { -3610, 1725, -3649, 1665 }, { -3599, -2897, -3558, -2915 }, { -3599, -2897, -3533, -3413 },
    { -3595, 3215, -3546, 2467 }, { -3558, 3288, -3595, 3215 }, { -3557, 2054, -3610, 1725 },
    { -3555, 484, -3504, 398 }, { -3546, 2467, -3530, 2218 }, { -3533, -3413, -3241, -3450 },
    { -3530, 2218, -3557, 2054 }, { -3504, 398, -3502, 23 }, { -3502, 23, -3501, -102 },
    { -3501, -102, -3700, -369 }, { -3483, 1106, -3230, 1494 }, { -3474, 1070, -3483, 1106 },
    { -3409, -1202, -3401, -740 }, { -3409, 3582, -3558, 3288 }, { -3403, -1917, -3599, -2897 },
    { -3401, -740, -3400, -702 }, { -3400, -702, -3123, -332 }, { -3394, -1236, -3409, -1202 },
    { -3377, -5086, -3266, -4975 }, { -3361, -4808, -3913, -4572 }, { -3359, 628, -3474, 1070 },
    { -3340, 596, -3359, 628 }, { -3332, 5752, -3275, 5787 }, { -3275, 5787, -2820, 6065 },
    { -3266, -4975, -3177, -4886 }, { -3241, -3450, -2764, -3298 }, { -3230, 1494, -3210, 1525 },
    { -3210, 1525, -3136, 1981 }, { -3206, -1659, -3394, -1236 }, { -3199, -3097, -3108, -2644 },
    { -3195, 3015, -3023, 3354 }, { -3193, 2978, -3195, 3015 }, { -3191, -1693, -3206, -1659 },
    { -3177, -4886, -3361, -4808 }, { -3162, 2517, -3193, 2978 }, { -3160, 2480, -3162, 2517 },
    { -3136, 1981, -3130, 2018 }, { -3130, 2018, -3160, 2480 }, { -3123, -332, -3101, -302 },
    { -3108, -2644, -3101, -2607 }, { -3104, 160, -3104, 198 }, { -3104, 198, -3340, 596 },
    { -3101, -2607, -3010, -2154 }, { -3101, -302, -3104, 160 }, { -3023, 3354, -3009, 3382 },
    { -3023, 3685, -3409, 3582 }, { -3010, -2154, -3003, -2117 }, { -3009, 3382, -2562, 3501 },
    { -3003, -2117, -3191, -1693 }, { -2926, 3711, -3023, 3685 }, { -2822, 3780, -2926, 3711 },
    { -2820, 6065, -2649, 6169 }, { -2764, -3298, -2365, -2998 }, { -2649, 6169, -2308, 6377 },
    { -2562, 3501, -2526, 3511 }, { -2526, 3511, -2370, 3615 }, { -2510, 3988, -2822, 3780 },
    { -2425, 4041, -2510, 3988 }, { -2393, -4710, -3177, -4886 }, { -2365, -2998, -1882, -2868 },
    { -2308, 6377, -1860, 6651 }, { -2302, 4126, -2510, 3988 }, { -2093, 4264, -2302, 4126 },
    { -2083, 4254, -2510, 3988 }, { -2083, 4254, -2425, 4041 }, { -2043, 4214, -2037, 4139 },
    { -2037, 4139, -2008, 3803 }, { -1966, -5609, -2526, -5594 }, { -1966, 6586, -1576, 6502 },
    { -1882, -2868, -1399, -2738 }, { -1879, 6805, -1482, 6805 }, { -1876, 6805, -1879, 6805 },
    { -1876, 6805, -1479, 6805 }, { -1860, 6651, -1711, 6742 }, { -1850, 4324, -2093, 4264 },
    { -1726, -5616, -1966, -5609 }, { -1617, 4364, -2043, 4214 }, { -1615, -4536, -2393, -4710 },
    { -1613, 4365, -1617, 4364 }, { -1611, 4363, -1613, 4365 }, { -1610, 4361, -1611, 4363 },
    { -1608, 4383, -1850, 4324 }, { -1576, 6502, -1184, 6418 }, { -1542, -3470, -1512, -3475 },
    { -1512, -3475, -596, -3658 }, { -1482, 6805, -1479, 6805 }, { -1450, -3103, -1542, -3470 },
    { -1450, -3095, -1450, -3103 }, { -1399, -2738, -949, -2519 }, { -1361, 4344, -1608, 4383 },
    { -1184, 6418, -848, 6200 }, { -1114, 4305, -1361, 4344 }, { -1026, -5631, -1726, -5616 },
    { -949, -2519, -500, -2300 }, { -870, 4170, -1114, 4305 }, { -848, 6200, -512, 5984 },
    { -821, -4442, -1615, -4536 }, { -726, -5637, -1026, -5631 }, { -626, 4034, -870, 4170 },
    { -596, -3658, -542, -3658 }, { -542, -3658, 305, -3612 }, { -512, 5984, -266, 5942 },
    { -500, -2300, -23, -2196 }, { -378, 3998, -626, 4034 }, { -304, -2609, -1450, -3095 },
    { -266, 5942, -20, 5901 }, { -250, -2593, -304, -2609 }, { -131, 3962, -378, 3998 },
    { -27, -5652, -726, -5637 }, { -27, -4347, -821, -4442 }, { -23, -2196, 468, -2287 },
    { -20, 5901, 227, 5860 }, { 116, 3926, -131, 3962 }, { 227, 5860, 474, 5818 },
    { 273, -5658, -27, -5652 }, { 305, -3612, 347, -3599 }, { 347, -3599, 1242, -3288 },
    { 363, 3890, 116, 3926 }, { 438, -2639, -250, -2593 }, { 462, -2635, 438, -2639 },
    { 463, -2059, 816, -2114 }, { 468, -2287, 957, -2393 }, { 474, 5818, 720, 5778 },
    { 610, 3854, 363, 3890 }, { 720, 5778, 966, 5736 }, { 768, -4253, -27, -4347 },
    { 816, -2114, 957, -2228 }, { 858, 3818, 610, 3854 }, { 957, -2393, 1451, -2471 },
    { 957, -2228, 1015, -2237 }, { 966, 5736, 1213, 5696 }, { 973, -5673, 273, -5658 },
    { 1015, -2237, 1451, -2306 }, { 1106, 3782, 858, 3818 }, { 1213, 5696, 1460, 5654 },
    { 1242, -3288, 1242, -3279 }, { 1242, -3279, 1425, -2912 }, { 1273, -5680, 973, -5673 },
    { 1353, 3747, 1106, 3782 }, { 1379, -2901, 462, -2635 }, { 1383, -2430, 1880, -2727 },
    { 1425, -2912, 1379, -2901 }, { 1460, 5654, 1706, 5612 }, { 1532, -2332, 1451, -2306 },
    { 1541, -4429, 768, -4253 }, { 1600, 3711, 1353, 3747 }, { 1706, 5612, 1953, 5572 },
    { 1848, 3675, 1600, 3711 }, { 1880, -2727, 2311, -2981 }, { 1953, 5572, 2200, 5530 },
    { 1973, -5695, 1273, -5680 }, { 2096, 3639, 1848, 3675 }, { 2200, 5530, 2446, 5489 },
    { 2273, -5701, 1973, -5695 }, { 2311, -2981, 2725, -3235 }, { 2321, -4607, 1541, -4429 },
    { 2343, 3603, 2096, 3639 }, { 2446, 5489, 2693, 5448 }, { 2589, 3646, 2343, 3603 },
    { 2693, 5448, 2940, 5407 }, { 2725, -3235, 3179, -3491 }, { 2835, 3689, 2589, 3646 },
    { 2940, 5407, 3186, 5365 }, { 2963, -5674, 2940, -5684 }, { 2972, -5716, 2273, -5701 },
    { 3055, 3624, 2821, 3661 }, { 3082, 3650, 2835, 3689 }, { 3091, -4826, 2321, -4607 },
    { 3101, -5616, 2940, -5684 }, { 3124, -5606, 2963, -5674 }, { 3124, -5606, 3101, -5616 },
    { 3165, -5589, 3124, -5606 }, { 3179, -3491, 3579, -3485 }, { 3186, 5365, 3433, 5324 },
    { 3271, -5722, 2972, -5716 }, { 3290, 3568, 3055, 3624 }, { 3300, -1081, 3349, -584 },
    { 3300, 400, 3450, 870 }, { 3329, 3611, 3082, 3650 }, { 3349, -584, 3398, -90 },
    { 3364, 3511, 3290, 3568 }, { 3398, -90, 3300, 400 }, { 3433, 5324, 3893, 4883 },
    { 3450, 870, 3602, 1346 }, { 3493, -5451, 3124, -5606 }, { 3493, -5451, 3165, -5589 },
    { 3507, -2019, 3550, -1514 }, { 3526, 3458, 3329, 3611 }, { 3550, -1514, 3300, -1081 },
    { 3579, -3485, 3700, -3000 }, { 3602, 1346, 3618, 1846 }, { 3604, -2509, 3507, -2019 },
    { 3618, 1846, 3797, 2313 }, { 3684, 3262, 3364, 3511 }, { 3700, -3000, 3604, -2509 },
    { 3709, 3196, 3684, 3262 }, { 3723, 3304, 3526, 3458 }, { 3770, 3058, 3723, 3304 },
    { 3797, 2313, 3811, 2813 }, { 3811, 2813, 3709, 3196 }, { 3817, 2813, 3770, 3058 },
    { 3861, -5044, 3091, -4826 }, { 3861, -5044, 3997, -5180 }, { 3893, 4883, 4046, 4736 },
    { 3994, -5037, 3861, -5044 }, { 3997, -5180, 4111, -5294 }, { 4046, 4736, 4511, 4292 },
    { 4260, -5024, 3994, -5037 }, { 4511, 4292, 4588, 4218 }, { 4588, 4218, 4848, 3461 },
    { 4739, -4787, 4260, -5024 }, { 4848, 3461, 5107, 2704 }, { 4963, -1284, 5140, -2064 },
    { 4978, -4668, 4739, -4787 }, { 5009, 1110, 5067, 312 }, { 5018, -487, 4963, -1284 },
    { 5058, 1906, 5009, 1110 }, { 5067, 312, 5018, -487 }, { 5107, 2704, 5058, 1906 },
    { 5135, -4452, 4978, -4668 }, { 5140, -2064, 5318, -2844 }, { 5318, -2844, 5496, -3624 },
    { 5448, -4021, 5135, -4452 }, { 5455, -3964, 5448, -4021 }, { 5496, -3624, 5455, -3964 },
};

float drake_shore(float x, float z) {
    float best = 1e18f, distance = 1e9f;
    nearer_shore(kDrakeShore, sizeof(kDrakeShore) / sizeof(kDrakeShore[0]), x, z, best, distance);
    return distance;
}

float drake_depth(float x, float z) {
    const float d = drake_shore(x, z);
    if (d <= 0.0f) return 70.0f;
    return 40.0f + 330.0f * (1.0f - std::exp(-d / 300.0f)) + swells(x, z) * smoothstep(200.0f, 600.0f, d);
}

// How far (x, z) is from the nearest of `count` edges, whichever side.
float edge_distance(const Edge* edges, size_t count, float x, float z) {
    float best = 1e18f, distance = 1e9f;
    nearer_shore(edges, count, x, z, best, distance);
    return std::fabs(distance);
}

// Marine Fortress (course 3) and Port Blue (course 5): walls standing out of
// the water -- a fortress's, and platforms' and a ship's -- rather than
// shores: thin and open-ended, so with no inside or outside to them that a
// shore's edges have. Their floors go by how far a point is from the
// nearest wall, either side. Marine Fortress's walls go down to some 60
// units, Port Blue's to 100 (its ship's to 300); each floor starts a little
// above that at a wall's foot, so as to meet it with no gap between.
constexpr Edge kMarineShore[] = {
    { -4245, -2540, -3270, -2540 }, { -4245, -2390, -4245, -2540 }, { -4245, -2340, -4245, -2390 },
    { -3920, -2340, -4245, -2340 }, { -3270, -2540, -2945, -2540 }, { -2983, 4861, -2974, 4765 },
    { -2983, 5261, -2983, 4861 }, { -2983, 5361, -2983, 5261 }, { -2974, 4765, -2926, 4372 },
    { -2967, 6154, -2951, 5761 }, { -2967, 6261, -2967, 6154 }, { -2951, 5676, -2983, 5361 },
    { -2951, 5761, -2951, 5676 }, { -2951, 6586, -2967, 6261 }, { -2951, 6661, -2951, 6586 },
    { -2945, -3640, -2939, -3730 }, { -2945, -3490, -2945, -3640 }, { -2945, -3140, -2945, -3490 },
    { -2945, -2990, -2945, -3140 }, { -2945, -2640, -2945, -2990 }, { -2945, -2490, -2945, -2640 },
    { -2945, -2340, -3920, -2340 }, { -2945, -2140, -2945, -2490 }, { -2945, -1990, -2945, -2140 },
    { -2945, -1640, -2945, -1990 }, { -2945, -1490, -2945, -1640 }, { -2945, -1140, -2945, -1490 },
    { -2945, -990, -2945, -1140 }, { -2945, -640, -2945, -990 }, { -2945, -490, -2945, -640 },
    { -2945, -140, -2945, -490 }, { -2945, 10, -2945, -140 }, { -2945, 360, -2945, 10 },
    { -2945, 880, -2945, 360 }, { -2945, 1360, -2945, 880 }, { -2945, 1360, -2945, 1454 },
    { -2945, 1454, -2945, 1800 }, { -2945, 1800, -2945, 1894 }, { -2945, 1894, -2945, 2240 },
    { -2945, 3361, -2945, 3468 }, { -2945, 3468, -2945, 3861 }, { -2945, 3861, -2945, 3968 },
    { -2945, 3968, -2945, 4361 }, { -2945, 4361, -2926, 4372 }, { -2940, 2300, -2945, 2240 },
    { -2940, 2300, -2920, 2520 }, { -2939, -3730, -2925, -3940 }, { -2925, -3940, -2916, -3994 },
    { -2916, -3994, -2895, -4120 }, { -2915, 2541, -2920, 2520 }, { -2915, 2541, -2895, 2620 },
    { -2896, 7880, -2608, 7840 }, { -2895, -4120, -2860, -4190 }, { -2895, 2620, -2884, 2641 },
    { -2884, 2641, -2845, 2720 }, { -2860, -4190, -2845, -4220 }, { -2855, 6757, -2951, 6661 },
    { -2845, -4220, -2810, -4255 }, { -2845, 2720, -2834, 2731 }, { -2845, 6767, -2855, 6757 },
    { -2836, 8060, -2896, 7880 }, { -2834, 2731, -2795, 2770 }, { -2810, -4255, -2795, -4270 },
    { -2805, 8155, -2836, 8060 }, { -2795, -4270, -2780, -4270 }, { -2795, 2770, -2784, 2770 },
    { -2784, 2770, -2745, 2770 }, { -2780, -4270, -2745, -4270 }, { -2745, -4270, -2710, -4235 },
    { -2745, 2770, -2734, 2759 }, { -2734, 2759, -2695, 2720 }, { -2710, -4235, -2695, -4220 },
    { -2695, -4220, -2660, -4150 }, { -2695, 2720, -2684, 2699 }, { -2684, 2699, -2645, 2620 },
    { -2660, -4150, -2645, -4120 }, { -2645, 2620, -2640, 2599 }, { -2640, 2599, -2620, 2520 },
    { -2638, -4090, -2645, -4120 }, { -2620, -4020, -2638, -4090 }, { -2620, 2520, -2615, 2490 },
    { -2615, 2490, -2595, 2380 }, { -2612, -3960, -2620, -4020 }, { -2595, -3820, -2612, -3960 },
    { -2595, -3820, -2595, -3470 }, { -2595, -3470, -2595, -3320 }, { -2595, -3320, -2595, -2970 },
    { -2595, -2970, -2595, -2820 }, { -2595, -2820, -2556, -2675 }, { -2567, 2276, -2595, 2380 },
    { -2567, 2276, -2466, 1897 }, { -2556, -2675, -2466, -2337 }, { -2552, 3361, -2945, 3361 },
    { -2466, -2337, -2424, -2182 }, { -2466, 1897, -2436, 1786 }, { -2445, 3361, -2552, 3361 },
    { -2445, 6783, -2845, 6767 }, { -2436, 1786, -2327, 1380 }, { -2424, -2182, -2327, -1820 },
    { -2345, 6783, -2445, 6783 }, { -2327, -1820, -2320, -1822 }, { -2324, 8005, -2805, 8155 },
    { -2320, -1822, -2304, -1827 }, { -2304, -1827, -2205, -1719 }, { -2251, 1304, -2327, 1380 },
    { -2251, 1304, -1974, 1027 }, { -2205, -1719, -1974, -1467 }, { -2170, 7955, -2324, 8005 },
    { -2142, 3099, -2445, 3361 }, { -2142, 3099, -2060, 3028 }, { -2104, 7780, -2608, 7840 },
    { -2104, 7780, -2095, 7805 }, { -2095, 7805, -2170, 7955 }, { -1974, -1467, -1781, -1260 },
    { -1974, 1027, -1893, 946 }, { -1945, 6767, -2345, 6783 }, { -1926, 2576, -2060, 3028 },
    { -1893, 946, -1595, 648 }, { -1890, 2453, -1926, 2576 }, { -1845, 6763, -1945, 6767 },
    { -1800, 7994, -1768, 7901 }, { -1781, -1260, -1653, -1122 }, { -1776, 2066, -1890, 2453 },
    { -1776, 2066, -1745, 1961 }, { -1768, 7901, -1726, 7816 }, { -1653, -1122, -1461, -1067 },
    { -1625, 1684, -1745, 1961 }, { -1592, 1608, -1625, 1684 }, { -1494, 617, -1595, 648 },
    { -1494, 617, -1124, 505 }, { -1461, -1067, -1172, -985 }, { -1452, 6763, -1845, 6763 },
    { -1386, 8034, -1800, 7994 }, { -1345, 6767, -1452, 6763 }, { -1294, 1310, -1592, 1608 },
    { -1250, 7780, -1726, 7816 }, { -1213, 1229, -1294, 1310 }, { -1190, 8010, -1386, 8034 },
    { -1172, -985, -826, -886 }, { -1021, 474, -1124, 505 }, { -1021, 474, -645, 361 },
    { -1010, 7815, -1190, 8010 }, { -984, 7780, -1250, 7780 }, { -984, 7780, -1010, 7815 },
    { -979, 6708, -1345, 6767 }, { -908, 6694, -979, 6708 }, { -907, 1133, -1213, 1229 },
    { -907, 1133, -823, 1107 }, { -826, -886, -595, -820 }, { -645, 361, -538, 361 },
    { -595, -820, -395, -820 }, { -538, 361, -145, 361 }, { -528, 6571, -908, 6694 },
    { -528, 6571, -434, 6540 }, { -471, 7517, -604, 7449 }, { -447, 992, -823, 1107 },
    { -413, 7553, -471, 7517 }, { -395, -820, -95, -820 }, { -378, 7354, -604, 7449 },
    { -345, 961, -447, 992 }, { -342, 6491, -434, 6540 }, { -145, 361, -38, 361 },
    { -95, -820, 105, -820 }, { -38, 361, 355, 361 }, { -28, 7418, -413, 7553 },
    { 20, 6294, -342, 6491 }, { 41, 7172, -378, 7354 }, { 41, 7172, 54, 7213 },
    { 54, 7213, 84, 7388 }, { 84, 7388, -28, 7418 }, { 105, -820, 405, -820 },
    { 126, 961, -345, 961 }, { 255, 961, 126, 961 }, { 302, 6017, 20, 6294 },
    { 345, 961, 255, 961 }, { 355, 361, 423, 361 }, { 373, 5948, 302, 6017 },
    { 405, -820, 555, -820 }, { 423, 361, 674, 361 }, { 498, 7108, 328, 7066 },
    { 507, 6859, 328, 7066 }, { 555, -820, 905, -820 }, { 574, 7138, 498, 7108 },
    { 605, 5632, 373, 5948 }, { 674, 361, 674, 381 }, { 674, 381, 674, 386 },
    { 674, 386, 714, 386 }, { 674, 936, 674, 941 }, { 674, 941, 674, 961 },
    { 674, 961, 345, 961 }, { 674, 5539, 605, 5632 }, { 711, 5465, 674, 5539 },
    { 714, 386, 874, 386 }, { 755, 579, 755, 361 }, { 755, 661, 755, 579 },
    { 755, 879, 755, 661 }, { 755, 961, 755, 879 }, { 805, 361, 805, 443 },
    { 805, 443, 805, 661 }, { 805, 661, 805, 743 }, { 805, 743, 805, 961 },
    { 834, 936, 674, 936 }, { 839, 6484, 507, 6859 }, { 839, 6484, 873, 6526 },
    { 840, 3970, 845, 4074 }, { 840, 3970, 855, 3961 }, { 845, 4074, 878, 4561 },
    { 855, -39, 1205, -39 }, { 855, 20, 855, -39 }, { 855, 236, 855, 20 },
    { 855, 1086, 870, 1086 }, { 855, 1459, 855, 1086 }, { 855, 1561, 855, 1459 },
    { 855, 2032, 855, 1561 }, { 855, 2161, 855, 2032 }, { 855, 2632, 855, 2161 },
    { 855, 2761, 855, 2632 }, { 855, 3232, 855, 2761 }, { 855, 3361, 855, 3232 },
    { 855, 3832, 855, 3361 }, { 855, 3961, 855, 3832 }, { 861, 5167, 711, 5465 },
    { 870, 236, 855, 236 }, { 870, 1086, 874, 1086 }, { 873, 6526, 1003, 6657 },
    { 874, 236, 870, 236 }, { 874, 356, 874, 236 }, { 874, 386, 874, 356 },
    { 874, 936, 834, 936 }, { 874, 1056, 874, 936 }, { 874, 1056, 874, 1086 },
    { 877, 4671, 861, 5167 }, { 878, 4561, 877, 4671 }, { 898, -751, 1018, -744 },
    { 900, -772, 898, -751 }, { 900, -772, 905, -820 }, { 904, 6755, 574, 7138 },
    { 1003, 6657, 904, 6755 }, { 1018, -744, 1297, -727 }, { 1205, -39, 1355, -39 },
    { 1297, -727, 1417, -720 }, { 1355, -39, 1705, -39 }, { 1417, -720, 1697, -704 },
    { 1697, -704, 1840, -658 }, { 1705, -39, 1855, -39 }, { 1840, -658, 2173, -551 },
    { 1855, -39, 2205, -39 }, { 2173, -551, 2316, -505 }, { 2205, -39, 2355, -39 },
    { 2316, -505, 2649, -399 }, { 2355, -39, 2705, -39 }, { 2649, -399, 2799, -399 },
    { 2705, -39, 2855, -39 }, { 2799, -399, 3149, -399 }, { 2855, -39, 3204, -57 },
    { 3149, -399, 3238, -391 }, { 3238, -391, 3447, -371 }, { 3354, -65, 3204, -57 },
    { 3354, -65, 3498, -86 }, { 3447, -371, 3481, -363 }, { 3481, -363, 3560, -345 },
    { 3498, -86, 3560, -95 }, { 3560, -345, 3630, -310 }, { 3590, -110, 3560, -95 },
    { 3630, -310, 3660, -295 }, { 3660, -295, 3695, -260 }, { 3660, -145, 3590, -110 },
    { 3675, -160, 3660, -145 }, { 3695, -260, 3710, -245 }, { 3710, -245, 3710, -230 },
    { 3710, -230, 3710, -195 }, { 3710, -195, 3675, -160 },
};

// Marine Fortress, in the fortress's greenish rock: sloping from the walls
// as Sunset Bay's does but deepening sooner, then gently on.
constexpr float kMarineEdge = 2000.0f, kMarineShelfEnd = 3.0f * kMarineEdge;
float marine_depth(float x, float z) {
    const float d = edge_distance(kMarineShore, sizeof(kMarineShore) / sizeof(kMarineShore[0]), x, z);
    return 45.0f + 300.0f * (1.0f - std::exp(-d / 450.0f)) + shelf(d, kMarineEdge) +
           swells(x, z) * smoothstep(100.0f, 600.0f, d) * (1.0f - smoothstep(kMarineShelfEnd, kMarineShelfEnd + 500.0f, d));
}

constexpr Edge kPortBlueShore[] = {
    { -5900, -4150, -5800, -4150 }, { -5900, -3850, -5900, -4150 }, { -5900, -3750, -5900, -3850 },
    { -5900, -3750, -5600, -3750 }, { -5900, -2050, -5900, -2150 }, { -5900, -1750, -5900, -2050 },
    { -5900, -50, -5900, -150 }, { -5900, 250, -5900, -50 }, { -5900, 1950, -5900, 1850 },
    { -5900, 2250, -5900, 1950 }, { -5800, -4150, -5500, -4150 }, { -5800, -1750, -5900, -1750 },
    { -5800, -1750, -5500, -1750 }, { -5800, 250, -5900, 250 }, { -5800, 250, -5500, 250 },
    { -5800, 2250, -5900, 2250 }, { -5600, -3750, -5500, -3750 }, { -5600, -2150, -5900, -2150 },
    { -5600, -150, -5900, -150 }, { -5600, 1850, -5900, 1850 }, { -5500, -4150, -5500, -4050 },
    { -5500, -4050, -5500, -3750 }, { -5500, -2150, -5600, -2150 }, { -5500, -2150, -5500, -1850 },
    { -5500, -1850, -5500, -1750 }, { -5500, -150, -5600, -150 }, { -5500, -150, -5500, 150 },
    { -5500, 150, -5500, 250 }, { -5500, 1850, -5600, 1850 }, { -5500, 1850, -5500, 2150 },
    { -5500, 2150, -5500, 2250 }, { -5500, 2250, -5800, 2250 }, { -5066, -5750, -4854, -5962 },
    { -4995, -5679, -5066, -5750 }, { -4854, -5962, -4783, -6033 }, { -4783, -5467, -4995, -5679 },
    { -4712, -5538, -4783, -5467 }, { -4571, -5821, -4783, -6033 }, { -4500, -5750, -4712, -5538 },
    { -4500, -5750, -4571, -5821 }, { -4000, -7500, -4000, -7429 }, { -4000, -7429, -4000, -7000 },
    { -4000, -7000, -3929, -6929 }, { -3929, -6929, -3500, -6500 }, { -3900, -2000, -3900, -3500 },
    { -3900, -1000, -3900, -2000 }, { -3900, 0, -3900, -1000 }, { -3900, 1000, -3900, 0 },
    { -3900, 2000, -3900, 1000 }, { -3900, 3000, -3900, 2000 }, { -3900, 4000, -3900, 3000 },
    { -3900, 4000, -3600, 5000 }, { -3600, 5000, -3210, 5500 }, { -3571, -7929, -4000, -7500 },
    { -3500, -8000, -3571, -7929 }, { -3500, -8000, -2214, -8000 }, { -3436, -6500, -3500, -6500 },
    { -3400, -4500, -3900, -3500 }, { -3210, 5500, -2940, 6000 }, { -3050, -6500, -3436, -6500 },
    { -3036, -6486, -3050, -6500 }, { -2950, -6400, -3036, -6486 }, { -2940, 6000, -2860, 6000 },
    { -2921, -6400, -2950, -6400 }, { -2921, -6400, -2750, -6400 }, { -2860, 6000, -2590, 5500 },
    { -2750, -6400, -2736, -6414 }, { -2650, -6500, -2736, -6414 }, { -2557, -6500, -2650, -6500 },
    { -2400, -4500, -3400, -4500 }, { -2350, 5596, -1950, 5596 }, { -2350, 5725, -2350, 5596 },
    { -2350, 5908, -2350, 5725 }, { -2350, 5963, -2350, 5908 }, { -2350, 5978, -2350, 5963 },
    { -2214, -8000, -2000, -8000 }, { -2200, 5000, -2590, 5500 }, { -2000, -8000, -1786, -8000 },
    { -2000, -6500, -2557, -6500 }, { -1976, 5978, -2350, 5978 }, { -1957, -6457, -2000, -6500 },
    { -1950, 5596, -1850, 5596 }, { -1900, -3500, -2400, -4500 }, { -1900, -3500, -1900, -2000 },
    { -1900, -3500, -1500, -3700 }, { -1900, -2000, -1900, -1000 }, { -1900, -1000, -1900, 0 },
    { -1900, 0, -1900, 1000 }, { -1900, 200, -1450, 200 }, { -1900, 1000, -1900, 2000 },
    { -1900, 2000, -1900, 3000 }, { -1900, 4000, -2200, 5000 }, { -1900, 4000, -1900, 3000 },
    { -1850, 5596, -1850, 5725 }, { -1850, 5725, -1850, 5908 }, { -1850, 5908, -1850, 5978 },
    { -1850, 5978, -1976, 5978 }, { -1786, -8000, -500, -8000 }, { -1700, -6200, -1957, -6457 },
    { -1700, -6200, -1700, -6186 }, { -1700, -6186, -1700, -6100 }, { -1700, -6100, -1671, -6071 },
    { -1671, -6071, -1500, -5900 }, { -1500, -5900, -1486, -5900 }, { -1500, -3700, -1100, -3900 },
    { -1500, 5196, -1100, 5196 }, { -1500, 5325, -1500, 5196 }, { -1500, 5508, -1500, 5325 },
    { -1500, 5563, -1500, 5508 }, { -1500, 5578, -1500, 5563 }, { -1486, -5900, -1400, -5900 },
    { -1450, 200, -1000, 200 }, { -1357, -5857, -1400, -5900 }, { -1150, 4796, -750, 4796 },
    { -1150, 4925, -1150, 4796 }, { -1150, 5108, -1150, 4925 }, { -1150, 5163, -1150, 5108 },
    { -1150, 5178, -1150, 5163 }, { -1126, 5578, -1500, 5578 }, { -1100, -5600, -1357, -5857 },
    { -1100, -5579, -1100, -5600 }, { -1100, -5450, -1100, -5579 }, { -1100, -4650, -1014, -4736 },
    { -1100, -4621, -1100, -4650 }, { -1100, -4621, -1100, -4450 }, { -1100, -3900, -1014, -3986 },
    { -1100, -3729, -1100, -3900 }, { -1100, -3700, -1100, -3729 }, { -1100, -2600, -1014, -2686 },
    { -1100, -2571, -1100, -2600 }, { -1100, -2400, -1100, -2571 }, { -1100, -1700, -1014, -1786 },
    { -1100, -1671, -1100, -1700 }, { -1100, -1500, -1100, -1671 }, { -1100, -800, -1014, -886 },
    { -1100, -771, -1100, -800 }, { -1100, -600, -1100, -771 }, { -1100, 1200, -1014, 1114 },
    { -1100, 1229, -1100, 1200 }, { -1100, 1400, -1100, 1229 }, { -1086, -5436, -1100, -5450 },
    { -1086, -4436, -1100, -4450 }, { -1086, -3686, -1100, -3700 }, { -1086, -2386, -1100, -2400 },
    { -1086, -1486, -1100, -1500 }, { -1086, -586, -1100, -600 }, { -1086, 1414, -1100, 1400 },
    { -1086, 1414, -1000, 1500 }, { -1014, -3986, -1000, -4000 }, { -1014, -2686, -1000, -2700 },
    { -1014, -1786, -1000, -1800 }, { -1014, -886, -1000, -900 }, { -1014, 1114, -1000, 1100 },
    { -1000, -5350, -1086, -5436 }, { -1000, -5350, -962, -5350 }, { -1000, -4750, -1014, -4736 },
    { -1000, -4350, -1086, -4436 }, { -1000, -4350, -970, -4350 }, { -1000, -3600, -1086, -3686 },
    { -1000, -2829, -1000, -3600 }, { -1000, -2700, -1000, -2829 }, { -1000, -2643, -1000, -2700 },
    { -1000, -2300, -1086, -2386 }, { -1000, -2300, -1000, -2643 }, { -1000, -2229, -1000, -2300 },
    { -1000, -1800, -1000, -2229 }, { -1000, -1743, -1000, -1800 }, { -1000, -1400, -1086, -1486 },
    { -1000, -1400, -1000, -1743 }, { -1000, -1329, -1000, -1400 }, { -1000, -900, -1000, -1329 },
    { -1000, -843, -1000, -900 }, { -1000, -500, -1086, -586 }, { -1000, -500, -1000, -843 },
    { -1000, -500, -1000, -220 }, { -1000, -220, -1000, 200 }, { -1000, 200, -940, 200 },
    { -1000, 5196, -1100, 5196 }, { -1000, 5196, -1000, 5325 }, { -1000, 5325, -1000, 5508 },
    { -1000, 5508, -1000, 5578 }, { -1000, 5578, -1126, 5578 }, { -988, -4750, -1000, -4750 },
    { -980, -4000, -1000, -4000 }, { -970, -4350, -950, -4350 }, { -962, -5350, -950, -5350 },
    { -950, -5350, -650, -5350 }, { -950, -4750, -988, -4750 }, { -950, -4350, -530, -4350 },
    { -950, -4071, -950, -4500 }, { -950, -4071, -950, -3900 }, { -950, -4000, -980, -4000 },
    { -940, 200, -900, 200 }, { -925, 1100, -1000, 1100 }, { -900, 200, -210, 200 },
    { -900, 1100, -925, 1100 }, { -776, 5178, -1150, 5178 }, { -750, 4796, -650, 4796 },
    { -650, -5350, 250, -5350 }, { -650, 4796, -650, 4925 }, { -650, 4925, -650, 5108 },
    { -650, 5108, -650, 5178 }, { -650, 5178, -776, 5178 }, { -612, 1100, -900, 1100 },
    { -600, -1650, -560, -1930 }, { -600, -1530, -600, -1650 }, { -600, -1450, -600, -1530 },
    { -560, -1930, -500, -2350 }, { -530, -4350, -250, -4350 }, { -530, -4000, -950, -4000 },
    { -510, -1360, -600, -1450 }, { -500, -8000, -286, -8000 }, { -500, -2350, -200, -2650 },
    { -450, -1300, -510, -1360 }, { -330, -1300, -450, -1300 }, { -286, -8000, 1000, -8000 },
    { -250, -4350, -163, -4350 }, { -250, -4000, -530, -4000 }, { -250, -1300, -330, -1300 },
    { -210, 200, 250, 200 }, { -200, -2650, 250, -3100 }, { -163, -4350, -105, -4350 },
    { -150, -2205, -150, -2041 }, { -150, -2041, -150, -1795 }, { -150, -1795, -132, -1777 },
    { -132, -1777, -105, -1750 }, { -105, -4350, 217, -4068 }, { -50, -4750, -950, -4750 },
    { 50, -3700, -250, -4000 }, { 200, -1360, -250, -1300 }, { 217, -4068, 700, -3645 },
    { 237, -1792, -105, -1750 }, { 237, -1792, 751, -1856 }, { 250, -5350, 1000, -5350 },
    { 250, -4750, -50, -4750 }, { 250, -3500, 50, -3700 }, { 250, -3260, 250, -3500 },
    { 250, -3100, 250, -3260 }, { 250, 200, 370, 200 }, { 250, 1100, -612, 1100 },
    { 360, -2655, -150, -2205 }, { 370, 200, 550, 200 }, { 500, -4750, 250, -4750 },
    { 500, -1400, 200, -1360 }, { 500, 1100, 250, 1100 }, { 550, 200, 630, 160 },
    { 630, 160, 750, 100 }, { 650, -1250, 500, -1400 }, { 700, -3645, 700, -3369 },
    { 700, -3369, 700, -2955 }, { 700, -2955, 360, -2655 }, { 700, -100, 1200, -100 },
    { 750, -1150, 650, -1250 }, { 750, 100, 790, 20 }, { 790, 20, 850, -100 },
    { 810, -970, 750, -1150 }, { 850, -850, 810, -970 }, { 850, -400, 850, -850 },
    { 850, -100, 850, -400 }, { 929, 1500, -1000, 1500 }, { 951, -1694, 751, -1856 },
    { 951, -1694, 1250, -1450 }, { 1000, -8000, 1214, -8000 }, { 1000, -5350, 1250, -5350 },
    { 1214, -8000, 2500, -8000 }, { 1250, -5350, 2225, -5350 }, { 1250, -4750, 500, -4750 },
    { 1250, -1450, 1250, -910 }, { 1250, -910, 1250, -100 }, { 1250, -100, 1250, -70 },
    { 1250, -70, 1250, -50 }, { 1250, -50, 1250, 220 }, { 1250, 220, 1250, 400 },
    { 1250, 400, 1290, 440 }, { 1250, 1100, 500, 1100 }, { 1250, 1500, 929, 1500 },
    { 1290, 440, 1350, 500 }, { 1350, 500, 1650, 500 }, { 1400, -100, 1200, -100 },
    { 1575, -4750, 1250, -4750 }, { 1575, 1100, 1250, 1100 }, { 1650, 500, 2550, 500 },
    { 2225, -5350, 2550, -5350 }, { 2500, -8000, 2714, -8000 }, { 2536, 1500, 1250, 1500 },
    { 2550, -5350, 2562, -5350 }, { 2550, -4750, 1575, -4750 }, { 2550, 500, 2588, 500 },
    { 2550, 1100, 1575, 1100 }, { 2562, -5350, 2600, -5350 }, { 2562, 1100, 2550, 1100 },
    { 2588, -4750, 2550, -4750 }, { 2588, 500, 2600, 500 }, { 2600, -5550, 2614, -5564 },
    { 2600, -5379, 2600, -5550 }, { 2600, -5350, 2600, -5379 }, { 2600, -4750, 2588, -4750 },
    { 2600, -4750, 2600, -4707 }, { 2600, -4707, 2600, -4450 }, { 2600, -4450, 2600, -4284 },
    { 2600, -4284, 2600, -3288 }, { 2600, -3950, 3100, -3450 }, { 2600, -3122, 2600, -3288 },
    { 2600, -2125, 2600, -3122 }, { 2600, -1959, 2600, -2125 }, { 2600, -962, 2600, -1959 },
    { 2600, -962, 2600, 34 }, { 2600, 34, 2600, 200 }, { 2600, 200, 2643, 243 },
    { 2600, 1100, 2562, 1100 }, { 2600, 1175, 2600, 1100 }, { 2600, 1200, 2600, 1175 },
    { 2607, 493, 2600, 500 }, { 2614, -5564, 2700, -5650 }, { 2650, 250, 2643, 243 },
    { 2650, 250, 2650, 421 }, { 2650, 421, 2650, 450 }, { 2650, 450, 2607, 493 },
    { 2686, 1286, 2600, 1200 }, { 2700, -5650, 2843, -5650 }, { 2700, 1300, 2686, 1286 },
    { 2714, -8000, 4000, -8000 }, { 2750, 1500, 2536, 1500 }, { 2843, -5650, 3700, -5650 },
    { 3100, -3450, 3600, -2950 }, { 3100, -800, 2600, -300 }, { 3300, -2350, 3462, -2350 },
    { 3300, -2274, 3300, -2350 }, { 3300, -1950, 3300, -2274 }, { 3462, -2350, 3500, -2350 },
    { 3462, -1950, 3300, -1950 }, { 3500, -2350, 3538, -2331 }, { 3500, -1950, 3462, -1950 },
    { 3538, -2331, 3700, -2250 }, { 3538, -1969, 3500, -1950 }, { 3557, 1300, 2700, 1300 },
    { 3600, -2950, 3600, -2125 }, { 3600, -2125, 3600, -1300 }, { 3600, -1300, 3100, -800 },
    { 3700, -5650, 3821, -5529 }, { 3700, -2250, 3700, -2088 }, { 3700, -2088, 3700, -2050 },
    { 3700, -2050, 3538, -1969 }, { 3700, 1300, 3557, 1300 }, { 3821, -5529, 4550, -4800 },
    { 3821, 1179, 3700, 1300 }, { 4000, -8000, 4250, -8000 }, { 4036, 1500, 2750, 1500 },
    { 4100, -3774, 4100, -3850 }, { 4100, -3450, 4100, -3774 }, { 4100, -850, 4424, -850 },
    { 4100, -774, 4100, -850 }, { 4100, -450, 4100, -774 }, { 4176, -3450, 4100, -3450 },
    { 4176, -450, 4100, -450 }, { 4250, -8000, 5750, -8000 }, { 4250, 1500, 4036, 1500 },
    { 4424, -3850, 4100, -3850 }, { 4424, -3850, 4500, -3850 }, { 4424, -850, 4500, -850 },
    { 4464, 1500, 4250, 1500 }, { 4500, -3850, 4500, -3526 }, { 4500, -3526, 4500, -3450 },
    { 4500, -3450, 4176, -3450 }, { 4500, -850, 4500, -526 }, { 4500, -526, 4500, -450 },
    { 4500, -450, 4176, -450 }, { 4550, -4800, 5193, -4157 }, { 4550, 450, 3821, 1179 },
    { 5193, -4157, 5300, -4050 }, { 5193, -193, 4550, 450 }, { 5300, -3941, 5300, -4050 },
    { 5300, -3288, 5300, -3941 }, { 5300, -3288, 5300, -2291 }, { 5300, -2291, 5300, -2125 },
    { 5300, -2125, 5300, -1128 }, { 5300, -1128, 5300, -962 }, { 5300, -962, 5300, -867 },
    { 5300, -867, 5300, -300 }, { 5300, -300, 5193, -193 }, { 5750, -8000, 5750, -7714 },
    { 5750, -7714, 5750, -6000 }, { 5750, -6000, 5750, -5714 }, { 5750, -5714, 5750, -4000 },
    { 5750, -4000, 5750, -3714 }, { 5750, -3714, 5750, -2000 }, { 5750, -2000, 5750, -1714 },
    { 5750, -1714, 5750, 0 }, { 5750, 0, 5750, 214 }, { 5750, 214, 5750, 1500 },
    { 5750, 1500, 4464, 1500 },
};

// Port Blue, an oil rig's sea: level and deep, 15 m (kMaxDepth), in silt;
// rising only round the platforms' and the ship's feet, as mounds about rig
// legs, to meet their walls.
constexpr float kPortBlueDepth = kMaxDepth;
float port_blue_depth(float x, float z) {
    const float d = edge_distance(kPortBlueShore, sizeof(kPortBlueShore) / sizeof(kPortBlueShore[0]), x, z);
    return 95.0f + (kPortBlueDepth - 95.0f) * (1.0f - std::exp(-d / 250.0f));
}

// Glacier Coast (course 8): a great landmass to the north, a peninsula
// running south from it and islets off its tip, with banks going down some
// 60 units, as Sunset Bay's do; floored the same way, in grey rock, but
// dropping steeply away from the banks to the deepest (15 m) within some
// 30 m, as fjord water does.
constexpr Edge kGlacierShore[] = {
    { -9043, -11985, -9223, -12297 }, { -8998, -11907, -9043, -11985 }, { -8818, -11595, -8998, -11907 },
    { -8773, -11517, -8818, -11595 }, { -8773, -11517, -8716, -11446 }, { -8536, -11135, -8716, -11446 },
    { -8356, -10823, -8536, -11135 }, { -8311, -10745, -8356, -10823 }, { -8257, -10656, -8311, -10745 },
    { -8084, -10356, -8257, -10656 }, { -8030, -10254, -8084, -10356 }, { -7857, -9955, -8030, -10254 },
    { -7833, -9863, -7857, -9955 }, { -7725, -9466, -7833, -9863 }, { -7633, -9118, -7725, -9466 },
    { -7609, -9031, -7633, -9118 }, { -7600, -8940, -7609, -9031 }, { -7600, -8940, -7507, -8593 },
    { -7507, -8593, -7413, -8245 }, { -7376, -8166, -7413, -8245 }, { -7336, -8037, -7376, -8166 },
    { -7179, -7633, -7336, -8037 }, { -7179, -7280, -7179, -7633 }, { -7179, -7183, -7179, -7280 },
    { -7170, -7114, -7179, -7183 }, { -7170, -6754, -7170, -7114 }, { -7099, -6693, -7170, -6754 },
    { -6777, -6410, -7099, -6693 }, { -6711, -6336, -6777, -6410 }, { -6472, -6058, -6711, -6336 },
    { -6194, -5780, -6472, -6058 }, { -6118, -5704, -6194, -5780 }, { -6041, -5674, -6118, -5704 },
    { -5690, -5598, -6041, -5674 }, { -5584, -5589, -5690, -5598 }, { -5250, -5531, -5584, -5589 },
    { -5162, -5503, -5250, -5531 }, { -4810, -5411, -5162, -5503 }, { -4704, -5398, -4810, -5411 },
    { -4450, -981, -4081, -989 }, { -4450, 1, -4450, -981 }, { -4450, 16, -4450, 1 },
    { -4370, -5341, -4704, -5398 }, { -4369, 16, -4450, 16 }, { -4282, -5309, -4370, -5341 },
    { -4150, -2354, -3773, -2354 }, { -4150, -1405, -4150, -2354 }, { -4150, -1382, -4150, -1405 },
    { -4104, -4004, -4087, -4066 }, { -4087, -4066, -4012, -4329 }, { -4081, -989, -3600, -999 },
    { -4050, 446, -3673, 446 }, { -4050, 1395, -4050, 446 }, { -4050, 1417, -4050, 1395 },
    { -4040, -3507, -4104, -4004 }, { -4012, -4329, -3937, -4379 }, { -3950, 1417, -4050, 1417 },
    { -3937, -4379, -3674, -4529 }, { -3930, -5217, -4282, -5309 }, { -3773, -2354, -3450, -2354 },
    { -3704, -3995, -4040, -3507 }, { -3674, -4529, -3587, -4517 }, { -3673, 446, -3350, 446 },
    { -3638, -4056, -3704, -3995 }, { -3600, -5075, -3930, -5217 }, { -3600, -999, -3600, 2 },
    { -3600, 2, -3600, 16 }, { -3600, 16, -4369, 16 }, { -3587, -4517, -3354, -4484 },
    { -3563, -4106, -3638, -4056 }, { -3550, -1383, -4150, -1382 }, { -3550, -1383, -3450, -1383 },
    { -3542, -3039, -3531, -3277 }, { -3539, -3714, -3527, -3757 }, { -3538, -1925, -3523, -2066 },
    { -3534, -2825, -3542, -3039 }, { -3534, -1025, -3517, -1204 }, { -3531, -3277, -3524, -3361 },
    { -3527, -3757, -3477, -3924 }, { -3524, -3361, -3539, -3714 }, { -3523, -2066, -3487, -2375 },
    { -3518, -4146, -3563, -4106 }, { -3517, -5039, -3600, -5075 }, { -3517, -1204, -3486, -1475 },
    { -3506, -1629, -3538, -1925 }, { -3503, -2536, -3534, -2825 }, { -3491, -869, -3534, -1025 },
    { -3487, -2375, -3503, -2536 }, { -3486, -1475, -3506, -1629 }, { -3477, -3924, -3397, -4004 },
    { -3457, -736, -3491, -869 }, { -3448, -4177, -3518, -4146 }, { -3397, -4004, -3377, -4024 },
    { -3354, -4484, -3021, -4255 }, { -3350, 446, -3350, 1395 }, { -3350, 1395, -3350, 1417 },
    { -3350, 1417, -3950, 1417 }, { -3310, -437, -3457, -736 }, { -3245, -302, -3310, -437 },
    { -3232, -140, -3245, -302 }, { -3227, -4077, -3377, -4024 }, { -3215, 132, -3232, -140 },
    { -3190, -4090, -3227, -4077 }, { -3190, -4090, -3071, -4091 }, { -3186, -4897, -3517, -5039 },
    { -3178, 289, -3215, 132 }, { -3105, 566, -3178, 289 }, { -3103, -4862, -3186, -4897 },
    { -3072, 686, -3105, 566 }, { -3071, -4091, -2600, -4091 }, { -3021, -4255, -3448, -4177 },
    { -3004, -4833, -3103, -4862 }, { -2986, 1000, -3072, 686 }, { -2973, 1160, -2986, 1000 },
    { -2966, 5028, -2945, 4610 }, { -2948, 5091, -2966, 5028 }, { -2945, 4610, -2923, 4400 },
    { -2937, 1600, -2973, 1160 }, { -2923, 4400, -2850, 4107 }, { -2890, 5312, -2948, 5091 },
    { -2872, 5376, -2890, 5312 }, { -2860, 2800, -2830, 2560 }, { -2860, 3040, -2860, 2800 },
    { -2860, 3200, -2860, 3040 }, { -2850, 1800, -2937, 1600 }, { -2850, 4107, -2800, 4000 },
    { -2830, 2560, -2800, 2400 }, { -2830, 3440, -2860, 3200 }, { -2815, 5597, -2872, 5376 },
    { -2810, 3600, -2830, 3440 }, { -2800, 2000, -2850, 1800 }, { -2800, 2200, -2800, 2000 },
    { -2800, 2400, -2800, 2200 }, { -2800, 3800, -2810, 3600 }, { -2800, 4000, -2800, 3800 },
    { -2690, -4713, -3004, -4833 }, { -2609, 5870, -2815, 5597 }, { -2603, -4678, -2690, -4713 },
    { -2600, -4091, -2471, -4087 }, { -2532, 5944, -2609, 5870 }, { -2471, -4087, -2150, -4105 },
    { -2254, -4574, -2603, -4678 }, { -2230, 6034, -2532, 5944 }, { -2152, 6064, -2230, 6034 },
    { -2150, -4105, -2054, -4118 }, { -2054, -4118, -1700, -4153 }, { -2021, -4516, -2254, -4574 },
    { -2021, 6031, -2152, 6064 }, { -1963, -4501, -2021, -4516 }, { -1721, 6529, -1660, 6348 },
    { -1715, 5955, -2021, 6031 }, { -1700, -4153, -1346, -4137 }, { -1683, 6597, -1721, 6529 },
    { -1660, 6348, -1634, 6284 }, { -1634, 6284, -1464, 6348 }, { -1603, -4439, -1963, -4501 },
    { -1574, 6794, -1683, 6597 }, { -1518, 6802, -1574, 6794 }, { -1513, -4430, -1603, -4439 },
    { -1464, 6348, -1400, 6364 }, { -1455, 5778, -1715, 5955 }, { -1423, -4419, -1513, -4430 },
    { -1400, 6364, -1371, 6545 }, { -1380, 6834, -1518, 6802 }, { -1371, 6545, -1353, 6609 },
    { -1366, 6774, -1380, 6834 }, { -1353, 6609, -1366, 6774 }, { -1352, 5723, -1455, 5778 },
    { -1346, -4137, -1250, -4124 }, { -1250, -4124, -1142, -4138 }, { -1142, -4138, -800, -4138 },
    { -1107, 5493, -1352, 5723 }, { -1063, -4405, -1423, -4419 }, { -1035, 5415, -1107, 5493 },
    { -880, 6180, -781, 5885 }, { -823, 6254, -880, 6180 }, { -800, -4138, -458, -4153 },
    { -781, 5885, -740, 5783 }, { -740, 5273, -1035, 5415 }, { -740, 5783, -462, 5815 },
    { -703, -4423, -1063, -4405 }, { -660, 6437, -823, 6254 }, { -613, -4434, -703, -4423 },
    { -607, 5218, -740, 5273 }, { -562, 6469, -660, 6437 }, { -518, 5157, -607, 5218 },
    { -493, -4424, -613, -4434 }, { -462, 5815, -360, 5823 }, { -458, -4153, -350, -4145 },
    { -360, 5823, -197, 6022 }, { -350, -4145, -260, -4150 }, { -300, 6597, -562, 6469 },
    { -260, -4150, 100, -4134 }, { -255, 6475, -300, 6597 }, { -235, 4965, -518, 5157 },
    { -197, 6022, -140, 6100 }, { -140, 6100, -255, 6475 }, { -129, 536, -93, 857 },
    { -93, 857, -72, 989 }, { -74, 5340, 51, 5255 }, { -72, 989, -33, 1339 },
    { -57, 250, -129, 536 }, { -38, 1887, 0, 1990 }, { -33, 1339, -29, 1436 },
    { -32, 5404, -74, 5340 }, { -29, 136, -57, 250 }, { -29, 1436, -15, 1491 },
    { -15, 1491, -38, 1887 }, { -13, -4410, -493, -4424 }, { -5, 2795, -1, 2445 },
    { -1, 2445, 0, 2350 }, { 0, 2350, 0, 1990 }, { 0, 2410, 600, 2410 },
    { 10, 4735, -235, 4965 }, { 37, 2845, -5, 2795 }, { 37, 2845, 309, 3114 },
    { 51, 5255, 93, 5215 }, { 56, 31, -29, 136 }, { 92, 4667, 10, 4735 },
    { 93, 5215, 311, 5247 }, { 93, 5585, -32, 5404 }, { 100, -4134, 460, -4094 },
    { 169, 5597, 93, 5585 }, { 278, -4622, -13, -4410 }, { 309, 3114, 355, 3177 },
    { 311, 5247, 387, 5255 }, { 351, -4675, 278, -4622 }, { 355, 3177, 588, 3474 },
    { 362, -405, 56, 31 }, { 387, 5255, 560, 5340 }, { 387, 5645, 169, 5597 },
    { 424, -470, 362, -405 }, { 430, 4505, 92, 4667 }, { 438, -4752, 351, -4675 },
    { 441, 5577, 387, 5645 }, { 460, -4094, 550, -4074 }, { 519, 4469, 430, 4505 },
    { 526, 2700, 0, 2700 }, { 550, -4074, 640, -4088 }, { 560, 5340, 614, 5380 },
    { 588, 3474, 835, 3696 }, { 600, 2410, 1000, 2410 }, { 614, 5380, 441, 5577 },
    { 640, -4088, 1000, -4104 }, { 648, -695, 424, -470 }, { 701, -783, 648, -695 },
    { 715, -4953, 438, -4752 }, { 835, 3696, 1022, 3849 }, { 892, -1086, 701, -783 },
    { 941, 4183, 519, 4469 }, { 977, -1143, 892, -1086 }, { 986, 4700, 1063, 4567 },
    { 991, 4134, 941, 4183 }, { 992, -5154, 715, -4953 }, { 1000, -4104, 1360, -4088 },
    { 1000, 2380, 1000, 2410 }, { 1000, 2380, 1000, 2675 }, { 1000, 2675, 1000, 2700 },
    { 1000, 2700, 526, 2700 }, { 1003, 4015, 991, 4134 }, { 1004, 4792, 986, 4700 },
    { 1022, 3849, 1003, 4015 }, { 1033, 5085, 1004, 4792 }, { 1063, 4567, 1093, 4515 },
    { 1079, -5218, 992, -5154 }, { 1093, 4515, 1215, 4499 }, { 1105, 5045, 1033, 5085 },
    { 1163, -5265, 1079, -5218 }, { 1215, 4499, 1275, 4499 }, { 1234, 4720, 1269, 4853 },
    { 1240, 4664, 1234, 4720 }, { 1269, -1347, 977, -1143 }, { 1269, 4853, 1275, 4901 },
    { 1275, 4499, 1240, 4664 }, { 1275, 4901, 1105, 5045 }, { 1329, -1415, 1269, -1347 },
    { 1360, -4088, 1450, -4076 }, { 1450, -4076, 1552, -4089 }, { 1500, -5510, 1163, -5265 },
    { 1500, -5510, 1860, -5510 }, { 1552, -4089, 1900, -4089 }, { 1565, -1687, 1329, -1415 },
    { 1624, -1784, 1565, -1687 }, { 1685, -3916, 1885, -5183 }, { 1819, -2066, 1624, -1784 },
    { 1860, -5510, 1950, -5510 }, { 1885, -5183, 2010, -5933 }, { 1907, -2121, 1819, -2066 },
    { 1935, -3883, 1685, -3916 }, { 1950, -5510, 2058, -5524 }, { 2026, -4079, 1900, -4089 },
    { 2026, -4079, 2500, -4095 }, { 2199, -2325, 1907, -2121 }, { 2263, -2402, 2199, -2325 },
    { 2400, -5524, 2058, -5524 }, { 2490, -5510, 2400, -5524 }, { 2493, -2667, 2263, -2402 },
    { 2500, -4095, 2561, -4050 }, { 2549, -2706, 2493, -2667 }, { 2561, -4050, 2834, -3897 },
    { 2580, -5510, 2490, -5510 }, { 2610, -3916, 1935, -3883 }, { 2827, -2973, 2549, -2706 },
    { 2834, -3897, 2870, -3845 }, { 2870, -3845, 3044, -3650 }, { 2940, -5510, 2580, -5510 },
    { 2979, -3237, 2827, -2973 }, { 3044, -3650, 3058, -3580 }, { 3058, -3580, 3058, -3364 },
    { 3058, -3364, 2979, -3237 }, { 3300, -5510, 2940, -5510 }, { 3408, -5524, 3300, -5510 },
    { 3408, -5524, 3750, -5524 }, { 3840, -5510, 3750, -5524 }, { 3910, -4116, 2610, -3916 },
    { 4160, -4241, 3910, -4116 }, { 4200, -5510, 3840, -5510 }, { 4308, -5524, 4200, -5510 },
    { 4410, -4866, 4160, -4241 }, { 4650, -5524, 4308, -5524 }, { 4740, -5510, 4650, -5524 },
    { 4910, -5933, 4410, -4866 }, { 5100, -5510, 4740, -5510 }, { 5196, -5514, 5100, -5510 },
    { 5550, -5514, 5196, -5514 }, { 5636, -5535, 5550, -5514 }, { 5982, -5635, 5636, -5535 },
    { 6086, -5679, 5982, -5635 }, { 6414, -5775, 6086, -5679 }, { 6500, -5786, 6414, -5775 },
    { 6846, -5886, 6500, -5786 }, { 6904, -5945, 6846, -5886 }, { 7152, -6207, 6904, -5945 },
    { 7214, -6282, 7152, -6207 }, { 7456, -6539, 7214, -6282 }, { 7523, -6600, 7456, -6539 },
    { 7765, -6857, 7523, -6600 }, { 7773, -6961, 7765, -6857 }, { 7786, -7307, 7773, -6961 },
    { 7833, -7403, 7786, -7307 }, { 8003, -7756, 7833, -7403 }, { 8015, -7831, 8003, -7756 },
    { 8107, -8106, 8015, -7831 }, { 8149, -8219, 8107, -8106 }, { 8265, -8593, 8149, -8219 },
    { 8295, -8674, 8265, -8593 }, { 8407, -9017, 8295, -8674 }, { 8432, -9098, 8407, -9017 },
    { 8544, -9439, 8432, -9098 }, { 8576, -9546, 8544, -9439 }, { 8680, -9884, 8576, -9546 },
    { 8734, -9960, 8680, -9884 }, { 8928, -10256, 8734, -9960 }, { 8978, -10327, 8928, -10256 },
    { 9176, -10628, 8978, -10327 }, { 9226, -10717, 9176, -10628 }, { 9420, -11013, 9226, -10717 },
    { 9474, -11089, 9420, -11013 }, { 9668, -11385, 9474, -11089 }, { 9718, -11456, 9668, -11385 },
    { 9916, -11757, 9718, -11456 }, { 9965, -11832, 9916, -11757 }, { 10163, -12134, 9965, -11832 },
    { 10212, -12209, 10163, -12134 }, { 10409, -12510, 10212, -12209 }, { 10607, -12811, 10409, -12510 },
    { 10656, -12886, 10607, -12811 }, { 10705, -12961, 10656, -12886 }, { 10903, -13263, 10705, -12961 },
};

float glacier_shore(float x, float z) {
    float best = 1e18f, distance = 1e9f;
    nearer_shore(kGlacierShore, sizeof(kGlacierShore) / sizeof(kGlacierShore[0]), x, z, best, distance);
    return distance;
}

float glacier_depth(float x, float z) {
    const float d = glacier_shore(x, z);
    if (d <= 0.0f) return 40.0f;
    return 45.0f + 470.0f * (1.0f - std::exp(-d / 250.0f)) + swells(x, z) * smoothstep(100.0f, 600.0f, d);
}

// Southern Island (course 7): piers, towers and rocks standing in the
// sea, their feet going down to 200 or 300 units, among beaches going down
// to 50. With no closed shores, it goes by the nearest wall either side, as
// Marine Fortress does; and since its walls' feet lie at such different
// depths, each edge carries its own (measured as the lowest of the course's
// own vertices within 120 units of it), and the floor starts just above it
// there, meeting a pier's foot and a beach's alike, before sloping on as
// the open sea's does. The sign posts standing in the water are among them,
// so the floor rises to each one's foot too.
struct FootedEdge { float ax, az, bx, bz, foot; };
constexpr FootedEdge kSouthernShore[] = {
    { -2697, -938, -2686, -915, 50 }, { -2690, -1016, -2697, -938, 50 }, { -2688, -1042, -2690, -1016, 50 },
    { -2686, -915, -2653, -845, 50 }, { -2653, -845, -2632, -830, 50 }, { -2644, -1105, -2688, -1042, 50 },
    { -2632, -830, -2568, -785, 50 }, { -2629, -1126, -2644, -1105, 50 }, { -2600, 1394, -2600, 1390, 200 },
    { -2600, 1410, -2600, 1394, 200 }, { -2596, 1390, -2600, 1390, 200 }, { -2584, 1410, -2600, 1410, 200 },
    { -2580, 1390, -2596, 1390, 200 }, { -2580, 1406, -2580, 1390, 200 }, { -2580, 1410, -2584, 1410, 200 },
    { -2580, 1410, -2580, 1406, 200 }, { -2568, -785, -2542, -783, 50 }, { -2558, -1159, -2629, -1126, 50 },
    { -2542, -783, -2465, -776, 50 }, { -2535, -1170, -2558, -1159, 50 }, { -2465, -776, -2442, -787, 50 },
    { -2458, -1163, -2535, -1170, 50 }, { -2442, -787, -2371, -820, 50 }, { -2432, -1161, -2458, -1163, 50 },
    { -2420, 1390, -2420, 1394, 200 }, { -2420, 1394, -2420, 1410, 200 }, { -2416, 1390, -2420, 1390, 200 },
    { -2404, 1410, -2420, 1410, 200 }, { -2400, 1390, -2416, 1390, 200 }, { -2400, 1390, -2400, 1406, 200 },
    { -2400, 1406, -2400, 1410, 200 }, { -2400, 1410, -2404, 1410, 200 }, { -2368, -1117, -2432, -1161, 50 },
    { -2347, -1102, -2368, -1117, 50 }, { -2314, -1032, -2347, -1102, 50 }, { -2303, -1008, -2314, -1032, 50 },
    { -2303, -1008, -2138, -948, 50 }, { -2200, 1994, -2200, 1990, 200 }, { -2200, 2010, -2200, 1994, 200 },
    { -2196, 1990, -2200, 1990, 200 }, { -2184, 2010, -2200, 2010, 200 }, { -2180, 1990, -2196, 1990, 200 },
    { -2180, 2006, -2180, 1990, 200 }, { -2180, 2010, -2184, 2010, 200 }, { -2180, 2010, -2180, 2006, 200 },
    { -2138, -948, -1645, -769, 150 }, { -2130, 4350, -2130, 4450, 300 }, { -2130, 4450, -2130, 4650, 300 },
    { -2113, 4650, -2130, 4650, 300 }, { -2097, 4350, -2130, 4350, 300 }, { -2080, 4350, -2097, 4350, 300 },
    { -2080, 4350, -2080, 4550, 300 }, { -2080, 4550, -2080, 4650, 300 }, { -2080, 4650, -2113, 4650, 300 },
    { -2042, -700, -2371, -820, 100 }, { -2020, 1990, -2020, 1994, 200 }, { -2020, 1994, -2020, 2010, 200 },
    { -2016, 1990, -2020, 1990, 200 }, { -2004, 2010, -2020, 2010, 200 }, { -2000, 1394, -2000, 1390, 200 },
    { -2000, 1410, -2000, 1394, 200 }, { -2000, 1990, -2016, 1990, 200 }, { -2000, 1990, -2000, 2006, 200 },
    { -2000, 2006, -2000, 2010, 200 }, { -2000, 2010, -2004, 2010, 200 }, { -1996, 1390, -2000, 1390, 200 },
    { -1984, 1410, -2000, 1410, 200 }, { -1980, 1390, -1996, 1390, 200 }, { -1980, 1406, -1980, 1390, 200 },
    { -1980, 1410, -1984, 1410, 200 }, { -1980, 1410, -1980, 1406, 200 }, { -1820, 1390, -1820, 1394, 200 },
    { -1820, 1394, -1820, 1410, 200 }, { -1816, 1390, -1820, 1390, 200 }, { -1804, 1410, -1820, 1410, 200 },
    { -1800, 1390, -1816, 1390, 200 }, { -1800, 1390, -1800, 1406, 200 }, { -1800, 1406, -1800, 1410, 200 },
    { -1800, 1410, -1804, 1410, 200 }, { -1714, -581, -2042, -700, 150 }, { -1645, -769, -1513, -721, 150 },
    { -1600, 1994, -1600, 1990, 200 }, { -1600, 2010, -1600, 1994, 200 }, { -1596, 1990, -1600, 1990, 200 },
    { -1584, 2010, -1600, 2010, 200 }, { -1580, 1990, -1596, 1990, 200 }, { -1580, 2006, -1580, 1990, 200 },
    { -1580, 2010, -1584, 2010, 200 }, { -1580, 2010, -1580, 2006, 200 }, { -1513, -721, -987, -529, 200 },
    { -1420, 1990, -1420, 1994, 200 }, { -1420, 1994, -1420, 2010, 200 }, { -1416, 1990, -1420, 1990, 200 },
    { -1404, 2010, -1420, 2010, 200 }, { -1400, 1990, -1416, 1990, 200 }, { -1400, 1990, -1400, 2006, 200 },
    { -1400, 2006, -1400, 2010, 200 }, { -1400, 2010, -1404, 2010, 200 }, { -1220, -401, -1714, -581, 150 },
    { -1056, -341, -1220, -401, 200 }, { -987, -529, -461, -338, 200 }, { -980, 4850, -780, 4850, 200 },
    { -980, 5000, -980, 4850, 200 }, { -980, 5150, -980, 5000, 200 }, { -924, -293, -1056, -341, 200 },
    { -780, 4850, -580, 4850, 200 }, { -780, 5150, -980, 5150, 200 }, { -729, 3857, -658, 3786, 0 },
    { -701, 3885, -729, 3857, 0 }, { -680, 3906, -701, 3885, 0 }, { -680, 3906, -572, 3798, 0 },
    { -658, 3786, -517, 3645, 0 }, { -580, 4850, -580, 5000, 200 }, { -580, 4950, -340, 4950, 200 },
    { -580, 5000, -580, 5150, 200 }, { -580, 5150, -780, 5150, 200 }, { -572, 3798, -468, 3694, 0 },
    { -517, 3645, -498, 3664, 0 }, { -506, 4080, -680, 3906, 250 }, { -498, 3664, -468, 3694, 0 },
    { -468, 3694, -294, 3868, 250 }, { -461, -338, -330, -290, 200 }, { -423, 3997, -506, 4080, 250 },
    { -398, -102, -924, -293, 200 }, { -340, 4950, -100, 4950, 200 }, { -340, 5050, -580, 5050, 200 },
    { -330, -290, 196, -98, 200 }, { -294, 3868, -423, 3997, 250 }, { -266, -54, -398, -102, 200 },
    { -100, 4950, 140, 4950, 200 }, { -100, 5050, -340, 5050, 200 }, { -100, 5250, -100, 5050, 200 },
    { -100, 5450, -100, 5250, 200 }, { 50, 5450, -100, 5450, 200 }, { 115, -4470, 115, -4520, 200 },
    { 115, -4460, 115, -4470, 200 }, { 115, -4350, 115, -4400, 200 }, { 115, -4340, 115, -4350, 200 },
    { 125, -4550, 320, -4550, 200 }, { 125, -4467, 125, -4550, 200 }, { 125, -4300, 125, -4467, 200 },
    { 140, 4950, 380, 4950, 200 }, { 140, 5050, -100, 5050, 200 }, { 144, -4275, 125, -4300, 200 },
    { 165, -4560, 220, -4560, 200 }, { 196, -98, 328, -50, 200 }, { 200, -4200, 144, -4275, 200 },
    { 200, 5050, 200, 5250, 200 }, { 200, 5250, 200, 5450, 200 }, { 200, 5450, 50, 5450, 200 },
    { 220, -4560, 225, -4560, 200 }, { 260, -4560, 315, -4560, 5 }, { 260, 138, -266, -54, 200 },
    { 275, -5475, 300, -5475, 200 }, { 275, -5392, 275, -5475, 200 }, { 280, -5070, 296, -5070, 50 },
    { 280, -5066, 280, -5070, 50 }, { 280, -5050, 280, -5066, 50 }, { 280, -5050, 284, -5050, 50 },
    { 280, -4770, 298, -4770, 50 }, { 280, -4768, 280, -4770, 50 }, { 280, -4750, 280, -4768, 50 },
    { 280, -4750, 282, -4750, 50 }, { 283, -5392, 275, -5392, 200 }, { 296, -5070, 300, -5070, 50 },
    { 298, -4770, 300, -4770, 50 }, { 300, -5475, 300, -5433, 200 }, { 300, -5392, 283, -5392, 200 },
    { 300, -5392, 300, -5433, 200 }, { 300, -5070, 300, -5054, 50 }, { 300, -5054, 300, -5050, 50 },
    { 300, -5050, 284, -5050, 50 }, { 300, -4770, 300, -4752, 50 }, { 300, -4752, 300, -4750, 50 },
    { 300, -4750, 282, -4750, 50 }, { 315, -4560, 320, -4560, 5 }, { 320, -4550, 450, -4550, 200 },
    { 328, -50, 854, 141, 200 }, { 331, -4200, 200, -4200, 200 }, { 350, -4560, 405, -4560, 200 },
    { 375, -4200, 331, -4200, 200 }, { 380, 4950, 620, 4950, 200 }, { 380, 5050, 140, 5050, 200 },
    { 391, 186, 260, 138, 200 }, { 400, -4233, 375, -4200, 200 }, { 405, -4560, 410, -4560, 200 },
    { 450, -4550, 450, -4400, 200 }, { 450, -4400, 450, -4300, 200 }, { 450, -4300, 400, -4233, 200 },
    { 460, -4520, 460, -4510, 200 }, { 460, -4510, 460, -4460, 200 }, { 460, -4400, 460, -4390, 200 },
    { 460, -4390, 460, -4340, 200 }, { 570, 7400, 570, 7417, 300 }, { 570, 7400, 770, 7400, 300 },
    { 570, 7417, 570, 7450, 300 }, { 570, 7450, 670, 7450, 300 }, { 620, 4950, 860, 4950, 200 },
    { 620, 5050, 380, 5050, 200 }, { 670, 7450, 870, 7450, 300 }, { 770, 7400, 870, 7400, 300 },
    { 854, 141, 986, 189, 200 }, { 860, 4950, 1100, 4950, 200 }, { 860, 5050, 620, 5050, 200 },
    { 870, 7400, 870, 7433, 300 }, { 870, 7433, 870, 7450, 300 }, { 917, 377, 391, 186, 200 },
    { 986, 189, 1603, 414, 200 }, { 1085, 438, 917, 377, 200 }, { 1100, 4950, 1340, 4950, 200 },
    { 1100, 5050, 860, 5050, 200 }, { 1340, 4950, 1580, 4950, 200 }, { 1340, 5050, 1100, 5050, 200 },
    { 1580, 4950, 1820, 4950, 200 }, { 1580, 5050, 1340, 5050, 200 }, { 1603, 414, 1757, 470, 200 },
    { 1693, 534, 1950, 277, 200 }, { 1757, 683, 1085, 438, 200 }, { 1820, 4950, 2066, 4950, 100 },
    { 1820, 5050, 1580, 5050, 200 }, { 1950, 277, 2400, -173, 200 }, { 1950, 1130, 1693, 534, 200 },
    { 2066, 4950, 2313, 4950, 200 }, { 2066, 5050, 1820, 5050, 100 }, { 2300, -5500, 2309, -5467, 50 },
    { 2300, 5000, 2309, 5033, 200 }, { 2309, -5467, 2327, -5400, 50 }, { 2309, 5033, 2327, 5100, 200 },
    { 2313, 5050, 2066, 5050, 200 }, { 2318, -5567, 2300, -5500, 50 }, { 2318, 4933, 2300, 5000, 200 },
    { 2327, -5600, 2318, -5567, 50 }, { 2327, -5400, 2351, -5376, 50 }, { 2327, 4900, 2318, 4933, 200 },
    { 2327, 5100, 2351, 5124, 200 }, { 2351, -5376, 2400, -5327, 50 }, { 2351, 5124, 2400, 5173, 200 },
    { 2376, -5649, 2327, -5600, 50 }, { 2376, 4851, 2327, 4900, 200 }, { 2400, -5673, 2376, -5649, 50 },
    { 2400, -5080, 2400, -5327, 50 }, { 2400, -4341, 2400, -5080, 150 }, { 2400, -4141, 2400, -4341, 150 },
    { 2400, -3341, 2400, -4141, 200 }, { 2400, -3105, 2400, -3341, 200 }, { 2400, -2159, 2400, -3105, 200 },
    { 2400, -1959, 2400, -2159, 200 }, { 2400, -1159, 2400, -1959, 200 }, { 2400, -370, 2400, -1159, 200 },
    { 2400, -173, 2400, -370, 200 }, { 2400, 2173, 1950, 1130, 200 }, { 2400, 2173, 2473, 2173, 200 },
    { 2400, 2350, 2400, 2173, 200 }, { 2400, 3059, 2400, 2350, 200 }, { 2400, 3280, 2400, 3059, 200 },
    { 2400, 3941, 2400, 3280, 150 }, { 2400, 4606, 2400, 3941, 150 }, { 2400, 4827, 2376, 4851, 200 },
    { 2400, 4827, 2400, 4606, 50 }, { 2400, 5173, 2433, 5182, 50 }, { 2433, 5182, 2500, 5200, 50 },
    { 2467, -5691, 2400, -5673, 50 }, { 2473, 2173, 2600, 2173, 200 }, { 2500, -5700, 2467, -5691, 50 },
    { 2500, 5200, 2533, 5191, 50 }, { 2533, 5191, 2600, 5173, 50 }, { 2550, 700, 2550, 775, 50 },
    { 2550, 700, 2681, 525, 75 }, { 2550, 775, 2550, 1300, 50 }, { 2550, 1300, 2569, 1325, 50 },
    { 2567, -5682, 2500, -5700, 50 }, { 2575, -173, 2400, -173, 200 }, { 2600, -5673, 2567, -5682, 50 },
    { 2600, -5327, 2600, -4670, 50 }, { 2600, -5327, 2624, -5351, 50 }, { 2600, -4670, 2600, -4341, 50 },
    { 2600, -4341, 2600, -4008, 50 }, { 2600, -4008, 2600, -3341, 50 }, { 2600, -3341, 2600, -2750, 50 },
    { 2600, -2750, 2600, -2159, 50 }, { 2600, -2159, 2600, -1826, 50 }, { 2600, -1826, 2600, -1159, 50 },
    { 2600, -1159, 2600, -666, 50 }, { 2600, -666, 2600, -173, 50 }, { 2600, -173, 2575, -173, 50 },
    { 2600, 2173, 2600, 2616, 50 }, { 2600, 2173, 2609, 2164, 50 }, { 2600, 2616, 2600, 3059, 50 },
    { 2600, 3059, 2600, 3500, 50 }, { 2600, 3500, 2600, 3941, 50 }, { 2600, 3941, 2600, 4236, 50 },
    { 2600, 4236, 2600, 4827, 50 }, { 2600, 5173, 2624, 5149, 50 }, { 2609, 2164, 2673, 2100, 75 },
    { 2624, -5351, 2673, -5400, 50 }, { 2624, 5149, 2673, 5100, 50 }, { 2649, -5624, 2600, -5673, 50 },
    { 2649, 4876, 2600, 4827, 50 }, { 2664, -109, 2600, -173, 75 }, { 2673, -5600, 2649, -5624, 50 },
    { 2673, -5400, 2682, -5433, 50 }, { 2673, -100, 2664, -109, 75 }, { 2673, 2100, 2676, 2088, 75 },
    { 2673, 4900, 2649, 4876, 50 }, { 2673, 5100, 2682, 5067, 50 }, { 2676, 2088, 2700, 2000, 75 },
    { 2681, 525, 2700, 500, 75 }, { 2682, -5433, 2700, -5500, 50 }, { 2682, 5067, 2700, 5000, 50 },
    { 2691, -5533, 2673, -5600, 50 }, { 2691, 4967, 2673, 4900, 50 }, { 2697, -12, 2673, -100, 75 },
    { 2700, -5500, 2691, -5533, 50 }, { 2700, 0, 2697, -12, 75 }, { 2700, 0, 2775, 43, 125 },
    { 2700, 35, 2775, 78, 125 }, { 2700, 438, 2700, 0, 75 }, { 2700, 500, 2700, 438, 75 },
    { 2700, 1500, 2569, 1325, 75 }, { 2700, 1500, 2775, 1500, 125 }, { 2700, 1535, 2775, 1535, 125 },
    { 2700, 1562, 2700, 1500, 75 }, { 2700, 2000, 2700, 1562, 75 }, { 2700, 5000, 2691, 4967, 50 },
    { 2775, 43, 2875, 100, 125 }, { 2775, 78, 2875, 135, 125 }, { 2775, 1500, 2875, 1500, 125 },
    { 2775, 1535, 2875, 1535, 125 }, { 2809, 465, 2700, 465, 125 }, { 2809, 500, 2700, 500, 125 },
    { 2809, 1903, 2700, 1965, 125 }, { 2809, 1938, 2700, 2000, 125 }, { 2875, 100, 2875, 119, 125 },
    { 2875, 119, 2875, 135, 125 }, { 2875, 135, 2875, 241, 125 }, { 2875, 241, 2875, 465, 125 },
    { 2875, 465, 2809, 465, 125 }, { 2875, 465, 2875, 484, 125 }, { 2875, 484, 2875, 500, 125 },
    { 2875, 500, 2809, 500, 125 }, { 2875, 500, 2875, 547, 125 }, { 2875, 547, 2875, 640, 125 },
    { 2875, 640, 2875, 720, 20 }, { 2875, 720, 2875, 760, 20 }, { 2875, 760, 2875, 840, 20 },
    { 2875, 840, 2875, 880, 20 }, { 2875, 880, 2875, 920, 20 }, { 2875, 920, 2875, 1000, 20 },
    { 2875, 1000, 2875, 1080, 20 }, { 2875, 1080, 2875, 1120, 20 }, { 2875, 1120, 2875, 1160, 20 },
    { 2875, 1160, 2875, 1240, 20 }, { 2875, 1240, 2875, 1320, 20 }, { 2875, 1320, 2875, 1360, 20 },
    { 2875, 1360, 2875, 1407, 125 }, { 2875, 1407, 2875, 1500, 125 }, { 2875, 1500, 2875, 1519, 125 },
    { 2875, 1519, 2875, 1535, 125 }, { 2875, 1535, 2875, 1641, 125 }, { 2875, 1641, 2875, 1865, 125 },
    { 2875, 1865, 2809, 1903, 125 }, { 2875, 1865, 2875, 1884, 125 }, { 2875, 1884, 2875, 1900, 125 },
    { 2875, 1900, 2809, 1938, 125 },
};

// How far (x, z) is from the nearest of Southern Island's edges, and that
// edge's foot.
float southern_edge(float x, float z, float* foot) {
    float best = 1e18f;
    *foot = 0.0f;
    for (const FootedEdge& e : kSouthernShore) {
        const float ex = e.bx - e.ax, ez = e.bz - e.az;
        const float t = std::clamp(((x - e.ax) * ex + (z - e.az) * ez) / std::max(ex * ex + ez * ez, 1e-3f), 0.0f, 1.0f);
        const float dx = x - e.ax - t * ex, dz = z - e.az - t * ez;
        if (dx * dx + dz * dz < best) {
            best = dx * dx + dz * dz;
            *foot = e.foot;
        }
    }
    return std::sqrt(best);
}

float southern_depth(float x, float z) {
    float foot = 0.0f;
    const float d = southern_edge(x, z, &foot);
    const float start = std::max(10.0f, foot - 8.0f);
    const float sea = 45.0f + 300.0f * (1.0f - std::exp(-d / 1100.0f)) + shelf(d, kSunsetBuoys) +
                      swells(x, z) * smoothstep(100.0f, 600.0f, d);
    return std::max(sea, start * std::exp(-d / 400.0f));
}

// A course's floor: where its grid lies (its corner and its chunks each
// way), its sand (an RGBA16 block in one of the course's segments, or made
// here, kMade: its width and height in texels, each a power of two, and
// whether it repeats mirrored, for a texture whose edges do not meet --
// wrapped, Dolphin Park's lined up into stripes), and its depth. A texture's
// width must be its own, as the game loads it: 64-wide ones read as 32 wide
// came out in stripes, alternate rows from either half.
constexpr bool kWrapped = false, kMirrored = true;
constexpr int kMade = -1;
struct Floor {
    uint32_t course;
    float x0, z0;
    int across, down;
    int sand_segment;
    uint32_t sand;
    int width, height;
    bool mirrored;
    float (*depth)(float x, float z);
};
constexpr Floor kFloors[] = {
    // The top 32 rows of Dolphin Park's 32x64 beach strip, whose lower rows
    // fade into water.
    { 0, -25600.0f, -25600.0f, 50, 50, 13, 0xA5B0, 32, 32, kMirrored, dolphin_depth },
    // The sand of Sunny Beach's sandbar top, wrapped as the game wraps it
    // there. (The game's own floor is painted the water's turquoise, being
    // meant to be seen through it.)
    { 1, -20480.0f, -12288.0f, 40, 20, 14, 0x11800, 32, 32, kWrapped, sunny_depth },
    // The top 32 rows of Sunset Bay's 32x64 beach strip, sand going wet.
    { 2, -12288.0f, -13312.0f, 24, 26, 14, 0x0800, 32, 32, kMirrored, sunset_depth },
    // The lower half of a Drake Lake bank's 64x32 texture: olive-brown mud.
    { 4, -7168.0f, -7168.0f, 14, 15, 14, 0x6800, 64, 16, kMirrored, drake_depth },
    // Marine Fortress's green mossy rock, the top of its 32x64 texture.
    { 3, -12288.0f, -12288.0f, 23, 28, 14, 0x2800, 32, 32, kMirrored, marine_depth },
    // Port Blue has nothing like silt of its own -- metal, rust and signs --
    // so it is made: see make_silt.
    { 5, -11264.0f, -12288.0f, 21, 23, kMade, 0, 32, 32, kWrapped, port_blue_depth },
    // Glacier Coast's grey rock: rows 24 to 55 of its 32x64 cliff, below the
    // snow-capped top and above the water's edge, little snow left in it.
    { 8, -14336.0f, -14336.0f, 28, 28, 14, 0x3E00, 32, 32, kMirrored, glacier_depth },
    // Southern Island's beach sand, the top of its 32x64 strip.
    { 7, -12288.0f, -13312.0f, 24, 26, 14, 0x7000, 32, 32, kMirrored, southern_depth },
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

// A silt for a course without one of its own, at kMadeSand: 32x32 RGBA16,
// grey-brown, softly mottled by waves whose periods divide the texture's
// width, so that it wraps without a seam.
void make_silt(uint8_t* rdram) {
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const float u = 6.2831853f * float(x) / 32.0f, v = 6.2831853f * float(y) / 32.0f;
            float n = 0.45f * std::sin(u + 2.0f * v + 0.7f) + 0.35f * std::sin(3.0f * u - v + 1.9f) +
                      0.25f * std::sin(2.0f * u + 5.0f * v + 4.1f) + 0.20f * std::sin(7.0f * u + 3.0f * v + 2.6f);
            // A little grain from a hash of the texel.
            uint32_t h = uint32_t(x * 73856093) ^ uint32_t(y * 19349663);
            h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
            n += (float(h & 0xFF) / 255.0f - 0.5f) * 0.35f;
            const float shade = 1.0f + 0.12f * n;
            const int r = std::clamp(int(92.0f * shade), 0, 255), g = std::clamp(int(86.0f * shade), 0, 255),
                      b = std::clamp(int(70.0f * shade), 0, 255);
            const uint16_t texel = uint16_t(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
            *reinterpret_cast<uint16_t*>(rdram + ((kMadeSand + uint32_t(y * 32 + x) * 2) ^ 2)) = texel;
        }
    }
}

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
    // Loaded as a block of width x height texels, a line of width / 4
    // 64-bit words; the render tile repeats it, its masks the sizes' powers
    // of two.
    const auto log2 = [](int v) { uint32_t n = 0; while ((1 << (n + 1)) <= v) ++n; return n; };
    const uint32_t texels = uint32_t(floor.width * floor.height), words = uint32_t(floor.width) / 4;
    const uint32_t cm = floor.mirrored ? 1u : 0u;
    list.command(0xF5100000u, 0x07000000u);
    list.command(0xE6000000u, 0);
    list.command(0xF3000000u, ((texels - 1) << 12) | ((2048u + words - 1) / words) | 0x07000000u);
    list.command(0xE7000000u, 0);
    list.command(0xF5100000u | (words << 9), (cm << 18) | (log2(floor.height) << 14) | (cm << 8) | (log2(floor.width) << 4));
    list.command(0xF2000000u, (uint32_t(floor.width - 1) << 14) | (uint32_t(floor.height - 1) << 2));
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
                    const float y = -capped(floor.depth(x, z));
                    // The normal from the slope either way.
                    const float e = 40.0f;
                    const float dx = (capped(floor.depth(x + e, z)) - capped(floor.depth(x - e, z))) / (2 * e);
                    const float dz = (capped(floor.depth(x, z + e)) - capped(floor.depth(x, z - e))) / (2 * e);
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

// WR64_SEAFLOOR_TEXDUMP=<prefix> with WR64_SEAFLOOR_TEXLIST=<segment>:<offset
// in hex>:<texels>,...: a course's candidate sands, 32 texels wide RGBA16,
// once per course, as raw 8-bit RGBA (<prefix>_<course>_<segment>_<offset>.rgba).
void dump_textures(uint8_t* rdram, uint32_t course, const uint32_t segments[16]) {
    static const char* prefix = std::getenv("WR64_SEAFLOOR_TEXDUMP");
    static const char* list = std::getenv("WR64_SEAFLOOR_TEXLIST");
    static uint32_t dumped = ~0u;
    if (prefix == nullptr || list == nullptr || dumped == course) return;
    dumped = course;
    for (const char* at = list; *at != 0;) {
        int segment = 0;
        unsigned offset = 0, texels = 0;
        int used = 0;
        if (std::sscanf(at, "%d:%x:%u%n", &segment, &offset, &texels, &used) != 3 || segment < 0 || segment > 15) break;
        at += used;
        if (*at == ',') ++at;
        char path[512];
        std::snprintf(path, sizeof(path), "%s_%u_%d_%05X.rgba", prefix, course, segment, offset);
        if (FILE* f = std::fopen(path, "wb")) {
            const uint32_t base = (segments[segment] + offset) & 0x7FFFFF;
            for (unsigned i = 0; i < texels; ++i) {
                const uint16_t p = *reinterpret_cast<const uint16_t*>(rdram + ((base + i * 2) ^ 2));
                const uint8_t rgba[4] = { uint8_t((p >> 11) << 3), uint8_t(((p >> 6) & 31) << 3), uint8_t(((p >> 1) & 31) << 3),
                                          uint8_t((p & 1) ? 255 : 0) };
                std::fwrite(rgba, 1, 4, f);
            }
            std::fclose(f);
        }
    }
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
        *height = std::max(-capped(dolphin_depth(x, z)), beach);
        return true;
    }
    if (course == 3 || course == 5 || course == 7) {
        // Walls, not shores: the floor everywhere (standing on a wall's top
        // is the course's collision's to say).
        *height = -capped(floor->depth(x, z));
        return true;
    }
    if (course == 2 || course == 4 || course == 8) {
        // Sunset Bay, Drake Lake and Glacier Coast: the land rising from the
        // water, as far as a beach's height; Sunset Bay's banks going steeply
        // down.
        const float d = course == 2 ? sunset_shore(x, z) : course == 4 ? drake_shore(x, z) : glacier_shore(x, z);
        if (d <= 0.0f) {
            *height = std::min(-d * kSunnyBeachSlope, 50.0f);
        } else {
            *height = -capped(floor->depth(x, z));
            if (course == 2 && d < kSunsetBank) *height = std::max(*height, -d * kSunsetBankSlope);
        }
        return true;
    }
    // Sunny Beach: the beaches' sand, rising from the water at their slope
    // to the sandbar's top or the mainland's, and going on under the water
    // as far as they reach; past them the floor.
    const Shore shore = sunny_shore(x, z);
    if (shore.distance <= 0.0f) {
        *height = std::min(-shore.distance * kSunnyBeachSlope, shore.sandbar ? 20.0f : 50.0f);
    } else {
        *height = -capped(sunny_depth(x, z));
        if (shore.distance < kBeachReach) *height = std::max(*height, -shore.distance * kSunnyBeachSlope);
    }
    return true;
}

// Whether what stands out of the water on course `course` is walls, too
// tall to be flown over: Marine Fortress's and Port Blue's.
bool course_has_walls(uint32_t course) {
    return course == 3 || course == 5;
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
    // Sunset Bay's town and Drake Lake's banks: solid a little way in from
    // the water's edge.
    if (course == 2) return sunset_shore(x, z) + kLandEdge;
    // (Drake Lake's walked on further in, its island all through: landing
    // on it, the swim back found no way off.)
    if (course == 4) return drake_shore(x, z) + kDrakeLandEdge;
    // Marine Fortress's and Port Blue's walls, gone round.
    if (course == 3) return edge_distance(kMarineShore, sizeof(kMarineShore) / sizeof(kMarineShore[0]), x, z);
    if (course == 5) return edge_distance(kPortBlueShore, sizeof(kPortBlueShore) / sizeof(kPortBlueShore[0]), x, z);
    if (course == 7) {
        float foot = 0.0f;
        return southern_edge(x, z, &foot);
    }
    if (course == 8) return glacier_shore(x, z) + kLandEdge;
    if (course != 0) return nearest;
    for (const Outline& o : kSolid) nearest = std::min(nearest, outside(o, x, z));
    for (size_t i = 1; i < sizeof(kShore) / sizeof(kShore[0]); ++i) {
        nearest = std::min(nearest, std::hypot(x - kShore[i].x, z - kShore[i].z) - kPillar);
    }
    return nearest;
}

// Whether course `course`'s floor is drawn only with the eye below the
// waves. Drake Lake's water mixes in what lies beneath it for the game's own
// mirrored scenery (its authored reflection), and from above showed the bed
// through water that should look opaque: from above, the game's water is
// left as it was.
bool seafloor_only_below(uint32_t course) {
    return course == 4;
}

// The list drawing the seafloor round the frame's camera for the course
// loaded, as an address to call, or 0 for a course without one; `identity`
// is given an identity matrix to draw it under. `course` is the game's course
// ID and `segments` the frame's (13 and 14 the course's data, 3 the
// frame's buffer). Without waves merged round the camera, the floor round the
// course's middle.
uint32_t seafloor_list(uint8_t* rdram, uint32_t course, const uint32_t segments[16], uint32_t* identity) {
    *identity = 0x80000000u | (kBase + 0x40);
    dump_textures(rdram, course, segments);
    const Floor* floor = floor_for(course);
    const uint32_t segment13 = segments[13];
    const bool made = floor != nullptr && floor->sand_segment == kMade;
    if (seafloor_off() || floor == nullptr || segment13 == 0 || (!made && segments[floor->sand_segment] == 0)) return 0;
    const uint32_t sand = made ? kMadeSand : segments[floor->sand_segment] + floor->sand;
    static uint32_t built_course = ~0u, built_sand = 0;
    static bool ok = false;
    if (built_course != course || built_sand != sand) {
        built_course = course;
        built_sand = sand;
        ok = build(rdram, *floor, sand);
        if (ok && made) make_silt(rdram);
    }
    if (!ok) return 0;
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

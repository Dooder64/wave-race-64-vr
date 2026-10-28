// Seafloors under Dolphin Park, Sunny Beach, Sunset Bay (sand) and Drake
// Lake (mud), and the water's fog round an eye below the waves. Sunset Bay
// and Drake Lake are floored the same way as Sunny Beach, from shores
// measured the same way; see their depths below.
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
    // the buoys to the drop.
    const float past = std::max(0.0f, d - kBeachReach);
    const float channel = 330.0f * (1.0f - std::exp(-past / 300.0f));
    const float sea = 300.0f * (1.0f - std::exp(-past / 1100.0f)) + shelf_and_drop(d, kSunnyBuoys, kSunnyShelfEnd);
    const float in_channel = smoothstep(150.0f, 600.0f, z) * (1.0f - smoothstep(kSandbarEnd, kSandbarEnd + 1600.0f, std::fabs(x)));
    depth += in_channel * channel + (1.0f - in_channel) * sea;
    return depth + swells(x, z) * smoothstep(kBeachReach, kBeachReach + 500.0f, d) *
                       (1.0f - smoothstep(kSunnyShelfEnd, kSunnyShelfEnd + 500.0f, d));
}

// Sunset Bay (course 2): a long island with the town on it and a pier out
// to an islet at either side, the open sea all round, past a row of buoys
// some 1600 units off. Its banks are steep, 60 units down within 30 of the
// water's edge; past them the floor slopes as Dolphin Park's does, on past
// the buoys to three times their distance, and there falls away.
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
           shelf_and_drop(d, kSunsetBuoys, kSunsetShelfEnd) +
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
    // The top 32 rows of Sunset Bay's 32x64 beach strip, sand going wet.
    { 2, -12288.0f, -13312.0f, 24, 26, 14, 0x0800, kMirrored, sunset_depth },
    // The lower half of a Drake Lake bank's 32x64 texture: olive-brown mud.
    { 4, -7168.0f, -7168.0f, 14, 15, 14, 0x6800, kMirrored, drake_depth },
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
        *height = std::max(-dolphin_depth(x, z), beach);
        return true;
    }
    if (course == 2 || course == 4) {
        // Sunset Bay and Drake Lake: the land rising from the water, as far
        // as a beach's height; Sunset Bay's banks going steeply down.
        const float d = course == 2 ? sunset_shore(x, z) : drake_shore(x, z);
        if (d <= 0.0f) {
            *height = std::min(-d * kSunnyBeachSlope, 50.0f);
        } else {
            *height = -floor->depth(x, z);
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
    // Sunset Bay's town and Drake Lake's banks: solid a little way in from
    // the water's edge.
    if (course == 2) return sunset_shore(x, z) + kLandEdge;
    // (Drake Lake's walked on further in, its island all through: landing
    // on it, the swim back found no way off.)
    if (course == 4) return drake_shore(x, z) + kDrakeLandEdge;
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

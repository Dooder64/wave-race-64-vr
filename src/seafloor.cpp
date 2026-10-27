// A sand seafloor under Dolphin Park, and the water's fog round an eye below
// the waves.
//
// The game draws nothing under its water but a flat textured floor at the
// water's own level, so the water looks bottomless everywhere and a view
// from below the surface has nothing to see. Dolphin Park (course 0) is
// given a floor of its own instead: the course's sand, shallow beside the
// palm island and the piers in the middle, sloping gently deeper away from
// them, and at the buoy barrier (a ring of radius 7700 about the origin)
// falling away steeply into deep water. It is built once when the course is
// loaded, in world space, and drawn in place of the game's flat floor, before
// the water, so the water's depth tint (RT64's WaterPS: the deeper what lies
// under the surface, the more of the water's own colour) shades it from above.
//
// The island and the piers were measured from the course itself: the island
// from where its collision meets the waterline (WR64_SHORE_DUMP), the piers
// from the bottoms of their pillars (WR64_3D_TRACE_VERTS).
//
// Memory. Above the game's 4 MB, below the rewriter's scratch list at
// 0x700000: the floor at 0x500000, the fog at 0x5C0000. The cartridge never
// reads osMemSize, so nothing of the game's is there; build() still checks
// the floor's area is empty, or its own, before it writes.
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
namespace {

constexpr uint32_t kBase = 0x500000;
constexpr uint32_t kList = kBase + 0x100;
constexpr uint32_t kListEnd = kBase + 0x40000;
constexpr uint32_t kVertices = kBase + 0x40000;
constexpr uint32_t kVerticesEnd = kBase + 0xC0000;
constexpr uint32_t kMarker = 0x53454146u;       // "SEAF", at kBase

// The course's sand: the top 32 rows of its 32x64 RGBA16 beach strip, whose
// lower rows fade into water. In segment 13, the course's data. Its edges do
// not meet, so it is mirrored at each repeat; wrapped, the seams lined up
// into stripes across the floor.
constexpr uint32_t kSandTexture = 0xA5B0;

// The grid: chunks of 1024 units, each 4 cells of 256 a side, so a chunk's
// 25 vertices are one load and its texture coordinates start again at 0.
constexpr int kChunk = 1024, kCells = 4, kChunks = 22;
constexpr float kCell = float(kChunk) / kCells;
constexpr float kHalf = float(kChunk * kChunks) / 2.0f;
// The sand repeats every 128 units: 32 texels, 10.5 fixed point.
constexpr float kTexelsPerUnit = 32.0f / 128.0f;

// The barrier, and where the drop beyond it begins and ends.
constexpr float kBarrier = 7700.0f;

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

// How far below the water the floor lies at (x, z), in game units.
float depth_at(float x, float z) {
    float shore = 1e9f;
    for (const Anchor& a : kShore) shore = std::min(shore, std::hypot(x - a.x, z - a.z) - a.radius);
    shore = std::max(shore, 0.0f);
    const float r = std::hypot(x, z);
    // Shallow by the shore, a gentle slope out to about 7.5 m.
    float depth = 18.0f + 232.0f * smoothstep(0.0f, 5500.0f, shore);
    // Broad, low swells in the sand, flattening out towards the drop. Far
    // longer than the grid's cells: shorter ripples sampled at 256 units
    // showed as stripes.
    depth += 8.0f * std::sin(x * 0.0021f + z * 0.0008f) * std::sin(z * 0.0017f - x * 0.0006f) *
             (1.0f - smoothstep(kBarrier, kBarrier + 500.0f, r));
    // The drop: steep just past the barrier, then on down for good.
    depth += 5200.0f * smoothstep(kBarrier + 150.0f, kBarrier + 1600.0f, r) +
             std::max(0.0f, r - (kBarrier + 1600.0f)) * 0.6f;
    return depth;
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

// Writes the floor for the course data at physical `course_data` (segment
// 13); returns whether it fits and the area was free.
bool build(uint8_t* rdram, uint32_t course_data) {
    uint32_t* marker = reinterpret_cast<uint32_t*>(rdram + kBase);
    if (*marker != kMarker) {
        for (uint32_t a = kBase; a < kVerticesEnd; a += 4) {
            if (*reinterpret_cast<uint32_t*>(rdram + a) != 0) {
                std::fprintf(stderr, "[seafloor] memory at 0x%06X is in use; no seafloor\n", a);
                return false;
            }
        }
        *marker = kMarker;
    }
    // The identity matrix it is drawn under, after the marker.
    for (int i = 0; i < 16; ++i) {
        put_half(rdram, kBase + 0x40 + uint32_t(i) * 2, (i % 5) == 0 ? 1.0f : 0.0f);
        put_half(rdram, kBase + 0x60 + uint32_t(i) * 2, 0.0f);
    }

    Builder list{ rdram, kList, kListEnd };
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
    list.command(0xFD100000u, course_data + kSandTexture); // 32x32 RGBA16 sand, loaded as a block
    list.command(0xF5100000u, 0x07014050u);
    list.command(0xE6000000u, 0);
    list.command(0xF3000000u, 0x073FF100u);
    list.command(0xE7000000u, 0);
    list.command(0xF5101000u, 0x00054150u);                // render tile: 32x32, mirrored each way
    list.command(0xF2000000u, 0x0007C07Cu);

    // The sun, high and off to one side, gives the ripples and the drop
    // some relief.
    const float lx = 0.3f, ly = 0.9f, lz = 0.3f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    uint32_t vertex = kVertices;
    constexpr uint32_t kPerChunk = (kCells + 1) * (kCells + 1);
    for (int cz = 0; cz < kChunks; ++cz) {
        for (int cx = 0; cx < kChunks; ++cx) {
            if (vertex + kPerChunk * 16 > kVerticesEnd) return false;
            const float x0 = -kHalf + float(cx * kChunk), z0 = -kHalf + float(cz * kChunk);
            for (int j = 0; j <= kCells; ++j) {
                for (int i = 0; i <= kCells; ++i) {
                    const float x = x0 + float(i) * kCell, z = z0 + float(j) * kCell;
                    const float y = -depth_at(x, z);
                    // The normal from the slope either way.
                    const float e = 40.0f;
                    const float dx = (depth_at(x + e, z) - depth_at(x - e, z)) / (2 * e);
                    const float dz = (depth_at(x, z + e) - depth_at(x, z - e)) / (2 * e);
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
            list.command((0x04u << 24) | (kPerChunk << 9) | (kPerChunk * 16 - 1), vertex);
            for (int j = 0; j < kCells; ++j) {
                for (int i = 0; i < kCells; ++i) {
                    const uint32_t a = uint32_t(j * (kCells + 1) + i), b = a + 1;
                    const uint32_t c = a + (kCells + 1), d = c + 1;
                    list.command(0xBFu << 24, ((a * 5) << 16) | ((c * 5) << 8) | (d * 5));
                    list.command(0xBFu << 24, ((a * 5) << 16) | ((d * 5) << 8) | (b * 5));
                }
            }
            vertex += kPerChunk * 16;
        }
    }
    list.command(0xB8000000u, 0);
    if (list.full) return false;
    std::fprintf(stderr, "[seafloor] built: %u vertices, %u commands\n",
                 (vertex - kVertices) / 16, (list.at - kList) / 8);
    return true;
}

}  // namespace

// The list drawing the seafloor for the course loaded, as an address to
// call, or 0 for a course without one; `identity` is given an identity
// matrix to draw it under. `course` is the game's course ID and `segment13`
// the physical base of the course's data.
uint32_t seafloor_list(uint8_t* rdram, uint32_t course, uint32_t segment13, uint32_t* identity) {
    *identity = 0x80000000u | (kBase + 0x40);
    static const bool off = [] {
        const char* value = std::getenv("WR64_SEAFLOOR");
        return value != nullptr && value[0] == '0';
    }();
    if (off || course != 0 || segment13 == 0) return 0;
    static uint32_t built_for = 0;
    static bool ok = false;
    if (built_for != segment13) {
        built_for = segment13;
        ok = build(rdram, segment13);
    }
    return ok ? 0x80000000u | kList : 0;
}

}  // namespace wr64

// VR: waves all round the camera.
//
// The game builds its wave mesh to cover what its flat camera can see and no
// more: func_8004C1D0, while a frame is drawn, traces the camera's view down to
// the water, and in the next update func_80050B78 places a grid from the
// camera and func_80050F8C builds the mesh on it, into the frame's buffer
// behind segment 3, from which the static water list draws. A headset looks
// behind the camera too, where there is no water at all.
//
// So the mesh is built again for the camera turned a quarter, a half and three
// quarters round, after the game's own update has run. func_80051238, the
// update, also moves the waves on and draws random numbers, so it runs once,
// as the game runs it; only the placing and building are repeated. What they
// change is put back afterwards, so the game carries on from exactly where it
// would have been.
//
// What the placing and building change was measured (WR64_WATER_DIFF=1, the
// first version of this file): the mesh in the current frame's buffer, a few
// working areas of the water code, and the camera's directions, which are
// turned in place for func_8004C1D0. Those ranges, with a margin, are kept
// before the copies are built and put back after, and the mesh from each
// turned build is copied out first. WR64_WATER_DIFF=1 now checks this in a
// race instead: now and then it compares the game's whole 4 MB before and
// after the copies and reports anything still changed.
//
// The four meshes are not drawn as they are. Each is a hexagon of waves, about
// 1500 units across, on a triangular lattice fixed to the world (points 64
// apart along x, rows 64*sqrt(3)/2 apart along z, every other row offset by
// half a step), its outermost points flattened to the water level, inside a
// ring of six large flat quads -- and all of it translucent. Drawn on top of
// one another they overlap: the overlaps are doubly opaque, each flat ring
// cuts through the neighbouring waves, and each flattened border shows as a
// line. But the lattice is the same for all four, and where two cover the
// same point they agree on its height to a unit or two (measured with
// WR64_WATER_DUMP). So they are merged into one mesh instead: every lattice
// point of a hexagon round the camera takes the average of the meshes that
// cover it, each weighted by how far inside its own hexagon the point is --
// which leaves out the flattened borders and blends the shading where two
// meshes meet -- or lies flat at the water level if none does, and a ring of
// six flat quads reaches from its edge out to the distance, as the game's
// own does. That one mesh is drawn in place of the game's (src/dlrewrite.cpp),
// in the same state, so it looks like the game's water and gives the modern
// water renderer a single surface.
//
// The merged meshes are kept per frame buffer -- the game alternates between
// two, and segment 3 points into the one being drawn -- so one is never
// rewritten while the frame that uses it might still be read.

#include "recomp.h"
#include "wr64/vr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void func_80051238(uint8_t*, recomp_context*);
extern "C" void func_8004C1D0(uint8_t*, recomp_context*);
extern "C" void func_8004C998(uint8_t*, recomp_context*);
extern "C" void func_80050B78(uint8_t*, recomp_context*);
extern "C" void func_80050F8C(uint8_t*, recomp_context*);

namespace wr64 {

namespace {

// USA Rev A addresses, all KSEG0.
constexpr uint32_t kPlayers = 0x800DAB28;            // gPlayers
constexpr uint32_t kCameraIndex = 0x80223930;        // D_80223930
constexpr uint32_t kCameras = 0x80227C80;            // gCameraPerspective
constexpr uint32_t kCameraSize = 0x10C;
constexpr uint32_t kGfxPool = 0x801518B8;            // gGfxPool: the frame buffer being built
constexpr uint32_t kScratch = 0x80740000;            // above the rewriter's list, unused by the game

// Where func_8004C1D0 was told to write its vertices while a frame was drawn,
// for player one, for each of the game's two frame buffers: it alternates
// between two places as they do (0x801C45B8 with the first, 0x801C7BE8 with
// the second, in Rev A), so the place matching the buffer being built can be
// given to it. func_8004C998 is given the structure that output is +0x1C0 of.
uint32_t g_view_outputs[2] = { 0, 0 };

// The game's two frame buffers, each the base of segment 3 while its frame is
// drawn, and the part of either the water list's vertices occupy (the mesh is
// written from +0x13D68 to about +0x15B18).
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };
constexpr uint32_t kMeshStart = 0x13D00;
constexpr uint32_t kMeshSize = 0x5000;

// The part of the mesh area the water list loads vertices from: the
// hexagon's lattice and its ring's corners, which are told apart by position.
constexpr uint32_t kLatticeStart = 0x13D68;
constexpr uint32_t kLatticeEnd = 0x15B18;

// The full grid, which list 0x010082F0 draws from the same place for the
// results screen: a hexagon of waves 16 rows each way round the origin, 817
// points, with no ring round it, which ends here.
constexpr uint32_t kFullGridEnd = 0x17078;

// The game's view matrix in either frame buffer, after its projection.
constexpr uint32_t kViewMatrix = 0xE088;

// The turned builds: a quarter, a half and three quarters round.
constexpr int kTurns = 3;

// The merged mesh for each frame buffer, in physical memory above the game's
// 4 MB and the rewriter's list (src/dlrewrite.cpp) and below the audio task's
// copy at 0x7E0000: its vertices first, then the display list that draws them.
constexpr uint32_t kMerged = 0x741000;
constexpr uint32_t kMergedStride = 0x40000;
constexpr uint32_t kMergedList = 0x30000;          // where the list starts in each
constexpr uint32_t kMergedVertices = kMergedList / 16;
constexpr uint32_t kMergedCommands = (kMergedStride - kMergedList) / 8;

// Everything else a turned build was measured to change, with a margin, in
// physical addresses; the camera is kept separately. The large range is a
// grid of rows 0x200 apart beside the wave heights at D_80162420, of which
// a build fills in the cells its view covers -- wherever the camera is -- so
// it is kept whole.
struct Range { uint32_t start, size; };
constexpr Range kKept[] = {
    { 0x0D4700, 0x100 },
    { 0x160A80, 0x31A00 },      // the grid, through the water globals at 0x192420
    { 0x1C4300, 0x400 },
    { 0x1C7900, 0x400 },
};

// Which frame buffer each merged mesh was built for, so the rewriter only
// draws one that matches the frame it is rewriting.
bool g_copies_ready[2] = { false, false };

// The camera the last copies were built for, for WR64_WATER_DUMP.
float g_copy_camera[3] = { 0.0f, 0.0f, 0.0f };

// For each frame buffer, the camera its merged mesh was built for and the
// mesh's hexagon, for the sky (patches/sky_all_round.cpp).
struct BuiltFor { float eye[3]; float centre_x, centre_z, radius; };
BuiltFor g_built_for[2] = {};

uint32_t physical(uint32_t address) { return address & 0x00FFFFFFu; }

float& word_f(uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<float*>(rdram + physical(address));
}

int32_t& word_i(uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<int32_t*>(rdram + physical(address));
}

// Turns a camera's directions about the vertical by `angle` radians: its
// forward (0x64) and up (0x70) vectors, the point it looks at (0x7C) about its
// eye (0x4C), and its heading flattened onto the water (0xF0 and 0xF4, x and
// z) -- the one the grid is laid out along; func_8004C998 reads nothing else
// of the camera but its position. The fields the water code reads.
void turn_camera(uint8_t* rdram, uint32_t camera, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    auto turn = [&](uint32_t x_offset, uint32_t z_offset, float origin_x, float origin_z) {
        float& x = word_f(rdram, camera + x_offset);
        float& z = word_f(rdram, camera + z_offset);
        const float dx = x - origin_x, dz = z - origin_z;
        x = origin_x + c * dx + s * dz;
        z = origin_z - s * dx + c * dz;
    };
    turn(0x64, 0x6C, 0.0f, 0.0f);
    turn(0x70, 0x78, 0.0f, 0.0f);
    turn(0x7C, 0x84, word_f(rdram, camera + 0x4C), word_f(rdram, camera + 0x54));
    turn(0xF0, 0xF4, 0.0f, 0.0f);
}

// The game's building of the mesh for a camera turned `angle` round, into the
// game's own buffers, in both its halves. While a frame is drawn,
// func_8004C998 lays the vertices out over the patch of water the camera saw
// (D_801924B4 and on, which func_8004C1D0 then works out afresh); in the next
// update func_80050B78 places the grid and func_80050F8C sets the waves'
// heights on it. Here the patch is worked out for the turned camera first, the
// vertices laid out over it, the patch worked out again as the game leaves it,
// and the heights set.
void build_turned(uint8_t* rdram, recomp_context* ctx, uint32_t camera, float angle, uint32_t view_output) {
    turn_camera(rdram, camera, angle);

    ctx->r4 = 0;
    ctx->r5 = int32_t(view_output);
    func_8004C1D0(rdram, ctx);

    // func_8004C998 takes the start of the structure func_8004C1D0's output is
    // a field of; its third argument is unused.
    ctx->r4 = 0;
    ctx->r5 = int32_t(view_output - 0x1C0);
    ctx->r6 = 0;
    func_8004C998(rdram, ctx);

    ctx->r4 = 0;
    ctx->r5 = int32_t(view_output);
    func_8004C1D0(rdram, ctx);

    // func_80050B78 writes the grid's centre through two pointers.
    ctx->r4 = int32_t(camera);
    ctx->r5 = int32_t(kScratch);
    ctx->r6 = int32_t(kScratch + 4);
    func_80050B78(rdram, ctx);

    ctx->r4 = 0;
    ctx->r5 = word_i(rdram, kScratch);
    ctx->r6 = word_i(rdram, kScratch + 4);
    func_80050F8C(rdram, ctx);
}

// ---- merging -------------------------------------------------------------

// The lattice: points 64 apart along x, rows kRow apart along z, odd rows
// offset by half a step. A point is named by its row k and its x in half
// steps, X, which is even on even rows and odd on odd ones.
constexpr float kRow = 55.4256258f;                 // 64 * sqrt(3) / 2
constexpr int kHalfStep = 32;

// The hexagon of the merged mesh: up to kMaxRadius rows from the camera's
// point each way (34 rows is about 1900 units), and the ring of flat quads
// round it out to kFar units -- far enough that the camera of the results
// screen, high above the water, does not see its edge.
constexpr int kMaxRadius = 34;
constexpr float kFar = 30000.0f;

// A point of the game's lattice, with what the mesh gives it.
struct LatticePoint { int16_t y, s, t; uint32_t rgba; int depth; };

int64_t key_of(int k, int X) { return (int64_t(k) << 32) | uint32_t(X); }
int row_of(int64_t key) { return int(key >> 32); }
int column_of(int64_t key) { return int(int32_t(uint32_t(key))); }

int16_t half_at(const uint8_t* block, uint32_t offset) {
    return *reinterpret_cast<const int16_t*>(block + (offset ^ 2));
}

// One mesh's lattice points, from a copy of a frame buffer's mesh area
// (kMeshStart and on), each with its depth: how many points it lies inside
// the hexagon's border, which is flattened to the water level.
std::unordered_map<int64_t, LatticePoint> lattice_of(const std::vector<uint8_t>& mesh, uint32_t end) {
    std::unordered_map<int64_t, LatticePoint> points;
    for (uint32_t v = kLatticeStart - kMeshStart; v + 16 <= end - kMeshStart; v += 16) {
        const int x = half_at(mesh.data(), v), z = half_at(mesh.data(), v + 4);
        // The game rounds each build's points on its own, so a mesh can lie a
        // unit or so off the lattice; it is still the same lattice.
        const int k = int(std::lround(float(z) / kRow));
        const int X = int(std::lround(float(x) / float(kHalfStep)));
        if (std::fabs(float(z) - float(k) * kRow) > 2.5f || std::abs(x - X * kHalfStep) > 2) continue;
        if (((X ^ k) & 1) != 0) continue;
        const LatticePoint p{ half_at(mesh.data(), v + 2), half_at(mesh.data(), v + 8), half_at(mesh.data(), v + 10),
                              *reinterpret_cast<const uint32_t*>(mesh.data() + v + 12), 0 };
        points.emplace(key_of(k, X), p);
    }
    // The ring's corners can fall on the lattice by chance: keep only points
    // with a neighbour in their row.
    std::vector<int64_t> lonely;
    for (const auto& [key, p] : points) {
        const int k = row_of(key), X = column_of(key);
        if (!points.count(key_of(k, X - 2)) && !points.count(key_of(k, X + 2))) lonely.push_back(key);
    }
    for (int64_t key : lonely) points.erase(key);

    std::unordered_map<int, std::pair<int, int>> rows;    // k -> lowest and highest X
    int low_k = 1 << 30, high_k = -(1 << 30);
    for (const auto& [key, p] : points) {
        const int k = row_of(key), X = column_of(key);
        auto row = rows.emplace(k, std::make_pair(X, X)).first;
        row->second.first = std::min(row->second.first, X);
        row->second.second = std::max(row->second.second, X);
        low_k = std::min(low_k, k);
        high_k = std::max(high_k, k);
    }
    for (auto& [key, p] : points) {
        const int k = row_of(key), X = column_of(key);
        const auto& row = rows[k];
        p.depth = std::min(std::min(k - low_k, high_k - k), std::min(X - row.first, row.second - X) / 2);
    }
    return points;
}

struct MergeStats { uint32_t points, waves, triangles, loads; int radius; float centre_x, centre_z; };

// Merges the game's mesh and the turned builds' into one, written with the
// list that draws it into the merged area for frame buffer `b`. False when it
// does not fit, which it always should. The meshes' lattices end at
// `lattice_end`.
bool merge(uint8_t* rdram, int b, const std::vector<uint8_t> (&meshes)[1 + kTurns],
           float eye_x, float eye_z, MergeStats& stats, uint32_t lattice_end = kLatticeEnd) {
    // Each lattice point's height and shading, averaged over the meshes that
    // cover it, weighted by depth so that flattened borders count for nothing.
    struct Sum { float weight = 0, y = 0, s = 0, t = 0; };
    std::unordered_map<int64_t, Sum> sums;
    uint32_t rgba = 0;
    bool have_rgba = false;
    const int K0 = int(std::lround(eye_z / kRow));
    int X0 = int(std::lround(eye_x / kHalfStep));
    if (((X0 ^ K0) & 1) != 0) ++X0;
    int radius = 4;
    for (const auto& mesh : meshes) {
        for (const auto& [key, p] : lattice_of(mesh, lattice_end)) {
            const int dk = std::abs(row_of(key) - K0), dX = std::abs(column_of(key) - X0);
            radius = std::max(radius, std::max(dk, (dX + dk + 1) / 2) + 1);
            if (!have_rgba) { rgba = p.rgba; have_rgba = true; }
            if (p.depth < 1) continue;
            Sum& sum = sums[key];
            const float w = float(p.depth);
            sum.weight += w; sum.y += w * p.y; sum.s += w * p.s; sum.t += w * p.t;
        }
    }
    if (!have_rgba) return false;
    radius = std::min(radius, kMaxRadius);

    // The points: the hexagon's lattice, its border flat, then the ring's six
    // far corners. Rows are numbered from the camera's point, dk, and each is
    // a point shorter at both ends for each row away, which makes the sides
    // straight lines of the lattice.
    struct Point { int16_t x, y, z, s, t; };
    std::vector<Point> points;
    std::unordered_map<int64_t, uint32_t> index;
    uint32_t waves = 0;
    for (int dk = -radius; dk <= radius; ++dk) {
        const int k = K0 + dk, half = 2 * radius - std::abs(dk);
        const int16_t z = int16_t(std::lround(float(k) * kRow));
        for (int X = X0 - half; X <= X0 + half; X += 2) {
            Point p{ int16_t(X * kHalfStep), 0, z, 1024, 1024 };
            const bool border = std::abs(dk) == radius || X == X0 - half || X == X0 + half;
            const auto it = sums.find(key_of(k, X));
            if (!border && it != sums.end()) {
                const Sum& sum = it->second;
                p.y = int16_t(std::lround(sum.y / sum.weight));
                p.s = int16_t(std::lround(sum.s / sum.weight));
                p.t = int16_t(std::lround(sum.t / sum.weight));
                ++waves;
            }
            index.emplace(key_of(k, X), uint32_t(points.size()));
            points.push_back(p);
        }
    }
    const uint32_t lattice_points = uint32_t(points.size());
    auto at = [&](int k, int X) -> int64_t {
        const auto it = index.find(key_of(k, X));
        return it == index.end() ? -1 : int64_t(it->second);
    };

    // The triangles between each row and the next: for each point, the one
    // below it and the one below and to its right. Where the next row is
    // wider or narrower by a point at each end, these still cover the strip.
    std::vector<std::array<uint32_t, 3>> triangles;
    for (int dk = -radius; dk < radius; ++dk) {
        const int k = K0 + dk, half = 2 * radius - std::abs(dk);
        for (int X = X0 - half; X <= X0 + half; X += 2) {
            const int64_t a = at(k, X), below_left = at(k + 1, X - 1), below_right = at(k + 1, X + 1);
            const int64_t right = at(k, X + 2);
            if (below_left >= 0 && below_right >= 0) {
                triangles.push_back({ uint32_t(a), uint32_t(below_left), uint32_t(below_right) });
            }
            if (below_right >= 0 && right >= 0) {
                triangles.push_back({ uint32_t(a), uint32_t(below_right), uint32_t(right) });
            }
        }
    }
    // The ring: the hexagon's six corners in order, each side walked from one
    // to the next and fanned to the two far corners beyond it.
    const int corner_k[6] = { 0, radius, radius, 0, -radius, -radius };
    const int corner_X[6] = { 2 * radius, radius, -radius, -2 * radius, -radius, radius };
    uint32_t far_corner[6];
    for (int c = 0; c < 6; ++c) {
        const float dx = float(corner_X[c] * kHalfStep), dz = float(corner_k[c]) * kRow;
        const float scale = kFar / std::sqrt(dx * dx + dz * dz);
        const float fx = float(X0 * kHalfStep) + dx * scale, fz = float(K0) * kRow + dz * scale;
        far_corner[c] = uint32_t(points.size());
        points.push_back({ int16_t(std::clamp(fx, -32000.0f, 32000.0f)), 0,
                           int16_t(std::clamp(fz, -32000.0f, 32000.0f)), 1024, 1024 });
    }
    for (int c = 0; c < 6; ++c) {
        const int n = (c + 1) % 6;
        const int step_k = (corner_k[n] - corner_k[c]) / radius, step_X = (corner_X[n] - corner_X[c]) / radius;
        std::vector<uint32_t> side;
        for (int i = 0; i <= radius; ++i) {
            const int64_t p = at(K0 + corner_k[c] + i * step_k, X0 + corner_X[c] + i * step_X);
            if (p < 0) return false;
            side.push_back(uint32_t(p));
        }
        const int middle = radius / 2;
        for (int i = 0; i < radius; ++i) {
            triangles.push_back({ side[i], side[i + 1], i < middle ? far_corner[c] : far_corner[n] });
        }
        triangles.push_back({ side[middle], far_corner[n], far_corner[c] });
    }
    // All one way round, as the game's are (clockwise seen from above, in x
    // and z), in case faces are culled.
    for (auto& tri : triangles) {
        const Point &p = points[tri[0]], &q = points[tri[1]], &r = points[tri[2]];
        const float area = float(q.x - p.x) * float(r.z - p.z) - float(q.z - p.z) * float(r.x - p.x);
        if (area > 0.0f) std::swap(tri[1], tri[2]);
    }

    // Written out in batches of at most kBatch distinct points, each loaded
    // into the vertex cache together before its triangles are drawn. The
    // microcode names a cache entry by five times its number, in a byte, so
    // only the first fifty or so can be named.
    constexpr uint32_t kBatch = 48;
    const uint32_t base = kMerged + uint32_t(b) * kMergedStride;
    uint32_t vertex_count = 0, command_count = 0, loads = 0;
    auto command = [&](uint32_t w0, uint32_t w1) -> bool {
        if (command_count >= kMergedCommands) return false;
        uint32_t* out = reinterpret_cast<uint32_t*>(rdram + base + kMergedList + command_count * 8);
        out[0] = w0;
        out[1] = w1;
        ++command_count;
        return true;
    };
    auto put_half = [&](uint32_t address, int16_t value) {
        *reinterpret_cast<int16_t*>(rdram + (address ^ 2)) = value;
    };
    std::vector<uint32_t> batch;                            // point indices, by cache entry
    std::vector<std::array<uint32_t, 3>> batch_triangles;   // cache entries
    auto flush = [&]() -> bool {
        if (batch.empty()) return true;
        if (vertex_count + batch.size() > kMergedVertices) return false;
        const uint32_t first = vertex_count;
        for (uint32_t i : batch) {
            const Point& p = points[i];
            const uint32_t v = base + vertex_count * 16;
            put_half(v + 0, p.x); put_half(v + 2, p.y); put_half(v + 4, p.z); put_half(v + 6, 0);
            put_half(v + 8, p.s); put_half(v + 10, p.t);
            *reinterpret_cast<uint32_t*>(rdram + v + 12) = rgba;
            ++vertex_count;
        }
        const uint32_t n = uint32_t(batch.size());
        // G_VTX: the count at bit 9 with the byte length less one below it,
        // the first cache entry (times five) at bit 16, and the address
        // through segment 3, which the rewriter points here.
        if (!command((0x04u << 24) | (n << 9) | (n * 16 - 1), 0x03000000u | (first * 16))) return false;
        ++loads;
        for (const auto& tri : batch_triangles) {
            if (!command(0xBFu << 24, ((tri[0] * 5) << 16) | ((tri[1] * 5) << 8) | (tri[2] * 5))) return false;
        }
        batch.clear();
        batch_triangles.clear();
        return true;
    };
    for (const auto& tri : triangles) {
        uint32_t needed = 0;
        for (uint32_t i : tri) {
            if (std::find(batch.begin(), batch.end(), i) == batch.end()) ++needed;
        }
        if (batch.size() + needed > kBatch && !flush()) return false;
        std::array<uint32_t, 3> entries{};
        for (int j = 0; j < 3; ++j) {
            auto it = std::find(batch.begin(), batch.end(), tri[j]);
            if (it == batch.end()) {
                batch.push_back(tri[j]);
                it = batch.end() - 1;
            }
            entries[j] = uint32_t(it - batch.begin());
        }
        batch_triangles.push_back(entries);
    }
    if (!flush() || !command(0xB8u << 24, 0)) return false;

    stats.points = lattice_points;
    stats.waves = waves;
    stats.triangles = uint32_t(triangles.size());
    stats.loads = loads;
    stats.radius = radius;
    stats.centre_x = float(X0 * kHalfStep);
    stats.centre_z = float(K0) * kRow;
    return true;
}

// The copies, built and merged: the game's mesh is copied out; for each of
// kTurns turns, everything kept is put back as it was, the game's placing and
// building run for the camera turned that far, and the mesh is copied out of
// the frame buffer it went into; then everything is put back once more,
// registers included, and the meshes are merged.
void build_copies(uint8_t* rdram, recomp_context* ctx, uint32_t camera) {
    static std::vector<uint8_t> kept;
    size_t total = kCameraSize + 2 * kMeshSize;
    for (const Range& range : kKept) total += range.size;
    kept.resize(total);

    auto keep = [&]() {
        uint8_t* out = kept.data();
        std::memcpy(out, rdram + physical(camera), kCameraSize); out += kCameraSize;
        for (uint32_t buffer : kFrameBuffers) {
            std::memcpy(out, rdram + buffer + kMeshStart, kMeshSize); out += kMeshSize;
        }
        for (const Range& range : kKept) {
            std::memcpy(out, rdram + range.start, range.size); out += range.size;
        }
    };
    auto put_back = [&]() {
        const uint8_t* in = kept.data();
        std::memcpy(rdram + physical(camera), in, kCameraSize); in += kCameraSize;
        for (uint32_t buffer : kFrameBuffers) {
            std::memcpy(rdram + buffer + kMeshStart, in, kMeshSize); in += kMeshSize;
        }
        for (const Range& range : kKept) {
            std::memcpy(rdram + range.start, in, range.size); in += range.size;
        }
    };
    // The frame buffer a build wrote its mesh into: the one whose mesh changed.
    auto changed_buffer = [&]() -> int {
        const uint8_t* before = kept.data() + kCameraSize;
        for (int b = 0; b < 2; ++b) {
            if (std::memcmp(rdram + kFrameBuffers[b] + kMeshStart, before + b * kMeshSize, kMeshSize) != 0) {
                return b;
            }
        }
        return -1;
    };

    const recomp_context saved = *ctx;
    keep();

    const float eye_x = word_f(rdram, camera + 0x4C), eye_z = word_f(rdram, camera + 0x54);
    g_copy_camera[0] = eye_x;
    g_copy_camera[1] = eye_z;
    g_copy_camera[2] = std::atan2(word_f(rdram, camera + 0x64), -word_f(rdram, camera + 0x6C));

    // The frame buffer being built, which the turned builds write into -- and
    // which must be the one whose mesh changed.
    const uint32_t pool = physical(uint32_t(word_i(rdram, kGfxPool)));
    const int pool_index = pool == kFrameBuffers[0] ? 0 : pool == kFrameBuffers[1] ? 1 : -1;
    if (pool_index < 0 || g_view_outputs[pool_index] == 0) {
        g_copies_ready[0] = g_copies_ready[1] = false;
        *ctx = saved;
        return;
    }

    // The meshes: the game's own, then each turned build's, copied out.
    static std::vector<uint8_t> meshes[1 + kTurns];
    bool built = true;
    for (int turn = 0; turn <= kTurns && built; ++turn) {
        if (turn > 0) {
            put_back();
            build_turned(rdram, ctx, camera, 1.5707963f * float(turn), g_view_outputs[pool_index]);
            built = changed_buffer() == pool_index;
        }
        const uint8_t* mesh = rdram + kFrameBuffers[pool_index] + kMeshStart;
        meshes[turn].assign(mesh, mesh + kMeshSize);
    }
    put_back();
    *ctx = saved;

    MergeStats stats{};
    if (built) {
        built = merge(rdram, pool_index, meshes, eye_x, eye_z, stats);
    }
    g_copies_ready[pool_index] = built;
    if (built) {
        g_built_for[pool_index] = { { eye_x, word_f(rdram, camera + 0x50), eye_z },
                                    stats.centre_x, stats.centre_z, float(stats.radius * 2 * kHalfStep) };
    }
    if (!built) g_copies_ready[pool_index ^ 1] = false;

    static bool reported = false;
    if (!reported) {
        reported = true;
        if (built) {
            std::fprintf(stderr, "[wr64-water] the wave mesh is built %d more times, turned round the camera, and"
                                 " merged: %u lattice points (%u with waves), %d rows each way, %u triangles,"
                                 " %u vertex loads\n",
                         kTurns, stats.points, stats.waves, stats.radius, stats.triangles, stats.loads);
        }
        else {
            std::fprintf(stderr, "[wr64-water] a turned build did not write a mesh where expected, or the merge"
                                 " did not fit; the game's own water is drawn\n");
        }
        std::fflush(stderr);
    }
}

// WR64_WATER_DIFF=1: every tenth of a minute or so of racing, the game's whole
// 4 MB before the copies against after them, so anything they leave changed
// is reported. The copies themselves live above it. Measured with
// tools/scripts/race.txt: nothing is left changed but what lies below the game
// thread's stack pointer, and now and then parts of the audio heap, which the
// audio thread writes meanwhile -- checked by tracing each call, none of which
// touches it -- and which must therefore never be put back.
void build_copies_checked(uint8_t* rdram, recomp_context* ctx, uint32_t camera) {
    static uint32_t frames = 0;
    if (frames++ % 200 != 0) {
        build_copies(rdram, ctx, camera);
        return;
    }
    constexpr size_t kGameMemory = 0x400000;
    static std::vector<uint8_t> before;
    before.assign(rdram, rdram + kGameMemory);
    // WR64_WATER_DIFF=2 is the control: the same comparison with no copies
    // built in between, so what other threads change meanwhile can be told
    // from what the copies do.
    static const bool control = std::getenv("WR64_WATER_DIFF")[0] == '2';
    if (control) {
        const auto start = std::chrono::steady_clock::now();
        build_copies(rdram, ctx, camera);
        const auto took = std::chrono::steady_clock::now() - start;
        before.assign(rdram, rdram + kGameMemory);
        const auto until = std::chrono::steady_clock::now() + took;
        while (std::chrono::steady_clock::now() < until) {}
    }
    else {
        build_copies(rdram, ctx, camera);
    }
    // Each run of changes, allowing gaps of up to 32 unchanged bytes inside it.
    std::string ranges;
    size_t changed = 0;
    for (size_t i = 0; i < kGameMemory;) {
        if (rdram[i] == before[i]) { ++i; continue; }
        size_t end = i + 1, quiet = 0;
        while (end < kGameMemory && quiet <= 32) {
            if (rdram[end] != before[end]) quiet = 0; else ++quiet;
            ++end;
        }
        end -= quiet;
        char cell[40];
        std::snprintf(cell, sizeof(cell), " 0x%06zX+%zu", i, end - i);
        ranges += cell;
        changed += end - i;
        i = end;
    }
    // Below the game thread's stack pointer is only what calls left behind.
    const uint32_t stack = physical(uint32_t(ctx->r29));
    if (changed == 0) {
        std::fprintf(stderr, "[wr64-water] check: the copies left the game's memory as it was\n");
    }
    else {
        std::fprintf(stderr, "[wr64-water] check: the copies left changes (start+length; stack at 0x%06X):%s\n",
                     stack, ranges.c_str());
    }
    std::fflush(stderr);
}

}  // namespace

// For the display-list rewriter: the merged mesh for the frame buffer that
// segment 3 now points at -- the segment base its list expects, and the
// list's address through segment 3 -- or false when there is none.
bool water_merged(uint32_t segment3, uint32_t* base, uint32_t* list) {
    for (int b = 0; b < 2; ++b) {
        if (segment3 != kFrameBuffers[b] || !g_copies_ready[b]) continue;
        *base = kMerged + uint32_t(b) * kMergedStride;
        *list = 0x03000000u | kMergedList;
        return true;
    }
    return false;
}

// For the display-list rewriter, VR only: the full grid of waves for the
// frame buffer that segment 3 now points at, merged on its own -- the same
// mesh given four times -- round its centre, which gives it the ring of flat
// quads out to the distance that it lacks: on the results screen the camera
// looks down on it from high above and saw its edge. Written into the merged
// area for that buffer, in place of the race's merged mesh, and given to the
// sky as that is, with the camera; the segment base its list expects and the
// list, as water_merged gives them, or false.
bool water_full_grid_ringed(uint8_t* rdram, uint32_t segment3, uint32_t* base, uint32_t* list) {
    const int b = segment3 == kFrameBuffers[0] ? 0 : segment3 == kFrameBuffers[1] ? 1 : -1;
    if (b < 0) return false;
    g_copies_ready[b] = false;
    static std::vector<uint8_t> meshes[1 + kTurns];
    const uint8_t* mesh = rdram + segment3 + kMeshStart;
    for (auto& copy : meshes) copy.assign(mesh, mesh + kMeshSize);
    const auto points = lattice_of(meshes[0], kFullGridEnd);
    if (points.empty()) return false;
    float sum_x = 0.0f, sum_z = 0.0f;
    for (const auto& [key, p] : points) {
        sum_x += float(column_of(key) * kHalfStep);
        sum_z += float(row_of(key)) * kRow;
    }
    MergeStats stats{};
    if (!merge(rdram, b, meshes, sum_x / float(points.size()), sum_z / float(points.size()), stats, kFullGridEnd)) {
        return false;
    }
    // The camera, for the sky (patches/sky_all_round.cpp): where the game's
    // view matrix, which it builds beside its projection at +0xE088, puts the
    // origin of its view. Row vectors, a turn and then a move t, so the eye
    // is -t times the turn's transpose.
    float view[4][4];
    for (int i = 0; i < 16; ++i) {
        const uint32_t at = segment3 + kViewMatrix;
        const int16_t whole = *reinterpret_cast<const int16_t*>(rdram + ((at + i * 2) ^ 2));
        const uint16_t part = *reinterpret_cast<const uint16_t*>(rdram + ((at + 32 + i * 2) ^ 2));
        view[i / 4][i % 4] = float(whole) + float(part) / 65536.0f;
    }
    float eye[3];
    for (int j = 0; j < 3; ++j) {
        eye[j] = -(view[3][0] * view[j][0] + view[3][1] * view[j][1] + view[3][2] * view[j][2]);
    }
    g_built_for[b] = { { eye[0], eye[1], eye[2] }, stats.centre_x, stats.centre_z,
                       float(stats.radius * 2 * kHalfStep) };
    g_copies_ready[b] = true;

    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr, "[wr64-water] the full grid is ringed: %u lattice points, %d rows each way; the camera"
                             " at %.0f %.0f %.0f\n", stats.points, stats.radius, eye[0], eye[1], eye[2]);
        std::fflush(stderr);
    }
    *base = kMerged + uint32_t(b) * kMergedStride;
    *list = 0x03000000u | kMergedList;
    return true;
}

// For the sky: the camera the merged mesh for the frame buffer that segment 3
// points at was built for, and the centre and corner distance of its
// hexagon, whose corners point along +x and every 60 degrees from it.
bool water_built_for(uint32_t segment3, float eye[3], float* centre_x, float* centre_z, float* radius) {
    for (int b = 0; b < 2; ++b) {
        if (segment3 != kFrameBuffers[b] || !g_copies_ready[b]) continue;
        const BuiltFor& f = g_built_for[b];
        eye[0] = f.eye[0]; eye[1] = f.eye[1]; eye[2] = f.eye[2];
        *centre_x = f.centre_x; *centre_z = f.centre_z; *radius = f.radius;
        return true;
    }
    return false;
}

void water_copy_camera(float* eye_x, float* eye_z, float* heading) {
    *eye_x = g_copy_camera[0]; *eye_z = g_copy_camera[1]; *heading = g_copy_camera[2];
}

// func_8004C1D0, while a frame is drawn: remember what it was given, then run it.
void water_view_hook(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r4 == 0) {
        const uint32_t pool = physical(uint32_t(word_i(rdram, kGfxPool)));
        for (int b = 0; b < 2; ++b) {
            if (pool == kFrameBuffers[b]) g_view_outputs[b] = uint32_t(ctx->r5);
        }
    }
    func_8004C1D0(rdram, ctx);
}

// func_80051238, the water's update: the game's own first, then the copies --
// one player only, when objects are drawn all round (wr64::vr::all_round).
void water_update_hook(uint8_t* rdram, recomp_context* ctx) {
    func_80051238(rdram, ctx);

    const int32_t index = word_i(rdram, kCameraIndex);
    if (!vr::all_round() || word_i(rdram, kPlayers) != 1 || index < 0 || index > 3) {
        g_copies_ready[0] = g_copies_ready[1] = false;
        return;
    }
    const uint32_t camera = kCameras + uint32_t(index) * kCameraSize;
    static const bool checking = std::getenv("WR64_WATER_DIFF") != nullptr;
    if (checking) build_copies_checked(rdram, ctx, camera);
    else build_copies(rdram, ctx, camera);
}

}  // namespace wr64

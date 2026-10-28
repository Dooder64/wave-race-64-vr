// More, and more varied, fish under Dolphin Park.
//
// The course's fish are three kinds (textures 0x0806C480, 0x0806D480 and
// 0x0806DC80 in segment 8), each fish drawn at the frame's top level as a
// matrix load from the frame's segment 6 -- the fish's place and heading --
// then its three flat pieces, head, body and tail: for each, a matrix
// multiplied and pushed, a call of the piece's list (in segment 8, from
// 0x0806EC80 to 0x0806F100: its texture, its colour mode, TEXEL0 times the
// vertex colour, a load of four vertices and two triangles), and a pop
// (measured with WR64_3D_TRACE_RAW).
//
// Each fish is drawn twice instead: once where the game has it and once a
// few metres off, swimming alongside -- twice as many fish -- each copy
// with a size, a shape, a depth and a colour of its own. The colour comes
// from copies of the pieces' lists whose vertices are tinted (multiplying the
// texture by the vertex colour, as the lists do anyway); size, shape and
// depth from the fish's matrix, scaled, stretched and moved, written beside
// them. A fish's looks follow from its matrix's place in segment 6, which
// stays its own while the course runs. No fish is put above the waves or
// into the seabed.
//
// The game's fish keep to a few schools, so away from them the water was
// empty. The water within kFieldRadius of the camera is stocked as well:
// it is cut into cells, and each cell, by a number that follows from where
// it is, holds a school or not -- a few fish of one kind, colour and shape,
// swimming round together at a depth of their own. Their kinds are the
// game's three, copied from the game's own fish as they are drawn (the
// pieces' lists, and the pieces' matrices, which wag the tail), so the
// schools appear once the game has drawn a fish of each kind.
//
// A fish is long along its matrix's z, tall along y and flat across x (its
// pieces are side by side along z, measured with WR64_3D_TRACE_VERTS).
//
// Memory: the tinted lists at 0x690000 (built once per load of the course's
// segment 8), the matrices for each frame buffer at 0x680000 + 0x8000 b,
// the schools' vertices at 0x6C0000 + 0x20000 b.
//
// WR64_FISH=0 leaves the fish alone; WR64_FISH_LOG=1 reports how many are drawn;
// WR64_FISH_RADIUS=<units> stocks the water that far round the camera instead.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wr64 {

// src/seafloor.cpp: the seafloor's height, on a course with one.
bool seafloor_height_at(uint32_t course, float x, float z, float* height);
// patches/water_all_round.cpp: the camera the frame's waves were merged round.
bool water_built_for(uint32_t segment3, float eye[3], float* centre_x, float* centre_z, float* radius);

namespace {

constexpr uint32_t kMatrices = 0x680000;
constexpr uint32_t kMatrixStride = 0x8000;
constexpr uint32_t kMostMatrices = kMatrixStride / 64;
constexpr uint32_t kField = 0x6C0000;
constexpr uint32_t kFieldStride = 0x20000;
constexpr uint32_t kFieldVertices = kFieldStride / 16;
// The most vertices loaded at once for the schools (the seafloor loads 25).
constexpr uint32_t kBatchVertices = 24;
constexpr uint32_t kTinted = 0x690000;
constexpr uint32_t kTintedSlot = 0x200;
constexpr uint32_t kTintedEnd = 0x6C0000;
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };
constexpr uint32_t kPiecesFirst = 0x0806EC80, kPiecesEnd = 0x0806F100;

// The tints: the texture's own colours, then golds, reds, pinks, violets,
// blues, greens and a dark silver.
constexpr uint8_t kTints[][3] = {
    { 255, 255, 255 }, { 255, 210, 90 }, { 255, 150, 70 }, { 255, 95, 90 }, { 255, 140, 215 },
    { 185, 125, 255 }, { 110, 160, 255 }, { 100, 235, 255 }, { 130, 255, 140 }, { 215, 255, 90 },
    { 150, 150, 175 },
};
constexpr uint32_t kTintCount = sizeof(kTints) / sizeof(kTints[0]);

// Sizes from this to this times the game's; the depth below the surface
// (units) from this to this; the second fish this far off (units).
constexpr float kSmallest = 0.6f, kLargest = 1.8f;
constexpr float kShallowest = 40.0f, kDeepest = 320.0f;
constexpr float kNearest = 150.0f, kFarthest = 420.0f;
// The game's fish swim about this deep.
constexpr float kGameDepth = 130.0f;
// Stretched along their length and their height from this to this.
constexpr float kShortest = 0.7f, kLongest = 1.6f;
constexpr float kFlattest = 0.65f, kTallest = 1.45f;

// The schools: in cells this wide, as far as this from the camera, this
// many cells in a hundred holding one; as deep as this at most.
constexpr float kFieldCell = 360.0f, kFieldRadius = 2000.0f;
constexpr float kFieldChance = 0.55f;
constexpr float kFieldDeepest = 600.0f;
// Which way along its z a fish swims: measured from the game's own (WR64_FISH_LOG).
constexpr float kForward = 1.0f;

uint32_t word(const uint8_t* rdram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, rdram + (address & 0x7FFFFF), 4);
    return value;
}
void put_word(uint8_t* rdram, uint32_t address, uint32_t value) {
    std::memcpy(rdram + (address & 0x7FFFFF), &value, 4);
}
uint16_t half(const uint8_t* rdram, uint32_t address) {
    uint16_t value;
    std::memcpy(&value, rdram + ((address & 0x7FFFFF) ^ 2), 2);
    return value;
}

void put_half(uint8_t* rdram, uint32_t address, float value) {
    const int16_t whole = int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
    std::memcpy(rdram + ((address & 0x7FFFFF) ^ 2), &whole, 2);
}

struct Matrix { float m[4][4]; };
Matrix read_matrix(const uint8_t* rdram, uint32_t address) {
    Matrix r{};
    for (int i = 0; i < 16; ++i) {
        const uint32_t full = (uint32_t(half(rdram, address + 2 * i)) << 16) | half(rdram, address + 32 + 2 * i);
        r.m[i / 4][i % 4] = float(int32_t(full)) / 65536.0f;
    }
    return r;
}
void write_matrix(uint8_t* rdram, uint32_t address, const Matrix& matrix) {
    for (int i = 0; i < 16; ++i) {
        const double value = std::clamp(double(matrix.m[i / 4][i % 4]), -32768.0, 32767.99);
        const int32_t fixed = int32_t(std::lround(value * 65536.0));
        const int16_t high = int16_t(fixed >> 16);
        const uint16_t low = uint16_t(fixed & 0xFFFF);
        std::memcpy(rdram + ((address + i * 2) ^ 2), &high, 2);
        std::memcpy(rdram + ((address + 32 + i * 2) ^ 2), &low, 2);
    }
}

// A number from 0 to 1 that follows from `key` alone.
float chance(uint32_t key) {
    key ^= key >> 16; key *= 0x7FEB352Du;
    key ^= key >> 15; key *= 0x846CA68Bu;
    key ^= key >> 16;
    return float(key >> 8) / 16777216.0f;
}

// A kind of fish as the game last drew one, for the schools: the commands
// setting up its texture (the same for each of its pieces), and each piece's
// matrix (which the game moves, wagging the tail), vertices and triangles;
// and its matrix's scale.
struct Vertex { float x, y, z; int16_t s, t; uint8_t rgba[4]; };
struct Piece {
    Matrix matrix{};
    std::vector<Vertex> vertices;
    std::vector<std::array<uint8_t, 3>> triangles;
};
struct Kind {
    uint32_t key = 0;                                  // its first piece's list
    std::vector<std::pair<uint32_t, uint32_t>> setup;
    std::vector<Piece> pieces;
    float scale = 1.0f;
};

// Piece list `list` (segmented, in segment 8) read into `piece`, and its
// texture's setup into `setup` if that is empty; false if it is not the
// kind of list measured.
bool read_piece(const uint8_t* rdram, const uint32_t segments[16], uint32_t list, Piece& piece,
                std::vector<std::pair<uint32_t, uint32_t>>& setup) {
    const bool want_setup = setup.empty();
    piece.vertices.clear();
    piece.triangles.clear();
    const uint32_t at = (segments[8] + (list & 0xFFFFFF)) & 0x7FFFFF;
    for (uint32_t i = 0; i < 31; ++i) {
        const uint32_t w0 = word(rdram, at + i * 8), w1 = word(rdram, at + i * 8 + 4);
        const uint8_t op = uint8_t(w0 >> 24);
        if (op == 0xB8) return !piece.vertices.empty() && !piece.triangles.empty();
        if (op == 0x04) {
            const uint32_t count = (w0 >> 9) & 0x7F;
            if ((w1 >> 24) != 0x08 || !piece.vertices.empty() || count > 16) return false;
            const uint32_t from = (segments[8] + (w1 & 0xFFFFFF)) & 0x7FFFFF;
            for (uint32_t v = 0; v < count; ++v) {
                const uint32_t a = from + v * 16;
                Vertex vertex{ float(int16_t(half(rdram, a))), float(int16_t(half(rdram, a + 2))),
                               float(int16_t(half(rdram, a + 4))), int16_t(half(rdram, a + 8)),
                               int16_t(half(rdram, a + 10)), {} };
                for (int c = 0; c < 4; ++c) vertex.rgba[c] = rdram[(a + 12 + c) ^ 3];
                piece.vertices.push_back(vertex);
            }
        } else if (op == 0xBF) {
            const uint8_t a = uint8_t(((w1 >> 16) & 0xFF) / 5), b = uint8_t(((w1 >> 8) & 0xFF) / 5), c = uint8_t((w1 & 0xFF) / 5);
            if (std::max({ a, b, c }) >= piece.vertices.size()) return false;
            piece.triangles.push_back({ a, b, c });
        } else if (piece.vertices.empty()) {
            if (want_setup) setup.emplace_back(w0, w1);
        } else {
            return false;
        }
    }
    return false;
}

// a times b, rows times columns.
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
        }
    }
    return r;
}

struct State {
    uint32_t segment8 = 0;
    std::unordered_map<uint32_t, uint32_t> tinted;     // a piece's list -> its tinted copies' first slot
    uint32_t next_slot = kTinted;
    uint32_t segment3 = 0, matrices = 0;
    std::vector<Kind> kinds;
    uint32_t field_frames = 0, field_drawn = 0;
} g;

// The frame buffer `segment3` is, or -1.
int frame_buffer(uint32_t segment3) {
    for (int i = 0; i < 2; ++i) {
        if (segment3 == kFrameBuffers[i]) return i;
    }
    return -1;
}

// `m` stretched along its fish's length and height, as `key` has it.
void stretch(Matrix& m, uint32_t key) {
    const float length = kShortest + (kLongest - kShortest) * chance(key ^ 0x2Eu);
    const float height = kFlattest + (kTallest - kFlattest) * chance(key ^ 0x4Fu);
    for (int c = 0; c < 3; ++c) {
        m.m[1][c] *= height;
        m.m[2][c] *= length;
    }
}

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_FISH");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

// The address of piece list `piece` (segmented) tinted `tint`, or 0.
uint32_t tinted(uint8_t* rdram, const uint32_t segments[16], uint32_t piece, uint32_t tint) {
    if (segments[8] != g.segment8) {
        g.segment8 = segments[8];
        g.tinted.clear();
        g.next_slot = kTinted;
    }
    auto found = g.tinted.find(piece);
    if (found == g.tinted.end()) {
        if (g.next_slot + kTintCount * kTintedSlot > kTintedEnd) return 0;
        const uint32_t first = g.next_slot;
        g.next_slot += kTintCount * kTintedSlot;
        const uint32_t source = (segments[8] + (piece & 0xFFFFFF)) & 0x7FFFFF;
        for (uint32_t t = 0; t < kTintCount; ++t) {
            const uint32_t slot = first + t * kTintedSlot;
            const uint32_t vertices = slot + 0x100;
            uint32_t at = slot, copied = 0;
            bool ok = false;
            for (uint32_t i = 0; i < 31; ++i) {
                uint32_t w0 = word(rdram, source + i * 8), w1 = word(rdram, source + i * 8 + 4);
                if ((w0 >> 24) == 0x04) {
                    const uint32_t count = (w0 >> 9) & 0x7F;
                    if ((w1 >> 24) != 0x08 || copied + count > 16) break;
                    const uint32_t from = (segments[8] + (w1 & 0xFFFFFF)) & 0x7FFFFF;
                    for (uint32_t v = 0; v < count; ++v) {
                        const uint32_t to = vertices + (copied + v) * 16;
                        std::memcpy(rdram + to, rdram + from + v * 16, 16);
                        // The colour's red, green and blue (bytes 12 to 14, XOR 3).
                        for (int c = 0; c < 3; ++c) {
                            uint8_t& channel = rdram[(to + 12 + c) ^ 3];
                            channel = uint8_t(channel * kTints[t][c] / 255);
                        }
                    }
                    w1 = 0x80000000u | (vertices + copied * 16);
                    copied += count;
                }
                put_word(rdram, at, w0);
                put_word(rdram, at + 4, w1);
                at += 8;
                if ((w0 >> 24) == 0xB8) { ok = true; break; }
            }
            if (!ok) { put_word(rdram, slot, 0xB8000000u); put_word(rdram, slot + 4, 0); }
        }
        found = g.tinted.emplace(piece, first).first;
    }
    return 0x80000000u | (found->second + tint * kTintedSlot);
}

}  // namespace

// For the display-list rewriter, at a top-level matrix load (the command at
// physical `at`) on course `course`: if it begins one of Dolphin Park's
// fish, the commands to draw in its place -- the fish twice, varied -- in
// `out`, and in *next where the game's list goes on after it; otherwise
// false. `segments` are the frame's.
bool fish_rewrite(uint8_t* rdram, const uint32_t segments[16], uint32_t course, uint32_t at, uint32_t* next,
                  std::vector<std::pair<uint32_t, uint32_t>>& out) {
    if (!enabled() || course != 0) return false;
    const uint32_t load0 = word(rdram, at), load1 = word(rdram, at + 4);
    if ((load1 >> 24) != 0x06) return false;
    // The group: pushes, piece calls and pops, at least one piece.
    std::vector<std::pair<uint32_t, uint32_t>> group;
    bool fish = false;
    uint32_t pc = at + 8;
    for (int i = 0; i < 24; ++i, pc += 8) {
        const uint32_t w0 = word(rdram, pc), w1 = word(rdram, pc + 4);
        const uint8_t op = uint8_t(w0 >> 24);
        const bool push = op == 0x01 && ((w0 >> 16) & 0xFF) == 0x04;
        const bool piece = op == 0x06 && ((w0 >> 16) & 0xFF) == 0 && w1 >= kPiecesFirst && w1 < kPiecesEnd;
        if (!push && !piece && op != 0xBD) break;
        fish = fish || piece;
        group.emplace_back(w0, w1);
    }
    if (!fish) return false;

    const int b = frame_buffer(segments[3]);
    if (b < 0) return false;
    static const bool log = std::getenv("WR64_FISH_LOG") != nullptr;
    if (segments[3] != g.segment3) {
        static int frames = 0;
        if (log && frames++ % 60 == 0) {
            std::fprintf(stderr, "[wr64-fish] %u of the game's fish drawn last frame, %u in schools, %zu kinds\n",
                         g.matrices, g.field_drawn, g.kinds.size());
            std::fflush(stderr);
        }
        g.segment3 = segments[3];
        g.matrices = 0;
    }
    if (g.matrices + 2 > kMostMatrices) return false;

    const Matrix game = read_matrix(rdram, (segments[6] + (load1 & 0xFFFFFF)) & 0x7FFFFF);
    const uint32_t key = load1 & 0xFFFFFF;

    // Its kind, kept for the schools: the group and the pushed matrices.
    uint32_t first_piece = 0;
    for (const auto& [w0, w1] : group) {
        if ((w0 >> 24) == 0x06 && first_piece == 0) first_piece = w1;
    }
    auto kind = std::find_if(g.kinds.begin(), g.kinds.end(), [&](const Kind& k) { return k.key == first_piece; });
    if (kind == g.kinds.end() && g.kinds.size() < 8) {
        // Its pieces' vertices and triangles, once: the pushes and the
        // calls pair up in order.
        Kind added{ first_piece };
        bool ok = true;
        for (const auto& [w0, w1] : group) {
            if ((w0 >> 24) != 0x06) continue;
            added.pieces.emplace_back();
            ok = ok && read_piece(rdram, segments, w1, added.pieces.back(), added.setup);
        }
        if (ok) kind = g.kinds.insert(g.kinds.end(), added);
    }
    if (kind != g.kinds.end()) {
        size_t p = 0;
        for (const auto& [w0, w1] : group) {
            if ((w0 >> 24) == 0x01 && p < kind->pieces.size()) {
                kind->pieces[p++].matrix = read_matrix(rdram, (segments[6] + (w1 & 0xFFFFFF)) & 0x7FFFFF);
            }
        }
        kind->scale = std::sqrt(game.m[1][0] * game.m[1][0] + game.m[1][1] * game.m[1][1] + game.m[1][2] * game.m[1][2]);
    }

    // Which way the game's fish swim along their z, for kForward.
    if (log) {
        static std::unordered_map<uint32_t, std::array<float, 3>> last;
        static int reported = 0;
        const auto found = last.find(key);
        if (found != last.end() && reported < 40) {
            const float dx = game.m[3][0] - found->second[0], dz = game.m[3][2] - found->second[2];
            const float moved = std::hypot(dx, dz), along = std::hypot(game.m[2][0], game.m[2][2]);
            if (moved > 1.0f && along > 1e-3f) {
                std::fprintf(stderr, "[wr64-fish] fish %06X moved %.1f, along its z %.2f\n", key, moved,
                             (dx * game.m[2][0] + dz * game.m[2][2]) / (moved * along));
                ++reported;
            }
        }
        last[key] = { game.m[3][0], game.m[3][1], game.m[3][2] };
    }
    for (int copy = 0; copy < 2; ++copy) {
        const uint32_t k = key * 2654435761u + uint32_t(copy) * 0x9E3779B9u;
        const float size = kSmallest + (kLargest - kSmallest) * std::pow(chance(k ^ 0x51u), 1.5f);
        const uint32_t tint = copy == 0 && chance(k ^ 0x7Au) < 0.25f ? 0 : uint32_t(chance(k ^ 0x33u) * kTintCount) % kTintCount;
        Matrix m = game;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) m.m[r][c] *= size;
        }
        stretch(m, k);
        if (copy == 1) {
            const float angle = 6.2831853f * chance(k ^ 0xC1u);
            const float distance = kNearest + (kFarthest - kNearest) * chance(k ^ 0xD7u);
            m.m[3][0] += distance * std::cos(angle);
            m.m[3][2] += distance * std::sin(angle);
        }
        // Its depth: its own, kept between just under the waves and just
        // over the seabed, bigger fish clear of both by more.
        const float depth = kShallowest + (kDeepest - kShallowest) * chance(k ^ 0x9Bu);
        float y = m.m[3][1] + kGameDepth - depth;
        float floor = 0.0f;
        if (seafloor_height_at(course, m.m[3][0], m.m[3][2], &floor)) y = std::max(y, floor + 35.0f * size);
        m.m[3][1] = std::min(y, -35.0f * size);
        const uint32_t address = kMatrices + uint32_t(b) * kMatrixStride + g.matrices++ * 64;
        write_matrix(rdram, address, m);
        out.emplace_back(load0, 0x80000000u | address);
        for (const auto& [w0, w1] : group) {
            const bool piece = (w0 >> 24) == 0x06;
            const uint32_t list = piece ? tinted(rdram, segments, w1, tint) : 0;
            out.emplace_back(w0, piece && list != 0 ? list : w1);
        }
    }
    *next = pc;
    return true;
}

// For the display-list rewriter, where the seafloor is drawn: the commands
// drawing the schools round the frame's camera on course `course`, in `out`
// -- none until the game has drawn a fish. `segments` are the frame's.
void fish_field(uint8_t* rdram, const uint32_t segments[16], uint32_t course,
                std::vector<std::pair<uint32_t, uint32_t>>& out) {
    g.field_drawn = 0;
    if (!enabled() || course != 0 || g.kinds.empty()) return;
    const int b = frame_buffer(segments[3]);
    float eye[3], centre_x, centre_z, radius;
    if (b < 0 || !water_built_for(segments[3], eye, &centre_x, &centre_z, &radius)) return;
    const float seconds = float(g.field_frames++) / 30.0f;

    // The state the game draws its fish in (its list at 0x800E6CF8, called
    // just before them): two cycles, fogged, cut out where the texture is
    // clear, depth tested and written, not culled.
    static constexpr std::pair<uint32_t, uint32_t> kFishState[] = {
        { 0xB6000000u, 0xFFFFFFFFu }, { 0xB7000000u, 0x00010205u }, { 0xBB000001u, 0x80008000u },
        { 0xE7000000u, 0x00000000u }, { 0xBA001402u, 0x00100000u }, { 0xB900031Du, 0xC8113078u },
        { 0xFC127FFFu, 0xFFFFF238u }, { 0xBA000E02u, 0x00000000u }, { 0xB9000002u, 0x00000000u },
        { 0xBA001301u, 0x00080000u },
    };
    for (const auto& command : kFishState) out.push_back(command);

    // Every fish of the schools, worked out first and drawn by kind: its
    // matrix and its colour.
    struct Fish { Matrix matrix; uint32_t tint; };
    std::vector<std::vector<Fish>> by_kind(g.kinds.size());

    static const float field_radius = [] {
        const char* value = std::getenv("WR64_FISH_RADIUS");
        return value != nullptr ? float(std::atof(value)) : kFieldRadius;
    }();
    const int reach = int(std::ceil(field_radius / kFieldCell));
    const int eye_x = int(std::floor(eye[0] / kFieldCell)), eye_z = int(std::floor(eye[2] / kFieldCell));
    for (int iz = eye_z - reach; iz <= eye_z + reach; ++iz) {
        for (int ix = eye_x - reach; ix <= eye_x + reach; ++ix) {
            const uint32_t cell = (uint32_t(ix) * 73856093u) ^ (uint32_t(iz) * 19349663u);
            if (chance(cell ^ 0x11u) >= kFieldChance) continue;
            // The school: the circle it swims round, how fast and which way,
            // and where on it it is now.
            const float cx = (float(ix) + chance(cell ^ 0x22u)) * kFieldCell;
            const float cz = (float(iz) + chance(cell ^ 0x23u)) * kFieldCell;
            const float circle = 60.0f + 160.0f * chance(cell ^ 0x24u);
            const float speed = 25.0f + 40.0f * chance(cell ^ 0x25u);
            const float way = chance(cell ^ 0x26u) < 0.5f ? -1.0f : 1.0f;
            const float angle = 6.2831853f * chance(cell ^ 0x27u) + way * speed / circle * seconds;
            const float sx = cx + circle * std::cos(angle), sz = cz + circle * std::sin(angle);
            if (std::hypot(sx - eye[0], sz - eye[2]) > field_radius) continue;
            const float hx = -way * std::sin(angle), hz = way * std::cos(angle);
            // Its fish: one kind, colour and shape; sizes about its own; a
            // depth of its own, in what water there is.
            const size_t k = size_t(chance(cell ^ 0x28u) * float(g.kinds.size())) % g.kinds.size();
            const uint32_t tint = uint32_t(chance(cell ^ 0x29u) * kTintCount) % kTintCount;
            const float size = kSmallest + (kLargest - kSmallest) * std::pow(chance(cell ^ 0x2Au), 1.5f);
            float floor = -1e9f;
            seafloor_height_at(course, sx, sz, &floor);
            const float room = std::min(kFieldDeepest, -floor - 50.0f * size);
            const float depth = kShallowest + std::max(0.0f, room - kShallowest) * chance(cell ^ 0x2Bu);
            const int count = 2 + int(chance(cell ^ 0x2Cu) * 5.0f);
            for (int i = 0; i < count; ++i) {
                const uint32_t f = cell * 2654435761u + uint32_t(i) * 0x9E3779B9u;
                const float along = (chance(f ^ 0x31u) - 0.5f) * 160.0f;
                const float side = (chance(f ^ 0x32u) - 0.5f) * 120.0f;
                const float rise = (chance(f ^ 0x33u) - 0.5f) * 60.0f;
                const float own = size * (0.85f + 0.3f * chance(f ^ 0x34u));
                const float scale = own * g.kinds[k].scale;
                // Its axes: across (y cross z), up, and along its heading.
                const float fx = kForward * hx, fz = kForward * hz;
                Matrix m{};
                m.m[0][0] = fz * scale; m.m[0][2] = -fx * scale;
                m.m[1][1] = scale;
                m.m[2][0] = fx * scale; m.m[2][2] = fz * scale;
                stretch(m, cell);
                const float x = sx + hx * along + hz * side, z = sz + hz * along - hx * side;
                float y = -depth + rise, bottom = 0.0f;
                if (seafloor_height_at(course, x, z, &bottom)) y = std::max(y, bottom + 35.0f * own);
                m.m[3][0] = x;
                m.m[3][1] = std::min(y, -35.0f * own);
                m.m[3][2] = z;
                m.m[3][3] = 1.0f;
                by_kind[k].push_back({ m, tint });
            }
        }
    }

    // Drawn in world space under the floor's identity matrix, a kind at a
    // time: its texture set up once, then its fish a few to a vertex load,
    // each piece's vertices put through the piece's matrix and the fish's,
    // and coloured. One transform and one texture load a kind, where a
    // matrix and a texture load for each piece of each fish cost RT64 so
    // much that the headset fell to a few frames a second.
    const uint32_t base = kField + uint32_t(b) * kFieldStride;
    uint32_t used = 0;
    for (size_t k = 0; k < g.kinds.size(); ++k) {
        const Kind& kind = g.kinds[k];
        if (by_kind[k].empty()) continue;
        uint32_t per_fish = 0;
        for (const Piece& piece : kind.pieces) per_fish += uint32_t(piece.vertices.size());
        if (per_fish == 0 || per_fish > kBatchVertices) continue;
        for (const auto& command : kind.setup) out.push_back(command);
        const uint32_t per_load = kBatchVertices / per_fish;
        for (size_t first = 0; first < by_kind[k].size(); first += per_load) {
            const size_t last = std::min(by_kind[k].size(), first + per_load);
            const uint32_t load = used;
            if (used + uint32_t(last - first) * per_fish > kFieldVertices) break;
            std::vector<std::pair<uint32_t, uint32_t>> triangles;
            for (size_t n = first; n < last; ++n) {
                const Fish& fish = by_kind[k][n];
                const uint8_t* tint = kTints[fish.tint];
                for (const Piece& piece : kind.pieces) {
                    const uint32_t start = used - load;
                    const Matrix m = multiply(piece.matrix, fish.matrix);
                    for (const Vertex& v : piece.vertices) {
                        const uint32_t a = base + used++ * 16;
                        put_half(rdram, a + 0, v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0]);
                        put_half(rdram, a + 2, v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1]);
                        put_half(rdram, a + 4, v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2]);
                        put_half(rdram, a + 6, 0.0f);
                        put_half(rdram, a + 8, float(v.s));
                        put_half(rdram, a + 10, float(v.t));
                        for (int c = 0; c < 3; ++c) rdram[(a + 12 + c) ^ 3] = uint8_t(v.rgba[c] * tint[c] / 255);
                        rdram[(a + 15) ^ 3] = v.rgba[3];
                    }
                    for (const auto& t : piece.triangles) {
                        triangles.emplace_back(0xBF000000u, (((start + t[0]) * 5) << 16) | (((start + t[1]) * 5) << 8) |
                                                             ((start + t[2]) * 5));
                    }
                }
                ++g.field_drawn;
            }
            const uint32_t count = used - load;
            out.emplace_back((0x04u << 24) | (count << 9) | (count * 16 - 1), 0x80000000u | (base + load * 16));
            out.insert(out.end(), triangles.begin(), triangles.end());
        }
    }
}

}  // namespace wr64

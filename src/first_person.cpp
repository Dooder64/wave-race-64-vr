// VR: the race in first person, from the player's rider.
//
// Each rider is drawn by a list of its own (in segment 2) as eighteen parts,
// each a matrix load -- the part's place in the world, absolute -- and a call
// of the part's list. The matrices are written into the frame's buffer behind
// segment 3, rider k's part p at 0xE108 + 0x40 k + 0x100 p, in the same order
// for every character (measured with WR64_3D_TRACE): 0 the jet ski's hull,
// 1 its handlebars, 3 the head and helmet, 4 the chest, 5 the hips, 6 and 12
// the upper arms, and the arms' and legs' other parts after, in pairs. The
// player is rider 0.
//
// So the camera is replaced where the frame is rewritten, after the game has
// built all of it: the game's view, the projection multiply of 0xE088 in
// segment 3, is given in place of the game's chase camera a view from the
// rider's eyes, facing the way the chest faces -- leaning, pitching and
// rolling with it, nothing smoothed -- and the head is shrunk to nothing.
// Built from the same frame's matrices, the view and the body never part.
// The headset's own movement is added after, as for the game's camera.
// Everything the game itself works out from its camera (what it culls, where
// it builds the waves) still follows the chase camera, a few body lengths
// behind, which in VR draws all round it anyway.
//
// In the hull's and the head's own space y is up and z ahead; the chest is
// turned over, -y up, and its z points about 30 degrees above the way the
// rider faces while riding upright (WR64_FP_LOG=1 prints the parts' axes now
// and then). So the view faces along the chest's z turned down by
// WR64_FP_PITCH degrees (30), with its -y up. The eye is placed from the
// head's origin, its neck, in the head's own units: WR64_FP_EYE_UP above and
// WR64_FP_EYE_FORWARD ahead, along those axes.

#include "wr64/vr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wr64 {

namespace {

// The game's two frame buffers, each the base of segment 3 while its frame is
// drawn (as in patches/water_all_round.cpp).
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };

// The game's view matrix, and the player's parts, in either buffer.
constexpr uint32_t kViewMatrix = 0xE088;
constexpr uint32_t kRiderParts = 0xE108;
constexpr uint32_t kPartStride = 0x100;
constexpr uint32_t kHead = 3, kChest = 4, kHull = 0;

// The view written for each frame buffer, in physical memory above the
// merged waves and the sky (0x741000 to 0x7CD000) and below the audio task's
// copy at 0x7E0000.
constexpr uint32_t kView = 0x7D0000;
constexpr uint32_t kViewStride = 0x40;

struct Vec { float x, y, z; };

Vec operator+(Vec a, Vec b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec operator-(Vec a, Vec b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec operator*(Vec a, float s) { return { a.x * s, a.y * s, a.z * s }; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float length(Vec a) { return std::sqrt(dot(a, a)); }

// An N64 matrix: sixteen whole parts, then sixteen fractions, halfwords in
// byte-swapped words as RDRAM holds them. Row vectors: rows 0 to 2 are the
// part's x, y and z axes in the world, row 3 its origin.
struct Matrix { float m[4][4]; };

Matrix read_matrix(const uint8_t* rdram, uint32_t address) {
    Matrix r{};
    for (int i = 0; i < 16; ++i) {
        const int16_t whole = *reinterpret_cast<const int16_t*>(rdram + ((address + i * 2) ^ 2));
        const uint16_t part = *reinterpret_cast<const uint16_t*>(rdram + ((address + 32 + i * 2) ^ 2));
        r.m[i / 4][i % 4] = float(whole) + float(part) / 65536.0f;
    }
    return r;
}

void write_matrix(uint8_t* rdram, uint32_t address, const Matrix& matrix) {
    for (int i = 0; i < 16; ++i) {
        const double value = std::clamp(double(matrix.m[i / 4][i % 4]), -32768.0, 32767.99);
        const int32_t fixed = int32_t(std::lround(value * 65536.0));
        *reinterpret_cast<int16_t*>(rdram + ((address + i * 2) ^ 2)) = int16_t(fixed >> 16);
        *reinterpret_cast<uint16_t*>(rdram + ((address + 32 + i * 2) ^ 2)) = uint16_t(fixed & 0xFFFF);
    }
}

Vec row(const Matrix& m, int r) { return { m.m[r][0], m.m[r][1], m.m[r][2] }; }

float setting(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? float(std::atof(value)) : fallback;
}

// Off, on, or not yet decided (-1): on in VR mode unless WR64_VR_FIRST_PERSON=0,
// and with WR64_VR_FIRST_PERSON=1 without a headset too.
std::atomic<int> g_on{ -1 };

}  // namespace

namespace vr {

bool first_person() {
    int on = g_on.load(std::memory_order_relaxed);
    if (on < 0) {
        const char* value = std::getenv("WR64_VR_FIRST_PERSON");
        on = value != nullptr ? (std::strcmp(value, "0") != 0) : enabled();
        int expected = -1;
        g_on.compare_exchange_strong(expected, on ? 1 : 0, std::memory_order_relaxed);
    }
    return g_on.load(std::memory_order_relaxed) == 1;
}

void toggle_first_person() {
    const bool on = !first_person();
    g_on.store(on ? 1 : 0, std::memory_order_relaxed);
    std::fprintf(stderr, "[wr64-vr] first person %s\n", on ? "on" : "off");
    std::fflush(stderr);
}

}  // namespace vr

// For the display-list rewriter, in a race: the address of a view from the
// player's rider's eyes to multiply onto the projection in place of the
// game's, with the rider's head shrunk away; or 0, leaving the frame as it is.
uint32_t first_person_view(uint8_t* rdram, uint32_t segment3) {
    if (!vr::first_person()) return 0;
    int b = -1;
    for (int i = 0; i < 2; ++i) {
        if (segment3 == kFrameBuffers[i]) b = i;
    }
    if (b < 0) return 0;

    const uint32_t parts = segment3 + kRiderParts;
    const Matrix head = read_matrix(rdram, parts + kHead * kPartStride);
    const Matrix chest = read_matrix(rdram, parts + kChest * kPartStride);

    // The chest's axes, made square, and the head's scale: its own units to
    // the world's. A part that is not a rider's -- nothing drawn there this
    // frame -- has no axes, or a nonsensical scale.
    const float scale = length(row(head, 1));
    Vec up = row(chest, 1) * -1.0f, ahead = row(chest, 2);
    if (!(scale > 1e-4f && scale < 100.0f) || length(up) < 1e-6f || length(ahead) < 1e-6f) return 0;
    ahead = ahead * (1.0f / length(ahead));
    up = up - ahead * dot(up, ahead);
    if (length(up) < 1e-6f) return 0;
    up = up * (1.0f / length(up));
    static const float pitch = setting("WR64_FP_PITCH", 30.0f) * 3.14159265f / 180.0f;
    const Vec level = ahead * std::cos(pitch) - up * std::sin(pitch);
    up = up * std::cos(pitch) + ahead * std::sin(pitch);
    ahead = level;
    const Vec right = cross(ahead, up);

    static const float eye_up = setting("WR64_FP_EYE_UP", 70.0f);
    static const float eye_forward = setting("WR64_FP_EYE_FORWARD", 20.0f);
    const Vec neck = row(head, 3);
    const Vec eye = neck + up * (eye_up * scale) + ahead * (eye_forward * scale);

    // The view: the world turned into the eye's space, looking down -z with y
    // up, as the game's own camera is. Row vectors.
    const Vec back = ahead * -1.0f;
    Matrix view{};
    view.m[0][0] = right.x; view.m[0][1] = up.x; view.m[0][2] = back.x;
    view.m[1][0] = right.y; view.m[1][1] = up.y; view.m[1][2] = back.y;
    view.m[2][0] = right.z; view.m[2][1] = up.z; view.m[2][2] = back.z;
    view.m[3][0] = -dot(eye, right); view.m[3][1] = -dot(eye, up); view.m[3][2] = -dot(eye, back);
    view.m[3][3] = 1.0f;
    const uint32_t address = kView + uint32_t(b) * kViewStride;
    write_matrix(rdram, address, view);

    // The head, shrunk to its neck: drawn, but to nothing. The game only
    // reads this matrix to draw the head.
    Matrix shrunk{};
    shrunk.m[3][0] = neck.x; shrunk.m[3][1] = neck.y; shrunk.m[3][2] = neck.z; shrunk.m[3][3] = 1.0f;
    write_matrix(rdram, parts + kHead * kPartStride, shrunk);

    static const bool log = std::getenv("WR64_FP_LOG") != nullptr;
    static int frames = 0;
    if (log && frames++ % 30 == 0) {
        const Matrix hull = read_matrix(rdram, parts + kHull * kPartStride);
        const Matrix game = read_matrix(rdram, segment3 + kViewMatrix);
        // The game's camera: its view's origin, -t times the turn's transpose.
        Vec camera{};
        for (int j = 0; j < 3; ++j) {
            const float v = -(game.m[3][0] * game.m[j][0] + game.m[3][1] * game.m[j][1] + game.m[3][2] * game.m[j][2]);
            (j == 0 ? camera.x : j == 1 ? camera.y : camera.z) = v;
        }
        auto axes = [](const char* name, const Matrix& m) {
            std::fprintf(stderr, "  %-5s x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f) at (%.0f %.0f %.0f)\n", name,
                         m.m[0][0], m.m[0][1], m.m[0][2], m.m[1][0], m.m[1][1], m.m[1][2],
                         m.m[2][0], m.m[2][1], m.m[2][2], m.m[3][0], m.m[3][1], m.m[3][2]);
        };
        std::fprintf(stderr, "[wr64-fp] eye (%.1f %.1f %.1f), scale %.3f, the game's camera (%.0f %.0f %.0f), %.0f from the hull\n",
                     eye.x, eye.y, eye.z, scale, camera.x, camera.y, camera.z, length(camera - row(hull, 3)));
        axes("hull", hull);
        axes("head", head);
        axes("chest", chest);
        std::fflush(stderr);
    }
    return 0x80000000u | address;
}

}  // namespace wr64

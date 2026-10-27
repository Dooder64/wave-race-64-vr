// VR: the race in first person, from the player's rider.
//
// Each rider is drawn by a list of its own (in segment 2) as eighteen parts,
// each a matrix load -- the part's place in the world, absolute -- and a call
// of the part's list. The matrices are written into the frame's buffer behind
// segment 3, rider k's part p at 0xE108 + 0x40 k + 0x100 p, in the same order
// for every character (measured with WR64_3D_TRACE): 0 the jet ski's hull,
// 1 its handlebars, 3 the head and helmet, 4 the chest, 5 the hips, 6 and 12
// the upper arms, and the arms' and legs' other parts after, in pairs. The
// player's rider is the human's race slot (as src/haptics_observation.cpp
// reads it).
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
//
// Thrown off (animation 23), the view rides the tumbling chest until the eye
// meets the water. There the game sinks the body out of sight and, after a
// second or so, would start the climb back on (animation 7) -- placing the
// rider by the jet ski's stern, wherever it has drifted. Instead the view
// stays where it went in -- after a hard throw, fast or high, sinking into the
// haze for up to three seconds first, carried on a little by the throw --
// comes up to just above the waves, levels out, turns
// to the jet ski and swims to its stern at WR64_SWIM_SPEED km/h (25), and the
// climb is held back until it arrives (patches/swim_back.cpp; the game asks
// again every frame). Climbing, the view follows the rider's eyes up out of
// the water, still level -- the chest leans far back on the way up -- and
// turns to look out over the bow; seated, the chest's view takes over. A swim
// of more than 20 seconds gives up and lets the climb start where the view is.
//
// Wherever the eye is below the waves -- going in, or swimming low -- the
// water's fog is drawn round it after the scene and before the HUD: thicker
// with distance, lighter towards the surface, darker the deeper one looks.

#include "wr64/vr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wr64 {

// patches/water_all_round.cpp: the waves' height at a point.
float water_height_at(uint32_t segment3, float x, float z);
// patches/swim_back.cpp: whether the climb back on is held back.
void swim_set_holding(bool holding);

namespace {

// The game's two frame buffers, each the base of segment 3 while its frame is
// drawn (as in patches/water_all_round.cpp).
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };

// The game's view matrix, and the player's parts, in either buffer.
constexpr uint32_t kViewMatrix = 0xE088;
constexpr uint32_t kRiderParts = 0xE108;
constexpr uint32_t kRiderStride = 0x40;
constexpr uint32_t kPartStride = 0x100;

// The human's race slot, and each slot's rider state: its animation and the
// frame it is at, and the countdown to climbing back on after a fall (USA
// Rev A, as src/haptics_observation.cpp reads them), in physical memory.
constexpr uint32_t kHumanSlot = 0x0D48DC;
constexpr uint32_t kRiderState = 0x192690;
constexpr uint32_t kRiderStateStride = 0x1718;
constexpr uint32_t kAnimation = 0xC54, kAnimationFrame = 0xC58, kRecoverCountdown = 0x1608;
constexpr uint32_t kHead = 3, kChest = 4, kHull = 0;

// The view written for each frame buffer, in physical memory above the
// merged waves and the sky (0x741000 to 0x7CD000) and below the audio task's
// copy at 0x7E0000.
constexpr uint32_t kView = 0x7D0000;
constexpr uint32_t kViewStride = 0x40;

// The water's fog for each frame buffer, above the game's 4 MB beside the
// seafloor (src/seafloor.cpp): an identity matrix, then the spheres'
// vertices, then the list that draws them.
constexpr uint32_t kHaze = 0x5C0000;
constexpr uint32_t kHazeStride = 0x40000;
constexpr uint32_t kHazeVertices = 0x40;
constexpr uint32_t kHazeList = 0x18000;
// And after them the waves' underside: its vertices, then its list.
constexpr uint32_t kUnderVertices = 0x2D000;
constexpr uint32_t kUnderList = 0x2E000;

// The rider's animations: riding, thrown off, and climbing back on.
constexpr int32_t kRiding = 22, kThrown = 23, kClimbing = 7;

// The game's race runs at 30 frames a second, one view a frame.
constexpr float kFrameTime = 1.0f / 30.0f;

// Game units to a metre (as src/vr_openxr.cpp has it).
constexpr float kUnitsPerMetre = 33.3f;

// Where the game places the climbing rider: this far behind the hull's
// origin, which is near its bow (measured with WR64_FP_TRACE).
constexpr float kStern = 55.0f;

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

float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
Vec normalised(Vec v) { const float l = length(v); return l > 1e-6f ? v * (1.0f / l) : Vec{ 0.0f, 0.0f, 1.0f }; }
Vec mix(Vec a, Vec b, float t) { return a + (b - a) * t; }

// A camera: where the eye is and which way it looks, square.
struct Camera { Vec eye, right, up, ahead; };

Camera level_camera(Vec eye, Vec ahead) {
    const Vec up{ 0.0f, 1.0f, 0.0f };
    ahead = normalised(Vec{ ahead.x, 0.0f, ahead.z });
    return { eye, cross(ahead, up), up, ahead };
}

Camera blend(const Camera& a, const Camera& b, float t) {
    const Vec ahead = normalised(mix(a.ahead, b.ahead, t));
    Vec up = mix(a.up, b.up, t);
    up = normalised(up - ahead * dot(up, ahead));
    return { mix(a.eye, b.eye, t), cross(ahead, up), up, ahead };
}

// The swim back after a fall (see the top).
enum class Swim { Riding, Thrown, Swimming, Arrived, Climbing, Mounted };
struct SwimState {
    Swim phase = Swim::Riding;
    Vec eye{};
    Vec facing{ 0.0f, 0.0f, 1.0f };
    int frames = 0;
    int out_of_water = 0;
    // Thrown: where from, the chest's eye a frame ago, and the highest it
    // has been. In the water after a hard throw: how many
    // frames more to stay under, how deep, and the way the throw still
    // carries the swimmer.
    Vec thrown_from{}, last{};
    float peak = 0.0f;
    int under = 0;
    float depth = 0.0f;
    Vec drift{};
};
SwimState g_swim;

// The human's race slot, 0 to 3; 0 when it reads as anything else.
uint32_t human_slot(const uint8_t* rdram) {
    const int32_t slot = *reinterpret_cast<const int32_t*>(rdram + kHumanSlot);
    return slot >= 0 && slot < 4 ? uint32_t(slot) : 0;
}

// WR64_FP_TRACE=<file>: every race frame's hull, chest and head, how far the
// chest is from the hull, and the rider's animation, its frame and the
// countdown to climbing back on, first person or not, for studying falls.
void trace_frame(const uint8_t* rdram, uint32_t parts, uint32_t slot) {
    static std::FILE* trace = [] {
        const char* path = std::getenv("WR64_FP_TRACE");
        std::FILE* f = path != nullptr ? std::fopen(path, "w") : nullptr;
        if (f != nullptr) {
            std::fprintf(f, "frame slot hull_x hull_y hull_z hull_up_y chest_x chest_y chest_z head_x head_y head_z apart"
                            " animation animation_frame countdown\n");
        }
        return f;
    }();
    if (trace == nullptr) return;
    static uint32_t traced = 0;
    const Matrix hull = read_matrix(rdram, parts + kHull * kPartStride);
    const Matrix chest = read_matrix(rdram, parts + kChest * kPartStride);
    const Matrix head = read_matrix(rdram, parts + kHead * kPartStride);
    const Vec h = row(hull, 3), c = row(chest, 3), n = row(head, 3);
    const float hull_up = length(row(hull, 1)) > 0.0f ? row(hull, 1).y / length(row(hull, 1)) : 0.0f;
    const uint32_t state = kRiderState + slot * kRiderStateStride;
    const int32_t animation = *reinterpret_cast<const int32_t*>(rdram + state + kAnimation);
    const int32_t animation_frame = *reinterpret_cast<const int32_t*>(rdram + state + kAnimationFrame);
    const uint16_t countdown = *reinterpret_cast<const uint16_t*>(rdram + ((state + kRecoverCountdown) ^ 2));
    std::fprintf(trace, "%u %u %.1f %.1f %.1f %.2f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %d %d %u\n", traced++, slot,
                 h.x, h.y, h.z, hull_up, c.x, c.y, c.z, n.x, n.y, n.z, length(c - h), animation, animation_frame, countdown);
    std::fflush(trace);
}

// Off, on, or not yet decided (-1): on in VR mode unless WR64_VR_FIRST_PERSON=0,
// and with WR64_VR_FIRST_PERSON=1 without a headset too.
std::atomic<int> g_on{ -1 };

// How much of the chest's lean, pitch and roll the view takes, 0 to 1, from
// the settings (WR64_FP_INTENSITY in percent before they are read).
std::atomic<float> g_intensity{ std::clamp(float(std::atof(std::getenv("WR64_FP_INTENSITY") ? std::getenv("WR64_FP_INTENSITY") : "100")) / 100.0f, 0.0f, 1.0f) };

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

void set_first_person_intensity(float intensity) {
    g_intensity.store(std::clamp(intensity, 0.0f, 1.0f), std::memory_order_relaxed);
}

}  // namespace vr

namespace {

// The water round `eye` for frame buffer `b`, `surface` the water's height
// above it: nested spheres about the eye, drawn farthest first, tested
// against depth but not writing it, each translucent, so that whatever lies
// beyond a sphere is veiled by it and everything nearer is not. Together they
// thicken with distance as fog does -- each one's opacity is the water's
// between it and the next one in, for a visibility of WR64_UNDERWATER_VIS
// units (450, about 13 m) -- and the outermost is opaque. Their colour is the
// light coming through the water from that direction: brightest looking up
// at the surface, a blue-green out to the sides, near black straight down,
// and all of it dimmer the deeper the eye. Returns its list's address.
uint32_t build_haze(uint8_t* rdram, int b, Vec eye, float surface) {
    static const float visibility = std::max(50.0f, setting("WR64_UNDERWATER_VIS", 450.0f));
    const uint32_t base = kHaze + uint32_t(b) * kHazeStride;

    Matrix identity{};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;
    write_matrix(rdram, base, identity);

    constexpr int kShells = 40, kRings = 9, kAround = 16;
    constexpr float kInner = 25.0f, kOuter = 2400.0f;
    const Vec up_colour{ 70.0f, 150.0f, 170.0f }, side_colour{ 22.0f, 80.0f, 100.0f }, down_colour{ 2.0f, 9.0f, 16.0f };
    const float dim = std::exp(-std::max(0.0f, surface - eye.y) / 2000.0f);
    auto put_half = [&](uint32_t address, float value) {
        *reinterpret_cast<int16_t*>(rdram + (address ^ 2)) = int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
    };
    uint32_t* out = reinterpret_cast<uint32_t*>(rdram + base + kHazeList);
    uint32_t commands = 0;
    auto command = [&](uint32_t w0, uint32_t w1) { out[commands * 2] = w0; out[commands * 2 + 1] = w1; ++commands; };
    uint32_t vertex = 0;
    for (int shell = kShells - 1; shell >= 0; --shell) {
        auto radius = [&](int k) { return kInner + (kOuter - kInner) * std::pow(float(k) / float(kShells - 1), 1.6f); };
        const float r = radius(shell);
        const float inside = shell > 0 ? radius(shell - 1) : 0.0f;
        const float opacity = shell == kShells - 1 ? 1.0f : 1.0f - std::exp(-(r - inside) / visibility);
        const uint32_t alpha = uint32_t(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
        const uint32_t first = vertex;
        for (int ring = 0; ring < kRings; ++ring) {
            const float elevation = -1.5707963f + 3.14159265f * float(ring) / float(kRings - 1);
            const float t = std::sin(elevation);
            const Vec c = (t >= 0.0f ? mix(side_colour, up_colour, t) : mix(side_colour, down_colour, std::sqrt(-t))) * dim;
            const uint32_t rgba = (uint32_t(std::clamp(c.x, 0.0f, 255.0f)) << 24) |
                                  (uint32_t(std::clamp(c.y, 0.0f, 255.0f)) << 16) |
                                  (uint32_t(std::clamp(c.z, 0.0f, 255.0f)) << 8) | alpha;
            for (int i = 0; i < kAround; ++i) {
                // Each sphere turned a little from the last, so that their flat
                // facets do not line up into wedges on what lies behind.
                const float around = 6.2831853f * (float(i) + 0.618f * float(shell)) / float(kAround);
                const uint32_t v = base + kHazeVertices + vertex++ * 16;
                put_half(v + 0, eye.x + r * std::cos(elevation) * std::cos(around));
                put_half(v + 2, eye.y + r * std::sin(elevation));
                put_half(v + 4, eye.z + r * std::cos(elevation) * std::sin(around));
                put_half(v + 6, 0.0f); put_half(v + 8, 0.0f); put_half(v + 10, 0.0f);
                *reinterpret_cast<uint32_t*>(rdram + v + 12) = rgba;
            }
        }
        // Band by band: its two rings loaded together (32 vertices), then
        // its triangles, the cache entries named five times their number.
        for (int ring = 0; ring + 1 < kRings; ++ring) {
            const uint32_t n = 2 * kAround;
            command((0x04u << 24) | (n << 9) | (n * 16 - 1), base + kHazeVertices + (first + uint32_t(ring * kAround)) * 16);
            for (uint32_t i = 0; i < uint32_t(kAround); ++i) {
                const uint32_t j = (i + 1) % kAround;
                const uint32_t a = i, bb = j, c = kAround + i, d = kAround + j;
                command(0xBFu << 24, ((a * 5) << 16) | ((bb * 5) << 8) | (d * 5));
                command(0xBFu << 24, ((a * 5) << 16) | ((d * 5) << 8) | (c * 5));
            }
        }
    }
    command(0xB8u << 24, 0);
    return base + kHazeList;
}

// The underside of the waves round `eye` for frame buffer `b`, drawn in
// place of the water while the eye is below it: the game's water is lit and
// reflected as seen from above. A disc of the waves' own heights about the
// eye, opaque, its colour by the angle it is seen at: straight up, inside
// the cone light leaves the water by (about 49 degrees from overhead), the
// bright sky through the surface; outside it, the surface mirrors the water
// below, a dark blue-green; dimmer the deeper the eye. Returns its list's
// address.
uint32_t build_underside(uint8_t* rdram, int b, Vec eye, uint32_t segment3) {
    const uint32_t base = kHaze + uint32_t(b) * kHazeStride;
    constexpr int kAround = 16;
    constexpr float kRadii[] = { 0, 15, 35, 60, 95, 140, 200, 280, 400, 600, 900, 1400, 2400 };
    constexpr int kRings = int(sizeof(kRadii) / sizeof(kRadii[0]));
    const Vec sky{ 150.0f, 205.0f, 220.0f }, mirror{ 16.0f, 58.0f, 76.0f };
    auto put_half = [&](uint32_t address, float value) {
        *reinterpret_cast<int16_t*>(rdram + (address ^ 2)) = int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
    };
    const float surface = water_height_at(segment3, eye.x, eye.z);
    const float dim = std::exp(-std::max(0.0f, surface - eye.y) / 2000.0f);
    uint32_t vertex = 0;
    for (int ring = 0; ring < kRings; ++ring) {
        for (int i = 0; i < kAround; ++i) {
            const float around = 6.2831853f * float(i) / float(kAround);
            const float x = eye.x + kRadii[ring] * std::cos(around);
            const float z = eye.z + kRadii[ring] * std::sin(around);
            const float y = water_height_at(segment3, x, z);
            const Vec to{ x - eye.x, y - eye.y, z - eye.z };
            const float up = to.y / std::max(length(to), 1e-3f);
            const float window = clamp01((up - 0.58f) / (0.72f - 0.58f));
            const Vec c = mix(mirror, sky, window * window * (3.0f - 2.0f * window)) * dim;
            const uint32_t v = base + kUnderVertices + vertex++ * 16;
            put_half(v + 0, x); put_half(v + 2, y); put_half(v + 4, z);
            put_half(v + 6, 0.0f); put_half(v + 8, 0.0f); put_half(v + 10, 0.0f);
            *reinterpret_cast<uint32_t*>(rdram + v + 12) =
                (uint32_t(std::clamp(c.x, 0.0f, 255.0f)) << 24) | (uint32_t(std::clamp(c.y, 0.0f, 255.0f)) << 16) |
                (uint32_t(std::clamp(c.z, 0.0f, 255.0f)) << 8) | 0xFFu;
        }
    }
    uint32_t* out = reinterpret_cast<uint32_t*>(rdram + base + kUnderList);
    uint32_t commands = 0;
    auto command = [&](uint32_t w0, uint32_t w1) { out[commands * 2] = w0; out[commands * 2 + 1] = w1; ++commands; };
    for (int ring = 0; ring + 1 < kRings; ++ring) {
        const uint32_t n = 2 * kAround;
        command((0x04u << 24) | (n << 9) | (n * 16 - 1), base + kUnderVertices + uint32_t(ring * kAround) * 16);
        for (uint32_t i = 0; i < uint32_t(kAround); ++i) {
            const uint32_t j = (i + 1) % kAround;
            const uint32_t a = i, bb = j, c = kAround + i, d = kAround + j;
            command(0xBFu << 24, ((a * 5) << 16) | ((bb * 5) << 8) | (d * 5));
            command(0xBFu << 24, ((a * 5) << 16) | ((d * 5) << 8) | (c * 5));
        }
    }
    command(0xB8u << 24, 0);
    return base + kUnderList;
}

// One frame of the swim back (see the top): from the rider's animation, the
// view from the chest and the hull, the view to show. Holds back the climb
// while the swimmer is on the way.
Camera swim_step(uint32_t segment3, int32_t animation, const Camera& chest, const Matrix& hull) {
    static const float speed = setting("WR64_SWIM_SPEED", 25.0f) / 3.6f * kUnitsPerMetre;    // units a second
    static const float eye_height = setting("WR64_SWIM_EYE", 4.0f);                         // above the waves
    constexpr float kTurnRate = 4.0f;       // radians a second
    constexpr float kRiseRate = 120.0f;     // units a second, coming up
    constexpr float kArrived = 12.0f;       // units from the stern
    constexpr int kNeverWet = 45;           // frames thrown without meeting the water: let the game have it
    constexpr int kGiveUp = 20 * 30;        // frames of swimming
    constexpr int kHandBack = 12;           // frames over which the view passes back to the climbing rider
    // A hard throw: the body flying across the water faster than this, in
    // units a second (about 50 km/h), or rising higher than this. The
    // hardest -- twice as fast, or three times as high -- keep the swimmer
    // under for three seconds, 60 units down; WR64_SWIM_SINK scales it, 0
    // never.
    constexpr float kHardAcross = 460.0f, kHardHeight = 40.0f;
    constexpr float kSinkRate = 60.0f;      // units a second, going down
    static const float sink = setting("WR64_SWIM_SINK", 1.0f);

    SwimState& s = g_swim;
    const Vec bow = normalised(Vec{ row(hull, 2).x, 0.0f, row(hull, 2).z });
    const Vec stern = row(hull, 3) - bow * kStern;
    auto surface = [&](Vec p) { return water_height_at(segment3, p.x, p.z); };
    // Turns the way the swimmer faces towards `towards`, as far as a frame
    // allows; returns the angle that was left before turning.
    auto turn_to = [&](Vec towards) {
        const Vec want = normalised(Vec{ towards.x - s.eye.x, 0.0f, towards.z - s.eye.z });
        const float left = std::atan2(s.facing.z * want.x - s.facing.x * want.z, dot(s.facing, want));
        const float step = std::clamp(left, -kTurnRate * kFrameTime, kTurnRate * kFrameTime);
        const float c = std::cos(step), sn = std::sin(step);
        s.facing = normalised(Vec{ s.facing.x * c + s.facing.z * sn, 0.0f, -s.facing.x * sn + s.facing.z * c });
        return left;
    };
    auto swimmer = [&]() { return level_camera(s.eye, s.facing); };

    Camera shown = chest;
    switch (s.phase) {
    case Swim::Riding:
        if (animation == kThrown) {
            s.phase = Swim::Thrown;
            s.frames = 0;
            s.thrown_from = s.last = chest.eye;
            s.peak = chest.eye.y;
        }
        break;
    case Swim::Thrown:
        if (animation != kThrown || ++s.frames > kNeverWet) {
            s.phase = Swim::Riding;
        }
        else if (chest.eye.y < surface(chest.eye)) {
            s.phase = Swim::Swimming;
            s.eye = chest.eye;
            s.facing = length(Vec{ chest.ahead.x, 0.0f, chest.ahead.z }) > 0.1f
                           ? normalised(Vec{ chest.ahead.x, 0.0f, chest.ahead.z }) : bow;
            // How hard the throw was: how fast across the water the body
            // flew, and how high. A hard one keeps the swimmer under.
            const Vec flown{ chest.eye.x - s.thrown_from.x, 0.0f, chest.eye.z - s.thrown_from.z };
            const float across = length(flown) / (float(s.frames) * kFrameTime);
            const float climbed = s.peak - s.thrown_from.y;
            const float hard = std::max(clamp01((across - kHardAcross) / kHardAcross),
                                        clamp01((climbed - kHardHeight) / (2.0f * kHardHeight))) * sink;
            s.under = hard > 0.1f ? int(45.0f + 45.0f * hard) : 0;
            s.depth = 25.0f + 35.0f * hard;
            s.drift = Vec{ chest.eye.x - s.last.x, 0.0f, chest.eye.z - s.last.z } * (0.5f / kFrameTime);
            static const bool log = std::getenv("WR64_SWIM_LOG") != nullptr;
            if (log) {
                std::fprintf(stderr, "[wr64-swim] in the water: flew %.0f units a second, rose %.0f; %s\n", across, climbed,
                             s.under > 0 ? "a hard throw, staying under" : "coming straight up");
                std::fflush(stderr);
            }
            s.frames = 0;
            shown = swimmer();
        }
        else {
            s.peak = std::max(s.peak, chest.eye.y);
            s.last = chest.eye;
        }
        break;
    case Swim::Swimming: {
        if (animation != kThrown) {
            // The game moved on without the swimmer: the race ended, say.
            s.phase = animation == kClimbing ? Swim::Climbing : Swim::Riding;
            s.out_of_water = 0;
            shown = s.phase == Swim::Climbing ? swimmer() : chest;
            break;
        }
        if (s.under > 0) {
            // Under after a hard throw: sinking, carried on by the throw a
            // little way, turning towards the jet ski unseen.
            --s.under;
            turn_to(stern);
            s.eye = s.eye + s.drift * kFrameTime;
            s.drift = s.drift * 0.9f;
            const float down = surface(s.eye) - s.depth - s.eye.y;
            s.eye.y += std::clamp(down, -kSinkRate * kFrameTime, kRiseRate * kFrameTime);
            ++s.frames;
            shown = swimmer();
            break;
        }
        const float left = turn_to(stern);
        const Vec to{ stern.x - s.eye.x, 0.0f, stern.z - s.eye.z };
        const float distance = length(to);
        // Swimming the way it faces, slowed while still turning far round.
        const float pace = clamp01(std::cos(left) * 1.5f - 0.5f);
        const float move = std::min(distance, speed * kFrameTime * pace);
        s.eye = s.eye + s.facing * move;
        const float rise = surface(s.eye) + eye_height - s.eye.y;
        s.eye.y += std::clamp(rise, -3.0f * kRiseRate * kFrameTime, kRiseRate * kFrameTime);
        if (distance - move < kArrived || ++s.frames > kGiveUp) {
            s.phase = Swim::Arrived;
        }
        shown = swimmer();
        break;
    }
    case Swim::Arrived:
        turn_to(row(hull, 3));
        if (animation == kClimbing) {
            s.phase = Swim::Climbing;
            s.out_of_water = 0;
        }
        else if (animation != kThrown) {
            s.phase = Swim::Riding;
            break;
        }
        shown = swimmer();
        break;
    case Swim::Climbing: {
        // Climbing, the chest leans far back, so the view only follows the
        // eyes up, level, turning to look out over the bow.
        turn_to(row(hull, 3) + bow * 300.0f);
        if (chest.eye.y > surface(chest.eye)) ++s.out_of_water;
        s.eye = mix(s.eye, chest.eye, clamp01(float(s.out_of_water) / float(kHandBack)));
        if (animation == kRiding) {
            s.phase = Swim::Mounted;
            s.frames = 0;
        }
        else if (animation != kClimbing) {
            s.phase = Swim::Riding;
            break;
        }
        shown = swimmer();
        break;
    }
    case Swim::Mounted: {
        // Seated: the chest's view takes over.
        const float t = clamp01(float(++s.frames) / float(kHandBack));
        if (t >= 1.0f || animation == kThrown) {
            s.phase = Swim::Riding;
            break;
        }
        shown = blend(swimmer(), chest, t);
        break;
    }
    }
    swim_set_holding(s.phase == Swim::Thrown || s.phase == Swim::Swimming);
    return shown;
}

}  // namespace

// For the display-list rewriter, in a race: the address of a view from the
// player's rider's eyes -- or swimming back to the jet ski -- to multiply
// onto the projection in place of the game's, with the rider's head shrunk
// away; or 0, leaving the frame as it is. With the eye below the waves,
// `haze` is given the list drawing the water's fog round it, `identity` an
// identity matrix to draw it with and `underside` the list drawing the
// waves' underside in place of the water; otherwise both lists are 0.
uint32_t first_person_view(uint8_t* rdram, uint32_t segment3, uint32_t* haze, uint32_t* identity, uint32_t* underside) {
    *haze = 0;
    *underside = 0;
    int b = -1;
    for (int i = 0; i < 2; ++i) {
        if (segment3 == kFrameBuffers[i]) b = i;
    }
    if (b < 0) return 0;
    const uint32_t slot = human_slot(rdram);
    const uint32_t parts = segment3 + kRiderParts + slot * kRiderStride;
    trace_frame(rdram, parts, slot);
    if (!vr::first_person()) {
        g_swim.phase = Swim::Riding;
        swim_set_holding(false);
        return 0;
    }
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
    const Vec chest_eye = neck + up * (eye_up * scale) + ahead * (eye_forward * scale);

    // Thrown off, the swim back takes over the view.
    const int32_t animation = *reinterpret_cast<const int32_t*>(
        rdram + kRiderState + slot * kRiderStateStride + kAnimation);
    const Matrix hull = read_matrix(rdram, parts + kHull * kPartStride);
    // The settings' first-person intensity: how much of the chest's lean,
    // pitch and roll the view takes, the rest of it level, facing the same
    // way.
    const Camera body{ chest_eye, right, up, ahead };
    const float intensity = g_intensity.load(std::memory_order_relaxed);
    const Camera ridden = intensity >= 1.0f ? body : blend(level_camera(chest_eye, ahead), body, intensity);
    const Camera camera = swim_step(segment3, animation, ridden, hull);
    const Vec eye = camera.eye;
    static const bool fog_trace = std::getenv("WR64_FOG_TRACE") != nullptr;
    if (fog_trace) {
        static int n = 0;
        if (n++ % 15 == 0) std::fprintf(stderr, "[wr64-fog] eye %.0f %.0f %.0f surface %.1f\n",
                                        eye.x, eye.y, eye.z, water_height_at(segment3, eye.x, eye.z));
    }
    if (eye.y < water_height_at(segment3, eye.x, eye.z)) {
        *haze = 0x80000000u | build_haze(rdram, b, eye, water_height_at(segment3, eye.x, eye.z));
        *identity = 0x80000000u | (kHaze + uint32_t(b) * kHazeStride);
        *underside = 0x80000000u | build_underside(rdram, b, eye, segment3);
    }

    // The view: the world turned into the eye's space, looking down -z with y
    // up, as the game's own camera is. Row vectors.
    const Vec back = camera.ahead * -1.0f;
    Matrix view{};
    view.m[0][0] = camera.right.x; view.m[0][1] = camera.up.x; view.m[0][2] = back.x;
    view.m[1][0] = camera.right.y; view.m[1][1] = camera.up.y; view.m[1][2] = back.y;
    view.m[2][0] = camera.right.z; view.m[2][1] = camera.up.z; view.m[2][2] = back.z;
    view.m[3][0] = -dot(eye, camera.right); view.m[3][1] = -dot(eye, camera.up); view.m[3][2] = -dot(eye, back);
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

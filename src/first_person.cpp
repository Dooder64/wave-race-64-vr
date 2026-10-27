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
// meets the water or solid ground. There the game sinks the body out of sight
// and, after a second or so, would start the climb back on (animation 7) --
// placing the rider by the jet ski's stern, wherever it has drifted. Instead
// the view takes over where the body came down:
//
// - Onto solid ground (the course's collision, or a seafloor), it stands up
//   there, the eye as high above the ground as it was above the hull seated,
//   and walks to the water at WR64_WALK_SPEED km/h (10).
// - Crashing in a sharp turn -- the stick held at least 75% to one side
//   (WR64_SHARP_STICK) and the jet ski turning at least 30 degrees a second
//   (WR64_SHARP_TURN) -- and into the water fast and shallow, it skids:
//   skips across the surface,
//   losing speed at each touch, then slides to a stop, throwing up spray
//   (water::set_body_spray) and the game's own bow splash, kept from the
//   last second of riding (src/body_splash.cpp). Hard or steep, it sinks at a steady rate for
//   half a second to four -- the faster or higher the fall, the longer --
//   carried on a little by the throw. Any other crash fast enough -- into
//   something, or off a jump -- is an even chance of that sink or a bounce:
//   tumbling end over end, it leaps once from the water, two to four metres
//   (the harder the crash, the higher), and comes down into the same sink
//   (WR64_FALL=skid, sink or bounce forces one); a gentle fall sinks if hard
//   or steep enough, and otherwise comes straight up.
//   Skidding, bouncing, and sinking once under the water, the view tumbles -- faster
//   the faster the body moves, scaled by the first-person intensity, partly a
//   roll (WR64_TUMBLE_ROLL) and partly end over end -- and when the body stops
//   it finishes the turn it is on.
//
// Then it swims to the jet ski's stern at WR64_SWIM_SPEED km/h (25), level,
// the eye WR64_SWIM_EYE units above the waves (12, some 35 cm) -- up at a
// slant from a sink, standing where the water is shallow -- going round
// whatever stands in the way out of the water (course_clearance: the ramps,
// rocks and pillars) and the jet ski itself by the shortest way, and the
// climb is held back until it arrives
// (patches/swim_back.cpp; the game asks again every frame). Climbing, the
// view follows the rider's eyes up out of the water, still level -- the chest
// leans far back on the way up -- and turns to look out over the bow; seated,
// the chest's view takes over. A swim of more than 20 seconds gives up and
// lets the climb start where the view is.
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
#include <queue>
#include <random>
#include <vector>

namespace wr64 {

// patches/water_all_round.cpp: the waves' height at a point.
float water_height_at(uint32_t segment3, float x, float z);
// patches/swim_back.cpp: whether the climb back on is held back.
void swim_set_holding(bool holding);
// src/callbacks.cpp: the stick as the game last read it, -1 to 1.
float last_stick_x();
// src/body_splash.cpp: this frame's rider and skidding body, for the game's
// bow splash round the body.
void body_splash_note(bool riding, float hull_x, float hull_z, float hull_water, float bow_x, float bow_z,
                      bool skidding, float body_x, float body_z, float body_water, float slide_x, float slide_z, float wet);
// src/seafloor.cpp: the seafloor's height, on a course with one; how far a
// point is from anything solid standing out of the water.
bool seafloor_height_at(uint32_t course, float x, float z, float* height);
float course_clearance(uint32_t course, float x, float z);
namespace water {
// src/water.cpp: the course's solid ground, and the skidding body's spray.
bool ground_height_at(float x, float z, float below, float* height);
void set_body_spray(bool active, float x, float y, float z, float wet);
}

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
enum class Swim { Riding, Thrown, Skidding, Bouncing, Sinking, Settling, Swimming, Arrived, Climbing, Mounted };
struct SwimState {
    Swim phase = Swim::Riding;
    Vec eye{};
    Vec facing{ 0.0f, 0.0f, 1.0f };
    int frames = 0;
    int out_of_water = 0;
    // Thrown: where from, the chest's eye a frame ago, and the highest it
    // has been.
    Vec thrown_from{}, last{};
    float peak = 0.0f;
    // Sinking: how many frames more to go down, and the way the throw still
    // carries the body. Swimming: whether it comes up at a slant
    // (after sinking) rather than straight up.
    int under = 0;
    Vec drift{};
    bool slant = false;
    // Skidding and bouncing: the body on the water, its speed across it, how
    // high it is off the water on a skip and how fast it rises; bouncing,
    // how long to sink after it comes down.
    Vec body{}, velocity{};
    float hop = 0.0f, rise = 0.0f;
    int sink_after = 0;
    // Riding: how far the stick has lately been held to one side, how fast
    // the jet ski turns (radians a second) and its bow a frame ago; thrown,
    // whether that was in a sharp turn.
    float stick = 0.0f, turning = 0.0f;
    Vec last_bow{ 0.0f, 0.0f, 1.0f };
    bool sharp = false;
    // The tumble: how far round (radians), about which axis, and how fast it
    // last turned. The view the throw left, eased out of over the first frames
    // of a skid or a sink.
    float spin = 0.0f, spin_rate = 0.0f;
    Vec spin_axis{ 1.0f, 0.0f, 0.0f };
    Camera entry{};
    // The rider's eye above the hull's origin when seated: standing on the
    // ground, the eye is this far above it.
    float stand = 60.0f;
    // Swimming: the way round what is in the way (see plan_route), which
    // point of it is next, and the stern and bow it was planned to and when.
    // Walking: the ground underfoot, going on down at a beach's slope where
    // the course's collision stops short of the water.
    std::vector<Vec> route;
    size_t leg = 0;
    Vec routed_to{}, routed_bow{};
    int routed_at = 0;
    float underfoot = -1e9f;
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

// The view `camera` turned `angle` radians about `axis` (a unit vector) at
// its eye.
Camera tumbled(const Camera& camera, Vec axis, float angle) {
    const float c = std::cos(angle), sn = std::sin(angle);
    auto turn = [&](Vec v) { return v * c + cross(axis, v) * sn + axis * (dot(axis, v) * (1.0f - c)); };
    return { camera.eye, turn(camera.right), turn(camera.up), turn(camera.ahead) };
}

// The swimmer keeps this far (units) from anything solid.
constexpr float kClearance = 30.0f;

// What the swimmer goes round: the course's solid things, and the jet ski,
// a box about its hull's origin `hull` facing `bow` (level), this far ahead
// and behind and to either side -- the hull's own model, measured
// (WR64_3D_TRACE_VERTS), reaches 49, 36 and 15.
constexpr float kHullAhead = 52.0f, kHullBehind = 38.0f, kHullSide = 17.0f;
struct Obstacles {
    uint32_t course;
    Vec hull, bow;
    // How far `p` is from the jet ski, negative inside.
    float ski(Vec p) const {
        const Vec d{ p.x - hull.x, 0.0f, p.z - hull.z };
        const float along = d.x * bow.x + d.z * bow.z, across = std::abs(d.x * bow.z - d.z * bow.x);
        const float out_along = std::max(along - kHullAhead, -kHullBehind - along);
        const float out_across = across - kHullSide;
        return out_along > 0.0f || out_across > 0.0f
            ? std::hypot(std::max(out_along, 0.0f), std::max(out_across, 0.0f))
            : std::max(out_along, out_across);
    }
    // How far `p` is from the nearest of them, negative inside.
    float clearance(Vec p) const { return std::min(ski(p), course_clearance(course, p.x, p.z)); }
};

// Whether the swimmer can go straight from `a` to `b` without coming within
// kClearance of anything solid -- or, starting that near something, while
// only drawing away from it first, and ending that near (beside the jet
// ski's stern, say), while only closing in at the last.
bool clear_between(const Obstacles& o, Vec a, Vec b) {
    constexpr float kNear = 0.8f * kClearance, kSlack = 1.0f;
    auto at = [&](float t) { return o.clearance(Vec{ a.x + (b.x - a.x) * t, 0.0f, a.z + (b.z - a.z) * t }); };
    const int steps = std::max(1, int(std::hypot(b.x - a.x, b.z - a.z) / 12.0f));
    const bool arriving_near = at(1.0f) < kNear;
    float last = at(0.0f);
    enum { Leaving, Open, Arriving } part = last < kNear ? Leaving : Open;
    for (int i = 1; i <= steps; ++i) {
        const float here = at(float(i) / float(steps));
        if (part == Leaving) {
            if (here >= kNear) part = Open;
            else if (here < last - kSlack) return false;
        }
        else if (part == Open) {
            if (here < kNear) {
                if (!arriving_near) return false;
                part = Arriving;
            }
        }
        else if (here > last + kSlack || here >= kNear) {
            return false;
        }
        last = here;
    }
    return true;
}

// The shortest way from `from` to `to` across a grid of cells `cell` units a
// side reaching `margin` beyond them, as the cells' centres in turn, ending
// with `to`; empty where there is none (see plan_route).
std::vector<Vec> grid_route(const Obstacles& o, Vec from, Vec to, float cell, float margin) {
    constexpr float kThroughSolid = 40.0f;
    constexpr int kMostCells = 128 * 128;
    const float x0 = std::min(from.x, to.x) - margin, z0 = std::min(from.z, to.z) - margin;
    const int w = int((std::max(from.x, to.x) + margin - x0) / cell) + 1;
    const int h = int((std::max(from.z, to.z) + margin - z0) / cell) + 1;
    if (w * h > kMostCells) return {};
    auto centre = [&](int i) { return Vec{ x0 + (float(i % w) + 0.5f) * cell, from.y, z0 + (float(i / w) + 0.5f) * cell }; };
    auto cell_of = [&](Vec p) {
        return std::clamp(int((p.x - x0) / cell), 0, w - 1) + w * std::clamp(int((p.z - z0) / cell), 0, h - 1);
    };
    std::vector<float> room(size_t(w * h));
    for (int i = 0; i < w * h; ++i) room[size_t(i)] = o.clearance(centre(i));
    const int start = cell_of(from), goal = cell_of(to);
    const int gx = goal % w, gz = goal / w;
    auto estimate = [&](int i) {
        const float dx = std::abs(float(i % w - gx)), dz = std::abs(float(i / w - gz));
        return std::max(dx, dz) + 0.41421f * std::min(dx, dz);
    };
    std::vector<float> cost(size_t(w * h), 1e30f);
    std::vector<int> came(size_t(w * h), -1);
    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    cost[size_t(start)] = 0.0f;
    open.push({ estimate(start), start });
    while (!open.empty()) {
        const auto [ranked, i] = open.top();
        open.pop();
        if (i == goal) break;
        if (ranked > cost[size_t(i)] + estimate(i) + 1e-3f) continue;
        const int x = i % w, z = i / w;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx == 0 && dz == 0) || x + dx < 0 || x + dx >= w || z + dz < 0 || z + dz >= h) continue;
                const int j = i + dx + dz * w;
                if (room[size_t(j)] < 0.5f * kClearance && room[size_t(j)] <= room[size_t(i)] && j != goal) continue;
                const float step = (dx != 0 && dz != 0 ? 1.41421f : 1.0f) * (room[size_t(j)] < kClearance ? kThroughSolid : 1.0f);
                if (cost[size_t(i)] + step < cost[size_t(j)]) {
                    cost[size_t(j)] = cost[size_t(i)] + step;
                    came[size_t(j)] = i;
                    open.push({ cost[size_t(j)] + estimate(j), j });
                }
            }
        }
    }
    if (goal != start && came[size_t(goal)] < 0) return {};
    std::vector<Vec> cells;
    for (int i = came[size_t(goal)]; i >= 0 && i != start; i = came[size_t(i)]) cells.push_back(centre(i));
    std::reverse(cells.begin(), cells.end());
    cells.push_back(to);
    return cells;
}

// `p`, or where it comes to going straight out from what is near it until
// `room(q)` -- how far q is from it -- is at least `want`.
template <typename Room>
Vec stepped_out(Vec p, float want, Room room) {
    for (int i = 0; i < 16; ++i) {
        const float here = room(p);
        if (here >= want) break;
        constexpr float e = 4.0f;
        const Vec away{ room(p + Vec{ e, 0.0f, 0.0f }) - room(p - Vec{ e, 0.0f, 0.0f }), 0.0f,
                        room(p + Vec{ 0.0f, 0.0f, e }) - room(p - Vec{ 0.0f, 0.0f, e }) };
        if (length(away) < 1e-3f) break;
        p = p + normalised(away) * std::max(8.0f, want - here + 4.0f);
    }
    return p;
}

// The way from `from` to `to` round anything solid in between, as the points
// to make for in turn, the last `to`: the shortest across a grid of cells,
// pulled straight between its corners. A cell within kClearance of anything
// solid costs far more to cross than open water, and one within half that
// (where the swimmer may not go) can only be crossed drawing away from it --
// so a swimmer beside something finds the way out, not through -- or to
// reach `to`. Straight to `to` where nothing is in the way, or no way round
// is found. The grid first reaches 15 m beyond `from` and `to`; where that is
// not far enough to get round, a coarser one reaching 50 m.
std::vector<Vec> plan_route(const Obstacles& o, Vec from, Vec to) {
    if (clear_between(o, from, to)) return { to };
    std::vector<Vec> route;
    // From too near something (thrown against a ramp, say), first straight
    // out from it, then the way round from there.
    const Vec out = stepped_out(from, kClearance, [&](Vec q) { return o.clearance(q); });
    if (out.x != from.x || out.z != from.z) {
        route.push_back(out);
        if (clear_between(o, out, to)) {
            route.push_back(to);
            return route;
        }
    }
    std::vector<Vec> cells = grid_route(o, out, to, 32.0f, 480.0f);
    if (cells.empty()) cells = grid_route(o, out, to, 64.0f, 1600.0f);
    if (cells.empty()) return { to };
    // Pulled straight: from each corner, on to the farthest cell in sight.
    Vec at = out;
    size_t next = 0;
    while (next < cells.size()) {
        size_t far = next;
        while (far + 1 < cells.size() && clear_between(o, at, cells[far + 1])) ++far;
        at = cells[far];
        route.push_back(at);
        next = far + 1;
    }
    return route;
}

// One frame of the swim back (see the top): from the rider's animation, the
// view from the chest and the hull, the view to show. Holds back the climb
// while the swimmer is on the way. `course` is the course raced.
Camera swim_step(uint32_t segment3, uint32_t course, int32_t animation, const Camera& chest, const Matrix& hull) {
    static const float speed = setting("WR64_SWIM_SPEED", 25.0f) / 3.6f * kUnitsPerMetre;    // units a second
    static const float walk = setting("WR64_WALK_SPEED", 10.0f) / 3.6f * kUnitsPerMetre;     // units a second
    static const float eye_height = setting("WR64_SWIM_EYE", 12.0f);                        // above the waves
    constexpr float kTurnRate = 4.0f;       // radians a second
    constexpr float kRiseRate = 120.0f;     // units a second, coming up
    constexpr float kArrived = 12.0f;       // units from the stern
    constexpr int kNeverWet = 45;           // frames thrown without meeting the water: let the game have it
    constexpr int kGiveUp = 20 * 30;        // frames of swimming
    constexpr int kHandBack = 12;           // frames over which the view passes back to the climbing rider
    constexpr int kEaseIn = 6;              // frames over which a skid or a sink takes over from the throw's view
    // A hard throw: the body flying across the water faster than this, in
    // units a second (about 50 km/h), or rising higher than this. The
    // hardest -- twice as fast, or three times as high -- keep the swimmer
    // under for three seconds, 60 units down; WR64_SWIM_SINK scales it, 0
    // never.
    constexpr float kHardAcross = 460.0f, kHardHeight = 40.0f;
    // A sink goes down at a steady rate, for longer the harder the fall: from
    // half a second for one barely hard enough to four seconds for one met
    // at this speed (about 150 km/h) or from this high above the water
    // (about 7.5 m) -- some 30 to 240 units down, stopping on the seabed.
    // WR64_SWIM_SINK scales how long, 0 never.
    constexpr float kSinkRate = 60.0f;      // units a second, going down
    constexpr float kSinkShort = 0.5f, kSinkLong = 4.0f;        // seconds
    constexpr float kFiercest = 1400.0f, kTallest = 250.0f;
    static const float sink = setting("WR64_SWIM_SINK", 1.0f);
    // A skid: the body meets the water faster than this across it (about
    // 32 km/h), at less than this slope down (about 35 degrees).
    constexpr float kSkidAcross = 300.0f, kSkidSlope = 0.7f;
    constexpr float kGravity = 330.0f;      // units a second squared: 9.8 m/s^2
    constexpr float kSkidStop = 35.0f;      // units a second: the skid is over
    constexpr float kLying = 6.0f;          // the eye above the water, lying on it
    // The tumble turns as a body this far round would roll (units), at most
    // this fast, and finishes its last turn at least this fast (radians a
    // second); WR64_TUMBLE_ROLL is how much of it is a roll about the way
    // the body travels rather than end over end (0 to 1).
    constexpr float kRollRadius = 40.0f, kMaxSpin = 14.0f, kSettleSpin = 4.0f;
    static const float roll_share = clamp01(setting("WR64_TUMBLE_ROLL", 0.5f));
    // WR64_FALL=skid, sink or bounce forces one kind of fall where it could be.
    static const int forced = [] {
        const char* value = std::getenv("WR64_FALL");
        return value == nullptr ? 0 : std::strcmp(value, "skid") == 0 ? 1 : std::strcmp(value, "sink") == 0 ? 2
             : std::strcmp(value, "bounce") == 0 ? 3 : 0;
    }();
    // A sharp turn: the stick at least this far to one side, the jet ski
    // turning at least this fast (degrees a second).
    static const float sharp_stick = setting("WR64_SHARP_STICK", 0.75f);
    static const float sharp_turn = setting("WR64_SHARP_TURN", 30.0f) * 3.14159265f / 180.0f;
    // A bounce: one leap, from this high (units) for the gentlest crash to
    // this high for the fiercest.
    constexpr float kLeapLow = 65.0f, kLeapHigh = 130.0f;
    static std::mt19937 chance{ std::random_device{}() };
    static const bool log = std::getenv("WR64_SWIM_LOG") != nullptr;

    SwimState& s = g_swim;
    const float intensity = g_intensity.load(std::memory_order_relaxed);
    const Vec bow = normalised(Vec{ row(hull, 2).x, 0.0f, row(hull, 2).z });
    const Vec stern = row(hull, 3) - bow * kStern;
    auto surface = [&](Vec p) { return water_height_at(segment3, p.x, p.z); };
    // The solid ground under `p`, from the course's collision or its
    // seafloor, at most a little above it; very low where there is none.
    auto ground = [&](Vec p) {
        float best = -1e9f, y = 0.0f;
        if (water::ground_height_at(p.x, p.z, p.y + 30.0f, &y)) best = y;
        if (seafloor_height_at(course, p.x, p.z, &y)) best = std::max(best, y);
        return best;
    };
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
    // The body tumbling at `moving` units a second: faster the faster it
    // goes, scaled by the first-person intensity.
    auto spin = [&](float moving) {
        s.spin_rate = std::min(kMaxSpin, intensity * moving / kRollRadius);
        s.spin += s.spin_rate * kFrameTime;
    };
    auto tumbling = [&]() {
        const Camera view = tumbled(swimmer(), s.spin_axis, s.spin);
        return s.frames < kEaseIn ? blend(s.entry, view, float(s.frames + 1) / float(kEaseIn)) : view;
    };
    // The tumble's axis for a body travelling `along`: end over end, with
    // some of it a roll.
    auto axis_for = [&](Vec along) {
        const Vec ahead = normalised(Vec{ along.x, 0.0f, along.z });
        const Vec side = cross(Vec{ 0.0f, 1.0f, 0.0f }, ahead);
        return normalised(side * (1.0f - roll_share) + ahead * roll_share);
    };
    auto start_swim = [&](bool slant) {
        s.phase = Swim::Swimming;
        s.slant = slant;
        s.frames = 0;
        s.route.clear();
        s.underfoot = -1e9f;
    };
    // Whether the body at `p` would be in something solid on the course
    // (skidding and sinking: the jet ski is no wall to them).
    auto in_solid = [&](Vec p) { return course_clearance(course, p.x, p.z) < 0.5f * kClearance; };
    const Obstacles obstacles{ course, row(hull, 3), bow };

    Camera shown = chest;
    bool spraying = false;
    float spray_wet = 0.0f;
    if (animation != kThrown && (s.phase == Swim::Skidding || s.phase == Swim::Bouncing || s.phase == Swim::Sinking ||
                                 s.phase == Swim::Settling || s.phase == Swim::Swimming)) {
        // The game moved on without the swimmer: the race ended, say.
        s.phase = animation == kClimbing ? Swim::Climbing : Swim::Riding;
        s.out_of_water = 0;
    }
    switch (s.phase) {
    case Swim::Riding:
        // Seated, how high the eye is above the hull: standing, the same.
        if (animation == kRiding) {
            s.stand += (std::clamp(chest.eye.y - row(hull, 3).y, 30.0f, 140.0f) - s.stand) * 0.05f;
            // How hard it is being turned: the stick held over the last few
            // frames, and the bow's swing.
            s.stick = std::max(std::abs(last_stick_x()), s.stick * 0.8f);
            const float swing = std::atan2(s.last_bow.z * bow.x - s.last_bow.x * bow.z, dot(s.last_bow, bow)) / kFrameTime;
            s.turning += (std::abs(swing) - s.turning) * 0.35f;
            s.last_bow = bow;
            static int logged = 0;
            if (log && logged++ % 30 == 0) {
                std::fprintf(stderr, "[wr64-swim] riding at %.0f %.0f %.0f: ground %.0f, water %.0f, eye %.0f above the hull;"
                                     " stick %.0f%%, turning %.0f degrees a second\n",
                             chest.eye.x, chest.eye.y, chest.eye.z, ground(chest.eye), surface(chest.eye), s.stand,
                             s.stick * 100.0f, s.turning * 180.0f / 3.14159265f);
                std::fflush(stderr);
            }
        }
        if (animation == kThrown) {
            s.phase = Swim::Thrown;
            s.sharp = s.stick >= sharp_stick && s.turning >= sharp_turn;
            if (log) {
                std::fprintf(stderr, "[wr64-swim] thrown: stick %.0f%%, turning %.0f degrees a second%s\n",
                             s.stick * 100.0f, s.turning * 180.0f / 3.14159265f, s.sharp ? " (a sharp turn)" : "");
                std::fflush(stderr);
            }
            s.stick = 0.0f;
            s.turning = 0.0f;
            s.frames = 0;
            s.thrown_from = s.last = chest.eye;
            s.peak = chest.eye.y;
        }
        break;
    case Swim::Thrown: {
        if (animation != kThrown || ++s.frames > kNeverWet) {
            s.phase = Swim::Riding;
            break;
        }
        const float water = surface(chest.eye);
        const float land = ground(chest.eye);
        const Vec moving = (chest.eye - s.last) * (1.0f / kFrameTime);
        if (land > water - 2.0f && chest.eye.y < land + 25.0f) {
            // Thrown onto solid ground: no sinking into it. The view comes to
            // rest where the body landed and stands up from there.
            s.eye = chest.eye;
            s.facing = length(Vec{ moving.x, 0.0f, moving.z }) > 1.0f ? normalised(Vec{ moving.x, 0.0f, moving.z }) : bow;
            if (log) {
                std::fprintf(stderr, "[wr64-swim] onto the ground at %.0f\n", land);
                std::fflush(stderr);
            }
            start_swim(false);
            shown = swimmer();
            break;
        }
        if (chest.eye.y >= water) {
            s.peak = std::max(s.peak, chest.eye.y);
            s.last = chest.eye;
            break;
        }
        // In the water. How hard the throw was: how fast across the water
        // the body flew, and how high; and how it meets the water.
        const Vec flown{ chest.eye.x - s.thrown_from.x, 0.0f, chest.eye.z - s.thrown_from.z };
        const float across_mean = length(flown) / (float(s.frames) * kFrameTime);
        const float climbed = s.peak - s.thrown_from.y;
        const float hard = std::max(clamp01((across_mean - kHardAcross) / kHardAcross),
                                    clamp01((climbed - kHardHeight) / (2.0f * kHardHeight))) * sink;
        const float across = length(Vec{ moving.x, 0.0f, moving.z });
        const float descent = std::max(0.0f, -moving.y);
        // Skimming in fast and shallow in a sharp turn skids; any other crash
        // fast enough bounces, and a hard or a steep one can sink instead.
        // Where a crash could bounce or sink, it is an even chance.
        const float met = std::sqrt(across * across + descent * descent);
        const float fierce = std::max(clamp01((met - kSkidAcross) / (kFiercest - kSkidAcross)),
                                      clamp01((s.peak - water) / kTallest));
        const bool can_skid = across > kSkidAcross && descent < across * kSkidSlope;
        const bool can_sink = hard > 0.1f || descent > across;
        const bool can_bounce = met > kSkidAcross;
        bool skid = false, bounce = false;
        if (forced == 1 && can_skid) skid = true;
        else if (forced == 3 && can_bounce) bounce = true;
        else if (forced == 2 && can_sink) {}
        else if (s.sharp && can_skid) skid = true;
        else if (can_bounce) bounce = (chance() & 1) != 0;
        s.entry = chest;
        s.facing = across > 1.0f ? normalised(Vec{ moving.x, 0.0f, moving.z }) : bow;
        s.spin = 0.0f;
        s.spin_axis = axis_for(s.facing);
        s.frames = 0;
        if (bounce) {
            // End over end, forwards.
            s.phase = Swim::Bouncing;
            s.body = Vec{ chest.eye.x, water, chest.eye.z };
            s.velocity = Vec{ moving.x, 0.0f, moving.z } * 0.7f;
            s.spin_axis = normalised(cross(Vec{ 0.0f, 1.0f, 0.0f }, s.facing));
            s.hop = 0.0f;
            s.rise = std::sqrt(2.0f * kGravity * (kLeapLow + (kLeapHigh - kLeapLow) * fierce));
            s.sink_after = int((kSinkShort + (kSinkLong - kSinkShort) * fierce) * sink / kFrameTime);
            s.eye = s.body + Vec{ 0.0f, kLying, 0.0f };
        }
        else if (skid) {
            s.phase = Swim::Skidding;
            s.body = Vec{ chest.eye.x, water, chest.eye.z };
            s.velocity = Vec{ moving.x, 0.0f, moving.z } * 0.85f;
            s.hop = 0.0f;
            s.rise = std::min(0.22f * across, 220.0f);
            s.eye = s.body + Vec{ 0.0f, kLying, 0.0f };
        }
        else if (can_sink || can_bounce) {
            // How long by how fierce the fall: how fast the body met the
            // water, or how high above it the throw took it, whichever is more.
            s.phase = Swim::Sinking;
            s.eye = chest.eye;
            s.under = int((kSinkShort + (kSinkLong - kSinkShort) * fierce) * sink / kFrameTime);
            s.drift = Vec{ moving.x, 0.0f, moving.z } * 0.5f;
        }
        else {
            // A gentle fall: straight back up, no tumble.
            s.eye = chest.eye;
            start_swim(false);
        }
        if (log) {
            std::fprintf(stderr, "[wr64-swim] in the water: %.0f units a second across, %.0f down, rose %.0f; %s",
                         across, descent, climbed,
                         s.phase == Swim::Skidding ? "skidding (a sharp turn)"
                         : s.phase == Swim::Bouncing ? "bouncing (by chance)"
                         : s.phase == Swim::Sinking ? (can_bounce ? "sinking (by chance)" : "sinking")
                         : "coming straight up");
            if (s.phase == Swim::Sinking) std::fprintf(stderr, " for %.1f s", float(s.under) * kFrameTime);
            if (s.phase == Swim::Bouncing) std::fprintf(stderr, " %.0f units up, then under for %.1f s",
                                                        s.rise * s.rise / (2.0f * kGravity), float(s.sink_after) * kFrameTime);
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
        shown = s.phase == Swim::Swimming ? swimmer() : s.entry;
        break;
    }
    case Swim::Skidding: {
        // Skipping across the water, losing speed at each touch, then
        // sliding to a stop; onto solid ground, stopping short.
        // Into something solid: stopped dead.
        if (in_solid(s.body + s.velocity * kFrameTime)) s.velocity = Vec{};
        s.body = s.body + s.velocity * kFrameTime;
        const float water = surface(s.body);
        const float land = ground(Vec{ s.body.x, water + 40.0f, s.body.z });
        const bool on_land = land > water - 2.0f;
        const float base = on_land ? land : water;
        s.rise -= kGravity * kFrameTime;
        s.hop += s.rise * kFrameTime;
        float moving = length(s.velocity);
        bool touching = false;
        if (s.hop <= 0.0f) {
            s.hop = 0.0f;
            touching = true;
            if (s.rise < -40.0f && !on_land) {
                s.rise = -s.rise * 0.45f;           // a skip
                s.velocity = s.velocity * 0.75f;
            }
            else {
                s.rise = 0.0f;                      // sliding
                const float lose = ((on_land ? 6.0f : 1.4f) * moving + 60.0f) * kFrameTime;
                s.velocity = moving > lose ? s.velocity * ((moving - lose) / moving) : Vec{};
            }
            moving = length(s.velocity);
        }
        if (moving > 1.0f) s.facing = normalised(s.velocity);
        s.eye = Vec{ s.body.x, base + s.hop + kLying, s.body.z };
        spin(moving);
        spraying = !on_land;
        spray_wet = touching ? 1.0f : 0.0f;
        ++s.frames;
        shown = tumbling();
        if (moving < kSkidStop && s.hop == 0.0f) {
            s.phase = Swim::Settling;
            s.slant = false;
        }
        break;
    }
    case Swim::Bouncing: {
        // Leaping once from the water and falling back, end over end, then
        // under, sinking as a sink does. Onto solid ground, or into
        // something solid, it stops there.
        if (in_solid(s.body + s.velocity * kFrameTime)) s.velocity = Vec{};
        s.body = s.body + s.velocity * kFrameTime;
        const float water = surface(s.body);
        const float land = ground(Vec{ s.body.x, water + 40.0f, s.body.z });
        const bool on_land = land > water - 2.0f;
        s.rise -= kGravity * kFrameTime;
        s.hop += s.rise * kFrameTime;
        bool landed = false;
        if (s.hop <= 0.0f && s.rise < 0.0f) {
            s.hop = 0.0f;
            landed = true;
            s.velocity = s.velocity * 0.7f;
        }
        const float moving = std::hypot(length(s.velocity), s.rise);
        s.eye = Vec{ s.body.x, (on_land ? land : water) + s.hop + kLying, s.body.z };
        spin(moving);
        spraying = !on_land;
        spray_wet = landed ? 1.0f : 0.0f;
        ++s.frames;
        shown = tumbling();
        if (landed) {
            if (on_land) {
                s.phase = Swim::Settling;
                s.slant = false;
            }
            else {
                s.phase = Swim::Sinking;
                s.under = s.sink_after;
                s.drift = s.velocity * 0.5f;
            }
        }
        break;
    }
    case Swim::Sinking: {
        // Under after a hard throw: sinking steadily, easing to a stop over
        // its last third of a second, carried on by the throw a little way,
        // tumbling, turning towards the jet ski unseen; never through the
        // seabed.
        --s.under;
        turn_to(stern);
        const Vec before = s.eye;
        if (in_solid(s.eye + s.drift * kFrameTime)) s.drift = Vec{};
        s.eye = s.eye + s.drift * kFrameTime;
        s.drift = s.drift * 0.9f;
        s.eye.y -= kSinkRate * kFrameTime * clamp01(float(s.under) / 10.0f);
        s.eye.y = std::max(s.eye.y, ground(s.eye) + 15.0f);
        const float moving = length(s.eye - before) / kFrameTime;
        spin(moving);
        ++s.frames;
        shown = tumbling();
        if (s.under <= 0 || (s.frames > kEaseIn && moving < 8.0f)) {
            s.phase = Swim::Settling;
            s.slant = true;
        }
        break;
    }
    case Swim::Settling: {
        // The body has stopped: the tumble finishes the turn it is on, then
        // the swim begins.
        const float upright = std::ceil(s.spin / 6.2831853f - 1e-4f) * 6.2831853f;
        const float step = std::max(kSettleSpin, s.spin_rate) * kFrameTime;
        s.spin = std::min(upright, s.spin + step);
        ++s.frames;
        shown = tumbling();
        if (s.spin >= upright - 1e-4f) {
            start_swim(s.slant);
            s.spin = 0.0f;
        }
        break;
    }
    case Swim::Swimming: {
        // The jet ski still coasting into the swimmer pushes it aside.
        const float struck = obstacles.ski(s.eye);
        if (struck < 0.5f * kClearance) {
            constexpr float e = 2.0f;
            const Vec away{ obstacles.ski(s.eye + Vec{ e, 0.0f, 0.0f }) - obstacles.ski(s.eye - Vec{ e, 0.0f, 0.0f }), 0.0f,
                            obstacles.ski(s.eye + Vec{ 0.0f, 0.0f, e }) - obstacles.ski(s.eye - Vec{ 0.0f, 0.0f, e }) };
            if (length(away) > 1e-3f) s.eye = s.eye + normalised(away) * (0.5f * kClearance - struck);
        }
        // Making for the stern the shortest way round anything solid:
        // planned again when the jet ski has moved or turned, at most three
        // times a second.
        // The stern, or where the jet ski has drifted into something solid
        // (backed into a ramp, say), the nearest place to it outside.
        const Vec goal = stepped_out(stern, 0.5f * kClearance + 2.0f,
                                     [&](Vec q) { return course_clearance(course, q.x, q.z); });
        if (s.route.empty() ||
            ((length(Vec{ stern.x - s.routed_to.x, 0.0f, stern.z - s.routed_to.z }) > 40.0f || dot(bow, s.routed_bow) < 0.95f) &&
             s.frames - s.routed_at >= 10)) {
            s.route = plan_route(obstacles, s.eye, goal);
            s.leg = 0;
            s.routed_to = stern;
            s.routed_bow = bow;
            s.routed_at = s.frames;
            if (log && s.route.size() > 1) {
                std::fprintf(stderr, "[wr64-swim] going round: %zu legs\n", s.route.size());
                std::fflush(stderr);
            }
        }
        // On past each corner once reached, or once the next is in sight.
        while (s.leg + 1 < s.route.size() &&
               (length(Vec{ s.route[s.leg].x - s.eye.x, 0.0f, s.route[s.leg].z - s.eye.z }) < 24.0f ||
                clear_between(obstacles, s.eye, s.route[s.leg + 1]))) {
            ++s.leg;
        }
        const Vec making_for = s.leg + 1 < s.route.size() ? s.route[s.leg] : goal;
        const float left = turn_to(making_for);
        const float distance = length(Vec{ making_for.x - s.eye.x, 0.0f, making_for.z - s.eye.z });
        const float remaining = length(Vec{ goal.x - s.eye.x, 0.0f, goal.z - s.eye.z });
        // On solid ground (or wading where it is shallower than a standing
        // rider), walking with the eye at standing height; otherwise
        // swimming, the eye just above the waves. Walking off the edge of the
        // course's collision -- which can end above the water while the
        // beach drawn goes on down into it -- the ground goes on down at a
        // beach's slope from where it was, not dropping at once.
        const float water = surface(s.eye);
        const float land = std::max(ground(s.eye), s.underfoot);
        const bool standing = land + s.stand > water + eye_height;
        const float target = standing ? land + s.stand : water + eye_height;
        // Moving the way it faces, slowed while still turning far round.
        const float pace = clamp01(std::cos(left) * 1.5f - 0.5f);
        const float move = std::min(distance, (standing ? walk : speed) * kFrameTime * pace);
        // Never into anything solid, the jet ski included: sliding along it
        // instead, if it can; from too near, only away. The last few strokes
        // to the stern may come as near the jet ski as they need.
        const Vec ahead = s.facing * move;
        const bool last_strokes = remaining < 3.0f * kClearance;
        auto room_at = [&](Vec q) { return last_strokes ? course_clearance(course, q.x, q.z) : obstacles.clearance(q); };
        const float room = room_at(s.eye);
        auto may = [&](Vec to) {
            const float there = room_at(to);
            return there >= 0.5f * kClearance || there > room;
        };
        if (may(s.eye + ahead)) s.eye = s.eye + ahead;
        else if (may(s.eye + Vec{ ahead.x, 0.0f, 0.0f })) s.eye.x += ahead.x;
        else if (may(s.eye + Vec{ 0.0f, 0.0f, ahead.z })) s.eye.z += ahead.z;
        s.underfoot = standing ? land - 0.5f * move : -1e9f;
        // Coming up from a sink, at a slant towards the jet ski rather than
        // straight up; standing up, or climbing out of the water, at once.
        float up = kRiseRate * kFrameTime;
        if (s.slant && !standing && s.eye.y < water - 2.0f) up = std::max(0.3f * kRiseRate * kFrameTime, 0.8f * move);
        else s.slant = false;
        s.eye.y += std::clamp(target - s.eye.y, -kRiseRate * kFrameTime, up);
        s.eye.y = std::max(s.eye.y, land + 15.0f);
        if (remaining - move < kArrived || ++s.frames > kGiveUp) {
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
    // The spray is thrown up from a little behind its source and out to the
    // sides: from just ahead of the body, so the swimmer sees it, not behind
    // the eye or too near to draw.
    const Vec spray_at = s.body + (length(s.velocity) > 1.0f ? normalised(s.velocity) * 45.0f : Vec{});
    water::set_body_spray(spraying, spray_at.x, surface(spray_at), spray_at.z, spray_wet);
    // And the game's own bow splash: kept while riding, thrown up round the
    // skidding body (src/body_splash.cpp).
    const Vec at_hull = row(hull, 3);
    body_splash_note(s.phase == Swim::Riding && animation == kRiding, at_hull.x, at_hull.z, surface(at_hull), bow.x, bow.z,
                     s.phase == Swim::Skidding || s.phase == Swim::Bouncing, s.body.x, s.body.z, surface(s.body),
                     s.velocity.x, s.velocity.z, spray_wet);
    swim_set_holding(s.phase == Swim::Thrown || s.phase == Swim::Skidding || s.phase == Swim::Bouncing || s.phase == Swim::Sinking ||
                     s.phase == Swim::Settling || s.phase == Swim::Swimming);
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
    const uint32_t course = *reinterpret_cast<const uint32_t*>(rdram + 0xD8170);
    const Camera camera = swim_step(segment3, course, animation, ridden, hull);
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

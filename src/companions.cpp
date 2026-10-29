// Fellow riders out on Dolphin Park, having fun on the water while you warm
// up -- one, for now, as a proof of concept: a real other racer riding a
// loop round the park, hitting the ramp at full speed and barrel-rolling
// over it.
//
// The racer: one of the three the player did not pick, by chance. Warm Up
// draws only the player's rider; the game loads the others for it but then
// lays the course's data over them (measured from dumps of RDRAM in a race
// and in Warm Up, WR64_RAM_DUMP). So the racer is loaded here, as the game
// loads it for a race: each character's models are a pair of MIO0 chunks in
// the cartridge, a full one for the player's rider and a smaller one for a
// computer's, listed in the game's table at kChunks; the smaller is unpacked
// here into kModel. Its addresses are its own (segment 8 counting from the
// chunk's start), and are set to where it lies, as the game sets them to
// where it loads it (relocate). Its twelve
// parts' lists -- hull, handlebars, a third piece of the jet ski, head,
// chest, hips, then upper arm, forearm, hand, thigh, shin and foot -- are in
// the game's templates at kTemplates, three for each character (full, and
// two of the smaller). The game draws a rider as eighteen parts, the limbs'
// six twice, the right side mirrored by its matrices. (The third character
// has five parts more, its hair, say, not yet drawn.) The player's character
// is told by its rider's hull list, which the game set from its full
// template.
//
// Its pose. Each part's matrix is the player's rider's, relative to the
// player's hull, kept while the player rides upright (copied before first
// person shrinks the body away), set on the companion's own hull. So it
// rides as the player's rider rides, crouched over the bars; its own leaning,
// pitching and rolling are its hull's.
//
// Its run. A loop east along z = 3560 over the hump of the ramp at x 1033 to
// 1849 (crest 90 units up at x 1441), on through the gap in the stone arches,
// round to the west along z = 6200 past the island, and back round; all at
// full speed (kSpeed), inside the buoys, clear of the other ramp, the piers
// and the island. Up the ramp's face it climbs with it; from the crest it
// flies, popping up at kPop and falling at kGravity, and rolls once over the
// way it goes while in the air; on the water it rides the waves and leans
// into the turns.
//
// Drawn just after the game draws the player's rider (its list 0x02000000),
// in the state that leaves. Memory: its matrices at kMatrices + 0x800 b, its
// chunk read at kPacked and unpacked at kModel.
//
// WR64_COMPANIONS=0 leaves the park to the player; WR64_COMPANION_LOG=1
// reports where it is now and then.

#include "recomp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

namespace wr64 {

// patches/water_all_round.cpp: the waves' height at a point.
float water_height_at(uint32_t segment3, float x, float z);
// src/body_splash.cpp: the companion's jet ski this frame, for its spray.
void body_splash_companion(bool on_water, float x, float z, float water, float ahead_x, float ahead_z, float speed);

}  // namespace wr64

namespace recomp {
void do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes);
}

namespace wr64 {

namespace {

constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };
constexpr int kParts = 18, kUniqueParts = 12;
constexpr uint32_t kMatrices = 0x520000, kMatrixStride = 0x800;
constexpr uint32_t kPacked = 0x530000, kPackedSize = 0x10000;
// Where the unpacked racer goes, above the game's 4 MB with the rest of the
// port's own data, and kOwned marking that area as ours.
constexpr uint32_t kOwned = 0x53F000, kModel = 0x540000, kModelEnd = 0x55D000;
constexpr uint32_t kOwnedMark = 0x434F4D50u;           // "COMP"
// USA Rev A, physical: the characters' chunks (full, smaller, 4, 0 for each),
// their part templates (17 words, 0x44 bytes, three for each), and the
// player's rider's parts' lists as the game set them.
constexpr uint32_t kChunks = 0x0DABD0, kTemplates = 0x0DACD0, kTemplateSize = 0x44;
constexpr uint32_t kPlayerParts = 0x227A60;
constexpr int kCharacters = 4;
constexpr uint32_t kRomBase = 0x10000000;
constexpr int32_t kRiding = 22;

// The run: the straights' ends and heights (z), the turns' radius, the speed
// (units a second, about 110 km/h), the ramp.
constexpr float kWest = -3000.0f, kEast = 4300.0f, kOut = 3560.0f, kBack = 6200.0f;
constexpr float kRadius = (kBack - kOut) / 2.0f;
constexpr float kSpeed = 1000.0f;
constexpr float kRampStart = 1033.0f, kRampCrest = 1441.0f, kRampTop = 90.0f;
constexpr float kRampNear = 3112.0f, kRampFar = 4126.0f;
// The ramp's face up to the crest: x and height, as measured.
constexpr float kFaceX[] = { 1033, 1273, 1373, 1433, 1441 };
constexpr float kFaceY[] = { -30, 30, 80, 90, 90 };
// Off the crest: the pop up and the fall (units a second, and squared), and
// the roll's start and length in the air (seconds).
constexpr float kPop = 330.0f, kGravity = 400.0f;
constexpr float kRollStart = 0.35f, kRollLength = 1.1f;
constexpr float kLean = 0.5f;                       // radians, into the turns

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
        const int16_t high = int16_t(fixed >> 16);
        const uint16_t low = uint16_t(fixed & 0xFFFF);
        std::memcpy(rdram + ((address + i * 2) ^ 2), &high, 2);
        std::memcpy(rdram + ((address + 32 + i * 2) ^ 2), &low, 2);
    }
}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// The inverse of an affine matrix (rows the axes, the last the place).
Matrix inverse_affine(const Matrix& a) {
    const float (*m)[4] = a.m;
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    Matrix r{};
    if (std::fabs(det) < 1e-9f) return r;
    const float s = 1.0f / det;
    r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * s;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * s;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * s;
    r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * s;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * s;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * s;
    r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * s;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * s;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * s;
    for (int j = 0; j < 3; ++j) {
        r.m[3][j] = -(m[3][0] * r.m[0][j] + m[3][1] * r.m[1][j] + m[3][2] * r.m[2][j]);
    }
    r.m[3][3] = 1.0f;
    return r;
}

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_COMPANIONS");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

// The player's rider's riding pose: each part relative to its hull, and the
// hull's scale.
struct Pose {
    bool ready = false;
    Matrix parts[kParts];
    float scale = 1.0f;
    float handed = 1.0f;    // +1 if the hull's x is its y cross its z, -1 if the other way
    Matrix hull{};          // the player's hull, as last seen riding
} g_pose;

// RDRAM's bytes as the game sees them (big-endian words kept native).
uint8_t byte_at(const uint8_t* rdram, uint32_t address) { return rdram[(address & 0x7FFFFF) ^ 3]; }
uint32_t word_at(const uint8_t* rdram, uint32_t address) {
    return *reinterpret_cast<const uint32_t*>(rdram + (address & 0x7FFFFC));
}

// Unpacks the MIO0 chunk at physical `from` to `to`, at most `room` bytes;
// returns its length, or 0 if it is not one or does not fit.
uint32_t unpack_mio0(uint8_t* rdram, uint32_t from, uint32_t to, uint32_t room) {
    if (word_at(rdram, from) != 0x4D494F30u) return 0;    // "MIO0"
    const uint32_t size = word_at(rdram, from + 4);
    uint32_t pairs = from + word_at(rdram, from + 8), raw = from + word_at(rdram, from + 12);
    if (size == 0 || size > room) return 0;
    uint32_t bits = from + 16, word = 0, mask = 0, out = 0;
    while (out < size) {
        if (mask == 0) {
            word = word_at(rdram, bits);
            bits += 4;
            mask = 0x80000000u;
        }
        if (word & mask) {
            rdram[(to + out) ^ 3] = byte_at(rdram, raw++);
            ++out;
        }
        else {
            const uint32_t pair = (uint32_t(byte_at(rdram, pairs)) << 8) | byte_at(rdram, pairs + 1);
            pairs += 2;
            const uint32_t length = (pair >> 12) + 3, back = (pair & 0xFFF) + 1;
            if (back > out) return 0;
            for (uint32_t i = 0; i < length && out < size; ++i, ++out) {
                rdram[(to + out) ^ 3] = rdram[(to + out - back) ^ 3];
            }
        }
        mask >>= 1;
    }
    return size;
}

// Sets the list at `list` (an offset into the unpacked chunk of `size`
// bytes) and those it calls to address the chunk where it lies, kModel, as
// the game sets a racer's to where it loads it: each command naming segment
// 8 -- a vertex load, a texture, a call -- is given its place in KSEG0.
void relocate(uint8_t* rdram, uint32_t list, uint32_t size, int depth = 0) {
    if (depth > 4) return;
    for (uint32_t at = kModel + list; at + 8 <= kModel + size; at += 8) {
        uint32_t* w = reinterpret_cast<uint32_t*>(rdram + at);
        const uint8_t op = uint8_t(w[0] >> 24);
        if (op == 0xB8) return;
        if ((w[1] >> 24) == 0x08 && (w[1] & 0xFFFFFF) < size) {
            if (op == 0x06) relocate(rdram, w[1] & 0xFFFFFF, size, depth + 1);
            w[1] = 0x80000000u | (kModel + (w[1] & 0xFFFFFF));
        }
    }
}

// The racer riding with the player: which character, and its parts' lists
// in its unpacked chunk.
struct Racer {
    int character = -1;
    bool ready = false;
    uint32_t lists[kUniqueParts]{};
} g_racer;

// Which character the player rides, from its rider's hull list: the game set
// it from the character's full template, the chunk 0x10000 into segment 8.
int player_character(const uint8_t* rdram) {
    const uint32_t hull = word_at(rdram, kPlayerParts);
    for (int c = 0; c < kCharacters; ++c) {
        const uint32_t full = word_at(rdram, kTemplates + uint32_t(c * 3) * kTemplateSize);
        if ((full >> 24) == 0x08 && hull == ((full & 0xFFFFFFu) + 0x08010000u)) return c;
    }
    return -1;
}

// Loads character `c`'s smaller chunk from the cartridge and unpacks it;
// whether its parts' lists are all there.
bool load_racer(uint8_t* rdram, int c) {
    const uint32_t rom = word_at(rdram, kChunks + uint32_t(c) * 16 + 4);
    if (rom == 0 || rom >= 0x01000000u) return false;
    // The area must be empty, or ours already.
    uint32_t& mark = *reinterpret_cast<uint32_t*>(rdram + kOwned);
    if (mark != kOwnedMark) {
        for (uint32_t a = kOwned; a < kModelEnd; a += 4) {
            if (*reinterpret_cast<const uint32_t*>(rdram + a) != 0) {
                std::fprintf(stderr, "[wr64-companion] memory at 0x%06X is in use; no companion\n", a);
                std::fflush(stderr);
                return false;
            }
        }
        mark = kOwnedMark;
    }
    recomp::do_rom_read(rdram, int32_t(0x80000000u | kPacked), kRomBase | rom, kPackedSize);
    const uint32_t size = unpack_mio0(rdram, kPacked, kModel, kModelEnd - kModel);
    if (size == 0) return false;
    const uint32_t record = kTemplates + uint32_t(c * 3 + 1) * kTemplateSize;
    for (int p = 0; p < kUniqueParts; ++p) {
        const uint32_t list = word_at(rdram, record + uint32_t(p) * 4);
        if ((list >> 24) != 0x08 || (list & 0xFFFFFF) + 8 > size || word_at(rdram, kModel + (list & 0xFFFFFF)) != 0xE7000000u) {
            return false;
        }
        g_racer.lists[p] = 0x80000000u | (kModel + (list & 0xFFFFFF));
    }
    // Each list once (the limbs' are drawn twice from the same).
    for (int p = 0; p < kUniqueParts; ++p) relocate(rdram, g_racer.lists[p] - (0x80000000u | kModel), size);
    return true;
}

// Where the run has the jet ski `along` units round its loop: its place
// (height aside), heading (x and z of the way it goes), and how hard it turns
// (+1 left, -1 right, 0 straight).
struct Track { float x, z, hx, hz, turn; bool outbound; };
Track track(float along) {
    const float straight = kEast - kWest, half = 3.14159265f * kRadius;
    const float loop = 2.0f * (straight + half);
    float s = std::fmod(along, loop);
    if (s < 0.0f) s += loop;
    if (s < straight) return { kWest + s, kOut, 1.0f, 0.0f, 0.0f, true };
    s -= straight;
    if (s < half) {
        const float a = -1.5707963f + s / kRadius;  // round the east end
        return { kEast + kRadius * std::cos(a), (kOut + kBack) / 2.0f + kRadius * std::sin(a), -std::sin(a), std::cos(a), 1.0f, false };
    }
    s -= half;
    if (s < straight) return { kEast - s, kBack, -1.0f, 0.0f, 0.0f, false };
    s -= straight;
    const float a = 1.5707963f + s / kRadius;        // round the west end
    return { kWest + kRadius * std::cos(a), (kOut + kBack) / 2.0f + kRadius * std::sin(a), -std::sin(a), std::cos(a), 1.0f, false };
}

float face_height(float x) {
    for (int i = 0; i + 1 < int(sizeof(kFaceX) / sizeof(kFaceX[0])); ++i) {
        if (x <= kFaceX[i + 1]) {
            const float t = (x - kFaceX[i]) / (kFaceX[i + 1] - kFaceX[i]);
            return kFaceY[i] + t * (kFaceY[i + 1] - kFaceY[i]);
        }
    }
    return kRampTop;
}


// ---- driving -----------------------------------------------------------
//
// It drives itself: steering for a point kLookahead units on along whichever
// route it follows, turning at most kTurnRate, gathering speed to kSpeed.
// Following a route it keeps to it; told to change, it simply steers for the
// other, and so sweeps onto it in a natural curve at speed, never stopping.
// Its two routines: the stunt loop over the ramp, and a lap of the park's
// edge (a circle of kLapRadius about the middle, inside the buoys and clear
// of the island and the arches), each kept for 60 to 200 seconds by chance.
// The change comes where the two meet, in the park's north-east, so the
// curve between them is short and over open water.
//
// Off the ramp's crest it flies and turns once in the air -- a roll either
// way, or a flip forwards or backwards, a quarter's chance each -- and one
// time in five gets it wrong: three-quarters round, it lands badly, the
// rider thrown off ahead, tumbling into the water and under, while the jet
// ski slews to a stop; five seconds on, the rider is back on and away.
enum class Routine { Stunts, Lap };
enum class Crash { None, Thrown, Waiting };
struct Drive {
    std::chrono::steady_clock::time_point last{}, change_at{}, crash_at{};
    Routine routine = Routine::Stunts;
    bool change = false;
    float x = 0, y = 0, z = 0, heading = 0, speed = 0, climb = 0, bank = 0, flip = 0, turning = 0;
    int nearest = -1;
    bool on_ramp = false, airborne = false, failed = false;
    float rise = 0, air = 0;
    int stunt = 0;
    Crash crash = Crash::None;
    float body_x = 0, body_y = 0, body_z = 0, body_hx = 1, body_hz = 0, body_spin = 0, body_rise = 0, body_speed = 0;
} g_drive;

constexpr float kLookahead = 900.0f, kTurnRate = 1.1f, kAccel = 450.0f, kSample = 50.0f;
constexpr float kLapRadius = 7000.0f;

std::mt19937& chance() {
    static std::mt19937 engine{ std::random_device{}() };
    return engine;
}
float uniform(float lo, float hi) { return lo + (hi - lo) * float(chance()() % 100000u) / 100000.0f; }

// WR64_COMPANION_TEST_CHANGE=<seconds>, in a scripted run only: each routine
// kept that long instead of 60 to 200 seconds.
std::chrono::milliseconds routine_length() {
    static const float test = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_CHANGE");
        return value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr ? float(std::atof(value)) : 0.0f;
    }();
    return std::chrono::milliseconds(int((test > 0.0f ? test : uniform(60.0f, 200.0f)) * 1000.0f));
}

// A routine's route as points kSample apart, in the way it is driven.
const std::vector<std::pair<float, float>>& route(Routine r) {
    static const std::vector<std::pair<float, float>> stunts = [] {
        std::vector<std::pair<float, float>> v;
        const float loop = 2.0f * ((kEast - kWest) + 3.14159265f * kRadius);
        for (float s = 0.0f; s < loop; s += kSample) { const Track t = track(s); v.emplace_back(t.x, t.z); }
        return v;
    }();
    static const std::vector<std::pair<float, float>> lap = [] {
        std::vector<std::pair<float, float>> v;
        const int n = int(6.2831853f * kLapRadius / kSample);
        for (int i = 0; i < n; ++i) {
            const float a = 6.2831853f * float(i) / float(n);
            v.emplace_back(kLapRadius * std::cos(a), kLapRadius * std::sin(a));
        }
        return v;
    }();
    return r == Routine::Stunts ? stunts : lap;
}

void start_drive(std::chrono::steady_clock::time_point now) {
    Drive& d = g_drive;
    d = Drive{};
    d.last = now;
    d.routine = Routine::Stunts;
    d.change_at = now + routine_length();
    const Track t = track(0.0f);
    d.x = t.x; d.z = t.z; d.heading = std::atan2(t.hz, t.hx); d.speed = kSpeed;
}

// A hull's frame: at (x, y, z), heading (hx, hz), pitched up by `pitch`,
// banked about its length by `bank`, and turned head over heels by `flip`.
Matrix frame_matrix(float x, float y, float z, float hx, float hz, float pitch, float bank, float flip) {
    struct V { float x, y, z; };
    auto add = [](V a, V b, float k) { return V{ a.x + b.x * k, a.y + b.y * k, a.z + b.z * k }; };
    auto times = [](V a, float k) { return V{ a.x * k, a.y * k, a.z * k }; };
    const V f{ hx * std::cos(pitch), std::sin(pitch), hz * std::cos(pitch) };
    const V u{ -hx * std::sin(pitch), std::cos(pitch), -hz * std::sin(pitch) };
    const float h = g_pose.handed;
    const V r{ (u.y * f.z - u.z * f.y) * h, (u.z * f.x - u.x * f.z) * h, (u.x * f.y - u.y * f.x) * h };
    // Banked: up and across turned about the length.
    const float cb = std::cos(bank), sb = std::sin(bank);
    const V u2 = add(times(u, cb), r, sb), r2 = add(times(r, cb), u, -sb);
    // Flipped: the length and up turned about the across.
    const float cf = std::cos(flip), sf = std::sin(flip);
    const V f3 = add(times(f, cf), u2, -sf), u3 = add(times(u2, cf), f, sf);
    Matrix m{};
    const float s = g_pose.scale;
    m.m[0][0] = r2.x * s; m.m[0][1] = r2.y * s; m.m[0][2] = r2.z * s;
    m.m[1][0] = u3.x * s; m.m[1][1] = u3.y * s; m.m[1][2] = u3.z * s;
    m.m[2][0] = f3.x * s; m.m[2][1] = f3.y * s; m.m[2][2] = f3.z * s;
    m.m[3][0] = x; m.m[3][1] = y; m.m[3][2] = z; m.m[3][3] = 1.0f;
    return m;
}

void drive(uint32_t segment3, std::chrono::steady_clock::time_point now, float dt) {
    Drive& d = g_drive;
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    const float water = water_height_at(segment3, d.x, d.z);

    // Time for the other routine: changed where the two meet.
    if (!d.change && now >= d.change_at) d.change = true;
    if (d.change && !d.airborne && d.crash == Crash::None && d.x > 4300.0f && d.z > 3000.0f && d.z < 6300.0f) {
        d.change = false;
        d.routine = d.routine == Routine::Stunts ? Routine::Lap : Routine::Stunts;
        d.nearest = -1;
        d.change_at = now + routine_length();
        if (log) {
            std::fprintf(stderr, "[wr64-companion] off on %s\n", d.routine == Routine::Stunts ? "a stunt run" : "a lap of the park");
            std::fflush(stderr);
        }
    }

    // A bail: the rider thrown off ahead, then under, then back on.
    if (d.crash != Crash::None) {
        const float since = std::chrono::duration<float>(now - d.crash_at).count();
        d.speed = std::max(0.0f, d.speed - 700.0f * dt);
        d.flip *= std::max(0.0f, 1.0f - 6.0f * dt);
        d.bank *= std::max(0.0f, 1.0f - 6.0f * dt);
        d.x += std::cos(d.heading) * d.speed * dt;
        d.z += std::sin(d.heading) * d.speed * dt;
        d.y = water;
        d.climb = 0.0f;
        const float body_water = water_height_at(segment3, d.body_x, d.body_z);
        if (d.crash == Crash::Thrown) {
            d.body_x += d.body_hx * d.body_speed * dt;
            d.body_z += d.body_hz * d.body_speed * dt;
            d.body_rise -= kGravity * dt;
            d.body_y += d.body_rise * dt;
            d.body_spin += 9.0f * dt;
            if (d.body_y <= body_water && d.body_rise < 0.0f) {
                d.crash = Crash::Waiting;
                d.body_speed = 0.0f;
            }
        }
        else {
            d.body_y -= 60.0f * dt;                     // sinking out of sight
        }
        if (since > 5.0f) {
            d.crash = Crash::None;
            d.flip = d.bank = 0.0f;
        }
        return;
    }

    if (d.airborne) {
        // Flying, turning the stunt, then down onto the water.
        d.air += dt;
        d.x += std::cos(d.heading) * d.speed * dt;
        d.z += std::sin(d.heading) * d.speed * dt;
        d.rise -= kGravity * dt;
        d.y += d.rise * dt;
        d.climb = d.rise / std::max(d.speed, 1.0f);
        const float r = std::clamp((d.air - kRollStart) / kRollLength, 0.0f, 1.0f);
        const float turn = 6.2831853f * (d.failed ? 0.72f : 1.0f) * r * r * (3.0f - 2.0f * r);
        d.bank = d.stunt == 0 ? turn : d.stunt == 1 ? -turn : 0.0f;
        d.flip = d.stunt == 2 ? turn : d.stunt == 3 ? -turn : 0.0f;
        if (d.y <= water && d.rise < 0.0f) {
            d.airborne = false;
            d.y = water;
            d.climb = 0.0f;
            if (d.failed) {
                d.crash = Crash::Thrown;
                d.crash_at = now;
                d.body_x = d.x; d.body_y = water + 40.0f; d.body_z = d.z;
                d.body_hx = std::cos(d.heading); d.body_hz = std::sin(d.heading);
                d.body_speed = d.speed * 0.6f;
                d.body_rise = 220.0f;
                d.body_spin = 0.0f;
                if (log) {
                    std::fprintf(stderr, "[wr64-companion] bailed the landing\n");
                    std::fflush(stderr);
                }
            }
            else {
                d.bank = d.flip = 0.0f;
            }
        }
        return;
    }

    // On the water: steering for a point ahead on its route, up to speed.
    d.speed = std::min(kSpeed, d.speed + kAccel * dt);
    const auto& points = route(d.routine);
    const int n = int(points.size());
    auto distance2 = [&](int i) {
        const float dx = points[i].first - d.x, dz = points[i].second - d.z;
        return dx * dx + dz * dz;
    };
    if (d.nearest < 0) {
        d.nearest = 0;
        for (int i = 1; i < n; ++i) if (distance2(i) < distance2(d.nearest)) d.nearest = i;
    }
    else {
        int best = d.nearest;
        for (int k = -10; k <= 40; ++k) {
            const int i = ((d.nearest + k) % n + n) % n;
            if (distance2(i) < distance2(best)) best = i;
        }
        d.nearest = best;
    }
    const auto& target = points[(d.nearest + int(kLookahead / kSample)) % n];
    const float want = std::atan2(target.second - d.z, target.first - d.x);
    const float diff = std::remainder(want - d.heading, 6.2831853f);
    const float step = std::clamp(diff, -kTurnRate * dt, kTurnRate * dt);
    d.heading = std::remainder(d.heading + step, 6.2831853f);
    d.turning += ((dt > 0.0f ? step / dt : 0.0f) - d.turning) * std::min(1.0f, 6.0f * dt);
    d.x += std::cos(d.heading) * d.speed * dt;
    d.z += std::sin(d.heading) * d.speed * dt;
    d.bank = kLean * std::clamp(d.turning / (kSpeed / kRadius), -1.0f, 1.0f);
    d.flip = 0.0f;

    // The ramp: up its face, heading along it on the stunt run, and off the
    // crest into the air with a stunt picked.
    const bool along_ramp = d.routine == Routine::Stunts && std::cos(d.heading) > 0.95f &&
                            d.z > kRampNear + 100.0f && d.z < kRampFar - 100.0f;
    if (along_ramp && d.x >= kRampStart && d.x < kRampCrest) {
        d.on_ramp = true;
        d.y = std::max(water, face_height(d.x));
        d.climb = (face_height(d.x + 10.0f) - face_height(d.x - 10.0f)) / 20.0f;
    }
    else if (d.on_ramp && d.x >= kRampCrest) {
        d.on_ramp = false;
        d.airborne = true;
        d.air = 0.0f;
        d.y = kRampTop;
        d.rise = kPop;
        d.stunt = int(chance()() % 4u);
        d.failed = chance()() % 5u == 0;
        // WR64_COMPANION_TEST_STUNT=<0-3>[,fail], in a scripted run only.
        static const int forced = [] {
            const char* value = std::getenv("WR64_COMPANION_TEST_STUNT");
            return value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr ? (value[0] - '0') + (std::strchr(value, 'f') ? 10 : 0) : -1;
        }();
        if (forced >= 0) { d.stunt = forced % 10; d.failed = forced >= 10; }
        if (log) {
            static const char* names[] = { "a roll left", "a roll right", "a front flip", "a back flip" };
            std::fprintf(stderr, "[wr64-companion] off the ramp: %s%s\n", names[d.stunt], d.failed ? ", going wrong" : "");
            std::fflush(stderr);
        }
    }
    else {
        d.on_ramp = false;
        d.y = water;
        d.climb = 0.0f;
    }
}

}  // namespace

// From src/first_person.cpp, at each race frame's view, before the body is
// shrunk: the player's rider's parts (at physical `parts`, the hull first,
// 0x100 apart), kept while it rides upright.
void companion_note_pose(const uint8_t* rdram, uint32_t parts, int32_t animation) {
    if (animation != kRiding) return;
    const Matrix hull = read_matrix(rdram, parts);
    const float scale = std::sqrt(hull.m[1][0] * hull.m[1][0] + hull.m[1][1] * hull.m[1][1] + hull.m[1][2] * hull.m[1][2]);
    if (!(scale > 1e-4f && scale < 100.0f)) return;
    const Matrix to_hull = inverse_affine(hull);
    for (int p = 0; p < kParts; ++p) g_pose.parts[p] = multiply(read_matrix(rdram, parts + uint32_t(p) * 0x100), to_hull);
    g_pose.scale = scale;
    g_pose.hull = hull;
    const float (*m)[4] = hull.m;
    const float cx = m[1][1] * m[2][2] - m[1][2] * m[2][1], cy = m[1][2] * m[2][0] - m[1][0] * m[2][2],
                cz = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    g_pose.handed = m[0][0] * cx + m[0][1] * cy + m[0][2] * cz >= 0.0f ? 1.0f : -1.0f;
    g_pose.ready = true;
}

// For the display-list rewriter, just after the player's rider is drawn on
// course `course`: the commands drawing the companions, in `out`. `segments`
// are the frame's.
void companions_draw(uint8_t* rdram, const uint32_t segments[16], uint32_t course,
                     std::vector<std::pair<uint32_t, uint32_t>>& out) {
    if (!enabled() || course != 0 || !g_pose.ready) return;
    int b = -1;
    for (int i = 0; i < 2; ++i) {
        if (segments[3] == kFrameBuffers[i]) b = i;
    }
    if (b < 0) return;
    // A new outing -- the first frame drawn, or the first after a while
    // without -- brings a racer of its own, one the player is not, by chance,
    // and starts its run again.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{};
    const Clock::time_point now = Clock::now();
    const bool fresh = g_racer.character < 0 || now - last > std::chrono::seconds(2);
    if (fresh) {
        const int player = player_character(rdram);
        static std::mt19937 chance{ std::random_device{}() };
        int c = int(chance() % uint32_t(kCharacters - 1));
        if (player >= 0 && c >= player) ++c;
        g_racer.character = c;
        g_racer.ready = load_racer(rdram, c);
        std::fprintf(stderr, "[wr64-companion] character %d rides with the player (character %d)%s\n", c, player,
                     g_racer.ready ? "" : ": its models did not load");
        std::fflush(stderr);
    }
    last = now;
    if (!g_racer.ready) return;
    const uint32_t* lists = g_racer.lists;
    (void)lists;

    // Its driving this frame.
    const float dt = std::clamp(std::chrono::duration<float>(now - g_drive.last).count(), 0.0f, 0.1f);
    g_drive.last = now;
    if (fresh) start_drive(now);
    drive(segments[3], now, dt);
    const Drive& d = g_drive;
    const float hx = std::cos(d.heading), hz = std::sin(d.heading);
    Matrix hull = frame_matrix(d.x, d.y, d.z, hx, hz, std::atan(d.climb), d.bank, d.flip);
    Matrix body = d.crash == Crash::None ? hull : frame_matrix(d.body_x, d.body_y, d.body_z, d.body_hx, d.body_hz, 0.0f, 0.0f, d.body_spin);
    const bool body_shown = d.crash == Crash::None || d.body_y > water_height_at(segments[3], d.body_x, d.body_z) - 80.0f;
    // The spray round its hull, on the water and moving (src/body_splash.cpp).
    body_splash_companion(!d.airborne && d.crash != Crash::Waiting, d.x, d.z,
                          water_height_at(segments[3], d.x, d.z), hx, hz, d.speed);
    // WR64_COMPANION_TEST_OFFSET=<dx>,<dz>, in a scripted run only: drawn
    // that far from where it rides, to bring it before a test's camera.
    static const std::pair<float, float> offset = [] {
        std::pair<float, float> o{ 0.0f, 0.0f };
        const char* value = std::getenv("WR64_COMPANION_TEST_OFFSET");
        if (value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr) std::sscanf(value, "%f,%f", &o.first, &o.second);
        return o;
    }();
    for (Matrix* m : { &hull, &body }) { m->m[3][0] += offset.first; m->m[3][2] += offset.second; }
    // WR64_COMPANION_TEST_BESIDE=1, in a scripted run only: drawn beside the
    // player's jet ski instead, 400 units ahead and 200 to its left.
    static const bool beside = std::getenv("WR64_COMPANION_TEST_BESIDE") != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
    if (beside) {
        hull = g_pose.hull;
        const float side = 200.0f / g_pose.scale, ahead = 400.0f / g_pose.scale;
        for (int c2 = 0; c2 < 3; ++c2) hull.m[3][c2] += hull.m[0][c2] * side + hull.m[2][c2] * ahead;
        body = hull;
    }

    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    static int frames = 0;
    if (log && frames++ % 40 == 0) {
        std::fprintf(stderr, "[wr64-companion] at %.0f %.0f %.0f, %s, speed %.0f, heading %.2f\n", d.x, d.y, d.z,
                     d.routine == Routine::Stunts ? "stunts" : "lap", d.speed, d.heading);
        std::fflush(stderr);
    }

    const uint32_t base = kMatrices + uint32_t(b) * kMatrixStride;
    for (int p = 0; p < kParts; ++p) {
        const uint32_t address = base + uint32_t(p) * 64;
        // The jet ski's parts (0 to 2) on its hull; the rider's on the body,
        // apart from it after a bail, and gone once under.
        if (p >= 3 && !body_shown) continue;
        write_matrix(rdram, address, multiply(g_pose.parts[p], p < 3 ? hull : body));
        out.emplace_back(0x01020040u, 0x80000000u | address);
        out.emplace_back(0x06000000u, lists[p < kUniqueParts ? p : p - 6]);
    }
}

}  // namespace wr64

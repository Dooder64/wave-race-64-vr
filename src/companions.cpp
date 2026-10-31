// Fellow riders out on Dolphin Park, having fun on the water while you warm
// up: the three racers the player did not pick. One rides a loop round the
// park, hitting the ramp at full speed and barrel-rolling over it, or laps
// the park's edge; the other two ride together (the pair, below), touring
// the park's sights and weaving round each other in the open water. All
// three swerve away from the player, and the pair from the first.
//
// The racers: the three the player did not pick, in an order by chance. Warm Up
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
// in the state that leaves. Memory: the first's matrices at kMatrices +
// 0x800 b, its chunk unpacked at kModel; the pair's in the area at
// kPairArea (kSlots), with their spray; each chunk read at kPacked first.
//
// WR64_COMPANIONS=0 leaves the park to the player; WR64_COMPANION_LOG=1
// reports where it is now and then.

#include "recomp.h"

#include <algorithm>
#include <atomic>
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
// src/body_splash.cpp: companion `index`'s jet ski this frame, for its spray.
void body_splash_companion(int index, bool on_water, float x, float z, float water, float ahead_x, float ahead_z,
                           float speed);

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
// The pair's: an area of its own (its spray, src/body_splash.cpp, from
// 0x5A0000), marked at its start. Each rider's model (a smaller chunk
// unpacks to at most 0x9F08 bytes) and matrices.
constexpr uint32_t kPairArea = 0x580000, kPairAreaEnd = 0x5C0000;
constexpr uint32_t kPairMark = 0x50414952u;            // "PAIR"
struct Slot { uint32_t model, model_end, matrices; };
constexpr Slot kSlots[3] = {
    { kModel, kModelEnd, kMatrices },
    { 0x581000, 0x58C000, 0x598000 },
    { 0x58C000, 0x597000, 0x599000 },
};
// USA Rev A, physical: the characters' chunks (full, smaller, 4, 0 for each),
// their part templates (17 words, 0x44 bytes, three for each), and the
// player's rider's parts' lists as the game set them.
constexpr uint32_t kChunks = 0x0DABD0, kTemplates = 0x0DACD0, kTemplateSize = 0x44;
constexpr uint32_t kPlayerParts = 0x227A60;
constexpr int kCharacters = 4;
constexpr uint32_t kRomBase = 0x10000000;
constexpr int32_t kRiding = 22;
// gGameModes, and Stunt Mode's.
constexpr uint32_t kGameModes = 0x1CE620, kStuntMode = 11;

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

// The settings' switch (Free Roam tab); WR64_COMPANIONS=0 overrides it.
std::atomic<bool> g_wanted{true};

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_COMPANIONS");
        return value == nullptr || value[0] != '0';
    }();
    return on && g_wanted.load(std::memory_order_relaxed);
}

// The player's rider's riding pose: each part relative to its hull, and the
// hull's scale.
struct Pose {
    bool ready = false;
    Matrix parts[kParts];
    float scale = 1.0f;
    float handed = 1.0f;    // +1 if the hull's x is its y cross its z, -1 if the other way
    float x_right = 1.0f;   // +1 if the hull's x points to the rider's right, -1 to its left
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

// Sets the list at `list` (an offset into the chunk of `size` bytes
// unpacked at `model`) and those it calls to address the chunk where it
// lies, as the game sets a racer's to where it loads it: each command naming
// segment 8 -- a vertex load, a texture, a call -- is given its place in
// KSEG0.
void relocate(uint8_t* rdram, uint32_t model, uint32_t list, uint32_t size, int depth = 0) {
    if (depth > 4) return;
    for (uint32_t at = model + list; at + 8 <= model + size; at += 8) {
        uint32_t* w = reinterpret_cast<uint32_t*>(rdram + at);
        const uint8_t op = uint8_t(w[0] >> 24);
        if (op == 0xB8) return;
        if ((w[1] >> 24) == 0x08 && (w[1] & 0xFFFFFF) < size) {
            if (op == 0x06) relocate(rdram, model, w[1] & 0xFFFFFF, size, depth + 1);
            w[1] = 0x80000000u | (model + (w[1] & 0xFFFFFF));
        }
    }
}

// The racers riding with the player: which character, and its parts' lists
// in its unpacked chunk. The first rides alone, the other two as the pair.
struct Racer {
    int character = -1;
    bool ready = false;
    uint32_t lists[kUniqueParts]{};
} g_racers[3];

// Whether the area from `from` to `to` is ours: marked so at `from`, or
// empty, and so marked now.
bool claim(uint8_t* rdram, uint32_t from, uint32_t to, uint32_t mark_value) {
    uint32_t& mark = *reinterpret_cast<uint32_t*>(rdram + from);
    if (mark == mark_value) return true;
    for (uint32_t a = from; a < to; a += 4) {
        if (*reinterpret_cast<const uint32_t*>(rdram + a) != 0) {
            std::fprintf(stderr, "[wr64-companion] memory at 0x%06X is in use; no companion there\n", a);
            std::fflush(stderr);
            return false;
        }
    }
    mark = mark_value;
    return true;
}

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

// Loads character `c`'s smaller chunk from the cartridge and unpacks it into
// slot `slot`; whether its parts' lists are all there.
bool load_racer(uint8_t* rdram, int c, int slot) {
    const uint32_t rom = word_at(rdram, kChunks + uint32_t(c) * 16 + 4);
    if (rom == 0 || rom >= 0x01000000u) return false;
    // The area must be empty, or ours already.
    if (slot == 0 ? !claim(rdram, kOwned, kModelEnd, kOwnedMark) : !claim(rdram, kPairArea, kPairAreaEnd, kPairMark)) {
        return false;
    }
    const uint32_t model = kSlots[slot].model;
    recomp::do_rom_read(rdram, int32_t(0x80000000u | kPacked), kRomBase | rom, kPackedSize);
    const uint32_t size = unpack_mio0(rdram, kPacked, model, kSlots[slot].model_end - model);
    if (size == 0) return false;
    Racer& racer = g_racers[slot];
    const uint32_t record = kTemplates + uint32_t(c * 3 + 1) * kTemplateSize;
    for (int p = 0; p < kUniqueParts; ++p) {
        const uint32_t list = word_at(rdram, record + uint32_t(p) * 4);
        if ((list >> 24) != 0x08 || (list & 0xFFFFFF) + 8 > size || word_at(rdram, model + (list & 0xFFFFFF)) != 0xE7000000u) {
            return false;
        }
        racer.lists[p] = 0x80000000u | (model + (list & 0xFFFFFF));
    }
    // Each list once (the limbs' are drawn twice from the same).
    for (int p = 0; p < kUniqueParts; ++p) relocate(rdram, model, racer.lists[p] - (0x80000000u | model), size);
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
//
// In Stunt Mode the park is laid out for it -- rings, checkpoints, and four
// ramps of its own, none where the stunt loop's is -- so it practises
// instead, for as long as the player rides: laps of the stunt course, in its
// checkpoints' order, through the rings and over every ramp (kPractice).
enum class Routine { Stunts, Lap, Practice };
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

// Whether this outing is Stunt Mode's (gGameModes, 11), set as it starts.
bool g_stunt = false;

// A ramp as ridden: its crest's middle, the way over it, and how far either
// side of its middle a jet ski is on it. All the park's ramps are one hump,
// the same either way (kFaceX/kFaceY from the crest back), 1000 across.
struct Ramp { float x, z, over_x, over_z, half_width; };
// The stunt loop's ramp, ridden east.
constexpr Ramp kLoopRamp[] = { { kRampCrest, (kRampNear + kRampFar) / 2.0f, 1.0f, 0.0f, (kRampFar - kRampNear) / 2.0f - 100.0f } };
// Stunt Mode's (their matrices, from a frame of the game's), ridden as the
// course goes: the three in a row along the south westward, the one in the
// west northward.
constexpr Ramp kStuntRamps[] = {
    { 2500, -4952, -1.0f, 0.0f, 400 }, { 92, -4952, -1.0f, 0.0f, 400 }, { -2407, -4952, -1.0f, 0.0f, 400 },
    { -6065, 664, 0.121f, 0.993f, 400 },
};
// The stunt course's lap, from the start (in the north-west, heading east):
// its rings (all ten, at y 60) and checkpoints, in order, with the ramps'
// lines run straight -- in along each before its face, out past it.
constexpr float kPractice[][2] = {
    { -2916, 3614 }, { -119, 2761 }, { 1133, 2348 }, { 2095, 2040 }, { 3670, 1478 },        // rings, checkpoint 1
    { 5265, 688 }, { 5750, -183 }, { 5745, -1326 }, { 5163, -2438 }, { 4308, -4250 },       // rings, checkpoint 2
    { 3500, -4952 }, { -3400, -4952 },                                                      // the three ramps
    { -4123, -3724 }, { -5037, -2324 }, { -6066, -881 },                                    // checkpoint 3, rings
    { -6138, 68 }, { -5956, 1558 },                                                         // the west ramp
    { -4771, 3227 },                                                                        // the last ring
};

std::mt19937& chance() {
    static std::mt19937 engine{ std::random_device{}() };
    return engine;
}
float uniform(float lo, float hi) { return lo + (hi - lo) * float(chance()() % 100000u) / 100000.0f; }

// Something a rider keeps clear of: where, how far off it passes, and
// whether it looks ahead for it (or only shies away once it is that near).
struct Obstacle { float x, z, clearance; bool ahead; };

// How much a rider at (x, z), heading `heading` at `speed`, turns aside
// (radians, onto its heading) and slows (a fraction of its speed) to keep
// clear of `obstacles`: one ahead of it, within reach and nearer the line it
// rides than its clearance, pushes it off that line, the more the nearer;
// one nearer than its clearance, whichever way, pushes it away.
struct Avoid { float turn = 0.0f, slow = 1.0f; };
Avoid avoid(float x, float z, float heading, float speed, const std::vector<Obstacle>& obstacles) {
    const float fx = std::cos(heading), fz = std::sin(heading);
    const float reach = 400.0f + 0.8f * speed;
    Avoid a;
    for (const Obstacle& o : obstacles) {
        const float rx = o.x - x, rz = o.z - z;
        const float along = rx * fx + rz * fz, across = rz * fx - rx * fz;
        const float away = across > 0.0f ? -1.0f : 1.0f;
        const float distance = std::hypot(rx, rz);
        if (distance < o.clearance) a.turn += away * 0.8f * (1.0f - distance / o.clearance);
        if (o.ahead && along > 0.0f && along < reach + o.clearance && std::fabs(across) < o.clearance) {
            a.turn += away * 1.2f * (1.0f - along / (reach + o.clearance)) * (1.0f - std::fabs(across) / o.clearance);
            if (along < o.clearance + 150.0f) a.slow = std::min(a.slow, 0.6f);
        }
    }
    a.turn = std::clamp(a.turn, -1.3f, 1.3f);
    return a;
}

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
    static const std::vector<std::pair<float, float>> practice = [] {
        std::vector<std::pair<float, float>> v;
        const int n = int(sizeof(kPractice) / sizeof(kPractice[0]));
        for (int i = 0; i < n; ++i) {
            const float ax = kPractice[i][0], az = kPractice[i][1];
            const float bx = kPractice[(i + 1) % n][0], bz = kPractice[(i + 1) % n][1];
            const float length = std::hypot(bx - ax, bz - az);
            for (float t = 0.0f; t < length; t += kSample) v.emplace_back(ax + (bx - ax) * t / length, az + (bz - az) * t / length);
        }
        return v;
    }();
    if (r == Routine::Practice) return practice;
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
    d.routine = g_stunt ? Routine::Practice : Routine::Stunts;
    d.change_at = now + routine_length();
    if (g_stunt) {
        // A third of the way round the lap, out of the way of the player's start.
        const auto& points = route(Routine::Practice);
        const size_t at = points.size() / 3;
        const auto& p = points[at];
        const auto& q = points[(at + 1) % points.size()];
        d.x = p.first; d.z = p.second; d.heading = std::atan2(q.second - p.second, q.first - p.first); d.speed = kSpeed;
        return;
    }
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

void drive(uint32_t segment3, std::chrono::steady_clock::time_point now, float dt, const std::vector<Obstacle>& obstacles) {
    Drive& d = g_drive;
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    const float water = water_height_at(segment3, d.x, d.z);

    // Time for the other routine: changed where the two meet.
    if (!d.change && now >= d.change_at && d.routine != Routine::Practice) d.change = true;
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

    // On the water: steering for a point ahead on its route, up to speed,
    // swerving round (and slowing for) the player.
    const Avoid av = avoid(d.x, d.z, d.heading, d.speed, obstacles);
    const float top = kSpeed * av.slow;
    d.speed = d.speed < top ? std::min(top, d.speed + kAccel * dt) : std::max(top, d.speed - 600.0f * dt);
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
    const float want = std::atan2(target.second - d.z, target.first - d.x) + av.turn;
    const float diff = std::remainder(want - d.heading, 6.2831853f);
    const float step = std::clamp(diff, -kTurnRate * dt, kTurnRate * dt);
    d.heading = std::remainder(d.heading + step, 6.2831853f);
    d.turning += ((dt > 0.0f ? step / dt : 0.0f) - d.turning) * std::min(1.0f, 6.0f * dt);
    d.x += std::cos(d.heading) * d.speed * dt;
    d.z += std::sin(d.heading) * d.speed * dt;
    // Into the turn: a heading growing is a turn to the right, and a bank towards the hull's x.
    d.bank = g_pose.x_right * kLean * std::clamp(d.turning / (kSpeed / kRadius), -1.0f, 1.0f);
    d.flip = 0.0f;

    // A ramp: up its face, heading over it on a run that rides it, and off
    // the crest into the air with a stunt picked. (`along`, how far past the
    // crest, is measured as the stunt loop's ramp's x.)
    bool on_face = false, past_crest = false;
    float along = 0.0f;
    const Ramp* ramps = d.routine == Routine::Stunts ? kLoopRamp : d.routine == Routine::Practice ? kStuntRamps : nullptr;
    const int ramp_count = d.routine == Routine::Stunts ? 1 : d.routine == Routine::Practice ? 4 : 0;
    for (int i = 0; i < ramp_count; ++i) {
        const Ramp& r = ramps[i];
        const float rx = d.x - r.x, rz = d.z - r.z;
        const float s = rx * r.over_x + rz * r.over_z, across = rz * r.over_x - rx * r.over_z;
        const bool lined_up = std::cos(d.heading) * r.over_x + std::sin(d.heading) * r.over_z > 0.95f &&
                              std::fabs(across) < r.half_width;
        if (!lined_up) continue;
        along = kRampCrest + s;
        if (along >= kRampStart && along < kRampCrest) on_face = true;
        else if (s >= 0.0f && s < 200.0f) past_crest = true;
        if (on_face || past_crest) break;
    }
    if (on_face) {
        d.on_ramp = true;
        d.y = std::max(water, face_height(along));
        d.climb = (face_height(along + 10.0f) - face_height(along - 10.0f)) / 20.0f;
    }
    else if (d.on_ramp && past_crest) {
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


// ---- the pair ------------------------------------------------------------
//
// The other two racers ride together, in one of three routines: the first
// two each kept for 60 to 200 seconds by chance, as the first rider's are,
// and then one of the other two, by chance.
//
// Sightseeing: off side by side to one of the park's sights (kSights) --
// the piers, the island, the stone arches, either ramp -- by chance, the
// last straight run in towards it, then stopped abreast, facing it, for 10
// to 30 seconds; then on to another.
//
// The weave: in the open water between the piers and the ramps, along it
// and parallel to both, east and back west on a long loop (kWeave*), the
// two crossing and recrossing each other's line as the strands of a double
// helix seen from above: each swings from side to side of the loop's line,
// the second the mirror of the first and kWeaveTrail behind it, so they
// cross that far apart and never meet. Near either end they come in onto
// the line, turn about together, and weave on back the other way.
//
// The trick: once, not for a time. Side by side to a place south-east of
// the ramp the first rider leaves alone (kStage), stopped there facing it
// until the first rider is well away from the line they will ride, then
// off together, level, each in its own lane over the ramp (kLane either
// side of its middle), and off the crest into a barrel roll: the one on the
// left rolling to the left, the one on the right to the right. Each has a
// one in twenty chance of getting it wrong and bailing as the first rider
// does. Once both are back on and slowed, one of the other two routines.
//
// Each steers for a point and keeps clear of things (avoid): the ramps, the
// arches' rocks, the island, the piers' pillars and wall, the first rider,
// the player, and -- only once very near -- each other. (Not on the run at
// the ramp: they hold their lanes.)

enum class PairRoutine { Sights, Weave, Trick };
enum class TrickStep { Gather, Wait, Run };
struct Rider {
    float x = 0, y = 0, z = 0, heading = 0, speed = 0, turning = 0, bank = 0, climb = 0;
    bool near = false, stopped = false;     // at a sight: on the run in; stopped there
    // On the trick: its lane (across the ramp from the middle), which way it
    // rolls (+1 towards the hull's x), whether it goes wrong; on the ramp, in
    // the air, down again; and a bail, as the first rider's.
    float lane = 0, roll = 1;
    bool failed = false, on_ramp = false, airborne = false, landed = false;
    float rise = 0, air = 0;
    Crash crash = Crash::None;
    std::chrono::steady_clock::time_point crash_at{};
    float body_x = 0, body_y = 0, body_z = 0, body_hx = 1, body_hz = 0, body_spin = 0, body_rise = 0, body_speed = 0;
};
struct Pair {
    std::chrono::steady_clock::time_point change_at{}, leave_at{}, set_off{}, waiting_since{};
    PairRoutine routine = PairRoutine::Sights;
    TrickStep step = TrickStep::Gather;
    bool change = false, parked = false;
    int sight = 0;
    bool swapped = false;   // each stopping at the other's place, the nearer
    float side = 1.0f;      // on the way, which side of the first the second rides
                            // (+1 the side its heading turns towards)
    float along = 0;        // on the weave: the formation's place round its loop
    Rider riders[2];
} g_pair;

constexpr float kPairSpeed = 900.0f, kPairTurnRate = 2.3f;
// Sightseeing: how far apart they stop, and the straight run in.
constexpr float kAbreast = 260.0f, kRunIn = 500.0f;

struct Sight { const char* name; float x, z, look_x, look_z; };
constexpr Sight kSights[] = {
    { "the piers", 150, 1150, -450, 350 },
    { "the island", 650, 4800, 100, 5190 },
    { "the stone arches", 3250, 2450, 3250, 3600 },
    { "the ramp", 1450, 2350, 1441, 3619 },
    { "the other ramp", -1900, 2900, -1142, 4199 },
    // Not a sight: where they gather for the trick, facing the other ramp.
    { "the start of a jump", 900, 2700, -1142, 4199 },
};
constexpr int kSightCount = int(sizeof(kSights) / sizeof(kSights[0])) - 1;
constexpr int kStage = kSightCount;

// The other ramp -- the same as the first rider's, turned -- for the trick:
// its crest's middle, the way over it (north-west, along its 801 units) and
// across it (its 1000), each lane's middle from its middle, and where the
// lanes cross the first rider's line (z = kOut) on the way.
constexpr float kOtherCrestX = -1142.0f, kOtherCrestZ = 4199.0f;
constexpr float kOverX = -726.0f / 801.0f, kOverZ = 338.0f / 801.0f;
constexpr float kAcrossX = kOverZ, kAcrossZ = -kOverX;
constexpr float kRampHalfLength = kRampCrest - kRampStart, kRampHalfWidth = 480.0f;
constexpr float kLane = 220.0f;
constexpr float kCrossingX = kOtherCrestX + kOverX * (kOut - kOtherCrestZ) / kOverZ, kCrossingZ = kOut;

// The weave's loop: east along z = kWeaveOut from kWeaveWest to kWeaveEast,
// round a half circle to z = kWeaveBack and west again, and round. Each
// rider swings kWeaveWidth either side of it, a full swing every
// kWeaveWave, coming in onto it near the ends.
constexpr float kWeaveWest = -300.0f, kWeaveEast = 5800.0f, kWeaveOut = 2100.0f, kWeaveBack = 1300.0f;
constexpr float kWeaveTurn = (kWeaveOut - kWeaveBack) / 2.0f, kWeaveMiddle = (kWeaveOut + kWeaveBack) / 2.0f;
constexpr float kWeaveWidth = 320.0f, kWeaveWave = 2400.0f, kWeaveTrail = 380.0f, kWeaveSpeed = 800.0f;
constexpr float kWeaveLoop = 2.0f * (kWeaveEast - kWeaveWest) + 6.2831853f * kWeaveTurn;

// What the pair keep clear of, standing in the water, but for the ramps:
// rocks, island, wall and pillars.
std::vector<Obstacle> fixed_obstacles_without_ramps() {
    static const std::vector<Obstacle> obstacles = [] {
        std::vector<Obstacle> v = {
            { 3150, 3220, 370, true }, { 3290, 4000, 440, true }, { 3270, 4850, 460, true },  // the rocks
            { 60, 5220, 650, true },                                                        // the island
            { -110, -553, 560, true },                                                      // the wall
        };
        constexpr float kPillars[][2] = {
            { -1500, 1600 }, { -1500, 1800 }, { -1200, 1600 }, { -1200, 1800 }, { -1200, 2300 }, { -1100, 700 },
            { -1100, 1950 }, { -1000, 1100 }, { -1000, 2300 }, { -700, 350 }, { -700, 1100 }, { -600, 0 },
            { -600, 250 }, { -400, -100 }, { -400, 300 }, { -300, 0 }, { -300, 250 }, { 200, 350 },
            { 400, 100 }, { 500, -200 }, { 500, 100 }, { 700, -200 }, { 900, -200 },
        };
        for (const auto& p : kPillars) v.push_back({ p[0], p[1], 210.0f, true });
        return v;
    }();
    return obstacles;
}

// And with the ramps: in Stunt Mode its own instead of the park's two.
const std::vector<Obstacle>& fixed_obstacles() {
    static const std::vector<Obstacle> stunt = [] {
        std::vector<Obstacle> v = fixed_obstacles_without_ramps();
        for (const Ramp& r : kStuntRamps) v.push_back({ r.x, r.z, 800, true });
        return v;
    }();
    static const std::vector<Obstacle> park = [] {
        std::vector<Obstacle> v = fixed_obstacles_without_ramps();
        v.push_back({ 1441, 3619, 800, true });
        v.push_back({ -1142, 4199, 850, true });
        return v;
    }();
    return g_stunt ? stunt : park;
}

// WR64_COMPANION_TEST_CHANGE, as for the first rider: each routine kept that
// long instead of 60 to 200 seconds.
std::chrono::milliseconds pair_routine_length() {
    static const float test = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_CHANGE");
        return value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr ? float(std::atof(value)) : 0.0f;
    }();
    return std::chrono::milliseconds(int((test > 0.0f ? test : uniform(60.0f, 200.0f)) * 1000.0f));
}

// Rider `i`'s place to stop at sight `sight`, and the way it faces there.
struct Spot { float x, z, fx, fz; };
Spot sight_spot(int sight, int i) {
    const Sight& s = kSights[sight];
    float fx = s.look_x - s.x, fz = s.look_z - s.z;
    const float length = std::hypot(fx, fz);
    fx /= length; fz /= length;
    const float side = (i == 0 ? 0.5f : -0.5f) * kAbreast;
    return { s.x - fz * side, s.z + fx * side, fx, fz };
}

// The weave's loop's line `s` units round: the place, the way on, and how
// far it is to the nearer end of the straight it is on (0 on the turns).
struct LoopPlace { float x, z, tx, tz, straight; };
LoopPlace loop_place(float s) {
    const float straight = kWeaveEast - kWeaveWest, half = 3.14159265f * kWeaveTurn;
    s = std::fmod(s, kWeaveLoop);
    if (s < 0.0f) s += kWeaveLoop;
    if (s < straight) return { kWeaveWest + s, kWeaveOut, 1.0f, 0.0f, std::min(s, straight - s) };
    s -= straight;
    if (s < half) {
        const float a = 1.5707963f - s / kWeaveTurn;     // round the east end
        return { kWeaveEast + kWeaveTurn * std::cos(a), kWeaveMiddle + kWeaveTurn * std::sin(a), std::sin(a), -std::cos(a), 0.0f };
    }
    s -= half;
    if (s < straight) return { kWeaveEast - s, kWeaveBack, -1.0f, 0.0f, std::min(s, straight - s) };
    s -= straight;
    const float a = -1.5707963f - s / kWeaveTurn;        // round the west end
    return { kWeaveWest + kWeaveTurn * std::cos(a), kWeaveMiddle + kWeaveTurn * std::sin(a), std::sin(a), -std::cos(a), 0.0f };
}

// Where rider `i` weaves with the formation `along` units round: the first
// at the place, swung to one side; the second kWeaveTrail behind, swung to
// the other.
std::pair<float, float> weave_point(float along, int i) {
    const float s = along - float(i) * kWeaveTrail;
    const LoopPlace c = loop_place(s);
    const float t = std::clamp(c.straight / 900.0f, 0.0f, 1.0f);
    const float swing = (i == 0 ? 1.0f : -1.0f) * kWeaveWidth * t * t * (3.0f - 2.0f * t) *
                        std::sin(6.2831853f * s / kWeaveWave);
    return { c.x - c.tz * swing, c.z + c.tx * swing };
}

// Moves rider `r` on along its heading, leaning into its turn.
void move(Rider& r, float dt) {
    r.x += std::cos(r.heading) * r.speed * dt;
    r.z += std::sin(r.heading) * r.speed * dt;
    r.bank = g_pose.x_right * kLean * std::clamp(r.turning * r.speed / (kSpeed * kSpeed / kRadius), -1.0f, 1.0f);
}

// Turns rider `r` towards (tx, tz), aside from `obstacles`, bringing it up
// or down towards `top` units a second, and moves it on.
void steer(Rider& r, float tx, float tz, float top, float dt, const std::vector<Obstacle>& obstacles) {
    const Avoid av = avoid(r.x, r.z, r.heading, r.speed, obstacles);
    const float want = std::atan2(tz - r.z, tx - r.x) + av.turn;
    const float diff = std::remainder(want - r.heading, 6.2831853f);
    const float step = std::clamp(diff, -kPairTurnRate * dt, kPairTurnRate * dt);
    r.heading = std::remainder(r.heading + step, 6.2831853f);
    r.turning += ((dt > 0.0f ? step / dt : 0.0f) - r.turning) * std::min(1.0f, 6.0f * dt);
    // Slower round a sharp turn.
    const float wanted = top * av.slow * (std::fabs(diff) > 1.0f ? 0.6f : 1.0f);
    r.speed = r.speed < wanted ? std::min(wanted, r.speed + kAccel * dt) : std::max(wanted, r.speed - 700.0f * dt);
    move(r, dt);
}

// Rider `r` coming to rest, turning slowly to face (look_x, look_z).
void settle(Rider& r, float look_x, float look_z, float dt) {
    r.speed = std::max(0.0f, r.speed - 500.0f * dt);
    const float diff = std::remainder(std::atan2(look_z - r.z, look_x - r.x) - r.heading, 6.2831853f);
    const float step = std::clamp(diff, -0.4f * dt, 0.4f * dt);
    r.heading = std::remainder(r.heading + step, 6.2831853f);
    r.turning += ((dt > 0.0f ? step / dt : 0.0f) - r.turning) * std::min(1.0f, 6.0f * dt);
    move(r, dt);
}

void log_pair(const char* what) {
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    if (!log) return;
    std::fprintf(stderr, "[wr64-companion] the pair: %s\n", what);
    std::fflush(stderr);
}

// Off to sight `sight`: each to the place there on its own side, and the
// second riding beside the first on that side.
void head_for(Pair& p, int sight, std::chrono::steady_clock::time_point now) {
    p.sight = sight;
    p.parked = false;
    p.set_off = now;
    for (Rider& r : p.riders) r.near = r.stopped = false;
    const Rider& a = p.riders[0];
    const Rider& c = p.riders[1];
    auto to = [&](const Rider& r, int i) {
        const Spot s = sight_spot(p.sight, i);
        return std::hypot(s.x - s.fx * kRunIn - r.x, s.z - s.fz * kRunIn - r.z);
    };
    p.swapped = to(a, 1) + to(c, 0) < to(a, 0) + to(c, 1);
    const float wx = kSights[sight].x - a.x, wz = kSights[sight].z - a.z;
    p.side = (c.z - a.z) * wx - (c.x - a.x) * wz >= 0.0f ? 1.0f : -1.0f;
}

// Whether sight `sight` is there to see: in Stunt Mode the park's ramps are not.
bool sight_there(int sight) { return !g_stunt || (sight != 3 && sight != 4); }

// Off to a sight, by chance not the one they are at.
void pick_sight(Pair& p, std::chrono::steady_clock::time_point now) {
    const int from = p.sight < kSightCount ? p.sight : 0;
    int to = from;
    while (to == from || !sight_there(to)) to = int(chance()() % uint32_t(kSightCount));
    head_for(p, to, now);
    char what[96];
    std::snprintf(what, sizeof what, "off to see %s", kSights[p.sight].name);
    log_pair(what);
}

// Onto the weave, the formation's place set some way on round the loop from
// the nearest to them.
void start_weave(Pair& p, std::chrono::steady_clock::time_point now) {
    p.change_at = now + pair_routine_length();
    const float mx = (p.riders[0].x + p.riders[1].x) / 2.0f, mz = (p.riders[0].z + p.riders[1].z) / 2.0f;
    float best = 0.0f, nearest = 1e30f;
    for (float s = 0.0f; s < kWeaveLoop; s += 50.0f) {
        const LoopPlace c = loop_place(s);
        const float d = std::hypot(c.x - mx, c.z - mz);
        if (d < nearest) { nearest = d; best = s; }
    }
    p.along = best + 1500.0f;
    p.routine = PairRoutine::Weave;
    for (Rider& r : p.riders) r.near = r.stopped = false;
    log_pair("off to weave between the piers and the ramps");
}

// Off to gather for the trick.
void start_trick(Pair& p, std::chrono::steady_clock::time_point now) {
    p.routine = PairRoutine::Trick;
    p.step = TrickStep::Gather;
    for (Rider& r : p.riders) {
        r.on_ramp = r.airborne = r.landed = r.failed = false;
        r.crash = Crash::None;
    }
    head_for(p, kStage, now);
    log_pair("off to jump the other ramp together");
}

// One of the two routines they are not on, by chance.
void next_routine(Pair& p, std::chrono::steady_clock::time_point now) {
    p.change = false;
    PairRoutine next[2];
    int n = 0;
    for (PairRoutine r : { PairRoutine::Sights, PairRoutine::Weave, PairRoutine::Trick }) {
        // (In Stunt Mode, no trick: its ramp is not there.)
        if (r != p.routine && !(g_stunt && r == PairRoutine::Trick)) next[n++] = r;
    }
    switch (next[chance()() % uint32_t(n)]) {
    case PairRoutine::Sights:
        p.routine = PairRoutine::Sights;
        p.change_at = now + pair_routine_length();
        pick_sight(p, now);
        break;
    case PairRoutine::Weave: start_weave(p, now); break;
    case PairRoutine::Trick: start_trick(p, now); break;
    }
}

void start_pair(std::chrono::steady_clock::time_point now) {
    Pair& p = g_pair;
    p = Pair{};
    p.change_at = now + pair_routine_length();
    // WR64_COMPANION_TEST_PAIR=weave, in a scripted run only: weaving from
    // the start, instead of stopped at a sight.
    static const bool weave = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_PAIR");
        return value != nullptr && value[0] == 'w' && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
    }();
    if (weave) {
        p.routine = PairRoutine::Weave;
        p.along = 1000.0f;
        for (int i = 0; i < 2; ++i) {
            Rider& r = p.riders[i];
            const auto [x, z] = weave_point(p.along, i);
            const auto [ax, az] = weave_point(p.along + 50.0f, i);
            r.x = x; r.z = z; r.heading = std::atan2(az - z, ax - x); r.speed = kWeaveSpeed;
        }
        return;
    }
    // Already stopped at a sight, as if they had been out a while.
    // (WR64_COMPANION_TEST_PAIR=<n>, scripted: at sight n, for a minute.)
    static const int test_sight = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_PAIR");
        return value != nullptr && value[0] >= '0' && value[0] < '0' + kSightCount && std::getenv("WR64_INPUT_SCRIPT") != nullptr
                   ? value[0] - '0' : -1;
    }();
    // (WR64_COMPANION_TEST_PAIR=trick, scripted: from the ramp's, off to
    // the trick at once.)
    static const bool trick = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_PAIR");
        return value != nullptr && value[0] == 't' && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
    }();
    p.sight = trick ? 3 : test_sight >= 0 ? test_sight : int(chance()() % uint32_t(kSightCount));
    while (!sight_there(p.sight)) p.sight = int(chance()() % uint32_t(kSightCount));
    p.parked = true;
    p.leave_at = now + std::chrono::milliseconds(test_sight >= 0 ? 60000 : int(uniform(5.0f, 20.0f) * 1000.0f));
    for (int i = 0; i < 2; ++i) {
        const Spot s = sight_spot(p.sight, i);
        Rider& r = p.riders[i];
        r.x = s.x; r.z = s.z; r.heading = std::atan2(s.fz, s.fx);
        r.near = r.stopped = true;
    }
    if (trick) start_trick(p, now);
    // WR64_COMPANION_TEST_PAIR_AT=<x>,<z>, scripted: stopped there instead,
    // one either side of it (z), for a minute.
    static const char* at = std::getenv("WR64_INPUT_SCRIPT") != nullptr ? std::getenv("WR64_COMPANION_TEST_PAIR_AT") : nullptr;
    float x = 0.0f, z = 0.0f;
    if (at != nullptr && std::sscanf(at, "%f,%f", &x, &z) == 2) {
        p.leave_at = now + std::chrono::seconds(60);
        for (int i = 0; i < 2; ++i) {
            p.riders[i].x = x;
            p.riders[i].z = z + (i == 0 ? -0.5f : 0.5f) * kAbreast;
        }
    }
}

// Side by side to their places at sight p.sight (see head_for); whether
// both are stopped there.
bool travel(Pair& p, float dt, const std::vector<Obstacle> obstacles[2]) {
    const Sight& sight = kSights[p.sight];

    // Side by side on the way: the second keeps kAbreast to the first's
    // side, level with it, until the first is nearly there; the first eases
    // off while the second is behind its place.
    const Rider& first = p.riders[0];
    const float fx = std::cos(first.heading), fz = std::sin(first.heading);
    const float beside_x = first.x - fz * p.side * kAbreast, beside_z = first.z + fx * p.side * kAbreast;
    const float behind = (beside_x - p.riders[1].x) * fx + (beside_z - p.riders[1].z) * fz;
    const Spot first_spot = sight_spot(p.sight, p.swapped ? 1 : 0);
    const bool together = !first.near && !p.riders[1].near &&
        std::hypot(first_spot.x - first_spot.fx * kRunIn - first.x, first_spot.z - first_spot.fz * kRunIn - first.z) > 900.0f;
    for (int i = 0; i < 2; ++i) {
        Rider& r = p.riders[i];
        const Spot s = sight_spot(p.sight, i ^ (p.swapped ? 1 : 0));
        if (r.stopped) {
            settle(r, sight.look_x, sight.look_z, dt);
            continue;
        }
        const float in_x = s.x - s.fx * kRunIn, in_z = s.z - s.fz * kRunIn;
        const float to_in = std::hypot(in_x - r.x, in_z - r.z);
        if (!r.near && to_in < 250.0f) r.near = true;
        if (!r.near) {
            if (i == 1 && together) {
                steer(r, beside_x + fx * 300.0f, beside_z + fz * 300.0f,
                      std::clamp(first.speed + 1.5f * behind, 300.0f, 1150.0f), dt, obstacles[i]);
                continue;
            }
            // Easing off nearing the run in, to turn onto it.
            float top = i == 0 && together ? std::clamp(kPairSpeed - 1.2f * behind, 500.0f, kPairSpeed) : kPairSpeed;
            top = std::min(top, std::max(450.0f, to_in * 0.9f));
            steer(r, in_x, in_z, top, dt, obstacles[i]);
            continue;
        }
        // The run in: slowing to a stop at its spot, aimed past it so it
        // comes in straight; come in wide of it, and round to try again.
        const float left = (s.x - r.x) * s.fx + (s.z - r.z) * s.fz;
        if (left < 30.0f) {
            if (std::fabs((r.x - s.x) * s.fz - (r.z - s.z) * s.fx) < 100.0f) r.stopped = true;
            else r.near = false;
            continue;
        }
        steer(r, s.x + s.fx * 300.0f, s.z + s.fz * 300.0f, std::clamp(left * 1.4f, 120.0f, 750.0f), dt, obstacles[i]);
    }
    return p.riders[0].stopped && p.riders[1].stopped;
}

void ride_sights(Pair& p, std::chrono::steady_clock::time_point now, float dt, const std::vector<Obstacle> obstacles[2]) {
    const Sight& sight = kSights[p.sight];
    if (p.parked) {
        for (Rider& r : p.riders) settle(r, sight.look_x, sight.look_z, dt);
        if (now < p.leave_at) return;
        if (p.change) {
            next_routine(p, now);
            return;
        }
        pick_sight(p, now);
        return;
    }
    // On the way (or a change due): another routine instead.
    if (p.change) {
        next_routine(p, now);
        return;
    }
    // Too long getting there -- caught going round something -- and they
    // try somewhere else.
    if (now - p.set_off > std::chrono::seconds(60)) {
        log_pair("gave up getting there");
        pick_sight(p, now);
        return;
    }
    if (travel(p, dt, obstacles)) {
        p.parked = true;
        p.leave_at = now + std::chrono::milliseconds(int(uniform(10.0f, 30.0f) * 1000.0f));
        char what[96];
        std::snprintf(what, sizeof what, "stopped to look at %s", sight.name);
        log_pair(what);
    }
}

void ride_weave(Pair& p, std::chrono::steady_clock::time_point now, float dt, const std::vector<Obstacle> obstacles[2]) {
    if (p.change) {
        next_routine(p, now);
        return;
    }
    // The formation moves on round the loop, slower while they are still
    // far from their places in it.
    float far = 0.0f;
    for (int i = 0; i < 2; ++i) {
        const auto [x, z] = weave_point(p.along, i);
        far = std::max(far, std::hypot(x - p.riders[i].x, z - p.riders[i].z));
    }
    p.along = std::fmod(p.along + kWeaveSpeed * dt * std::clamp(1.0f - (far - 300.0f) / 3000.0f, 0.3f, 1.0f), kWeaveLoop);
    // Each steers for a point a little on from its place, faster when its
    // place is ahead of it, slower when behind. Still far from it, coming
    // in from somewhere else, each looks out for the other too.
    for (int i = 0; i < 2; ++i) {
        Rider& r = p.riders[i];
        const auto [x, z] = weave_point(p.along, i);
        const auto [ax, az] = weave_point(p.along + 250.0f, i);
        const float ahead = (x - r.x) * std::cos(r.heading) + (z - r.z) * std::sin(r.heading);
        const float top = std::clamp(kWeaveSpeed + 1.5f * ahead, 150.0f, 1150.0f);
        if (far > 300.0f) {
            std::vector<Obstacle> joining = obstacles[i];
            const Rider& other = p.riders[1 - i];
            joining.push_back({ other.x, other.z, 250.0f, true });
            steer(r, ax, az, top, dt, joining);
        }
        else {
            steer(r, ax, az, top, dt, obstacles[i]);
        }
    }
}

// Rider `r` on the trick, down again: a bail played out as the first
// rider's (thrown off ahead, under, back on five seconds on), or riding on
// straight and slowing. Whether it is back on and nearly stopped.
bool recover(Rider& r, uint32_t segment3, std::chrono::steady_clock::time_point now, float dt) {
    const float water = water_height_at(segment3, r.x, r.z);
    r.y = water;
    r.climb = 0.0f;
    if (r.crash == Crash::None) {
        r.speed = std::max(0.0f, r.speed - 600.0f * dt);
        r.turning = 0.0f;
        move(r, dt);
        return r.speed < 60.0f;
    }
    r.speed = std::max(0.0f, r.speed - 700.0f * dt);
    r.bank *= std::max(0.0f, 1.0f - 6.0f * dt);
    r.x += std::cos(r.heading) * r.speed * dt;
    r.z += std::sin(r.heading) * r.speed * dt;
    if (r.crash == Crash::Thrown) {
        r.body_x += r.body_hx * r.body_speed * dt;
        r.body_z += r.body_hz * r.body_speed * dt;
        r.body_rise -= kGravity * dt;
        r.body_y += r.body_rise * dt;
        r.body_spin += 9.0f * dt;
        if (r.body_y <= water_height_at(segment3, r.body_x, r.body_z) && r.body_rise < 0.0f) {
            r.crash = Crash::Waiting;
            r.body_speed = 0.0f;
        }
    }
    else {
        r.body_y -= 60.0f * dt;                     // sinking out of sight
    }
    if (std::chrono::duration<float>(now - r.crash_at).count() > 5.0f) {
        r.crash = Crash::None;
        r.bank = 0.0f;
    }
    return false;
}

// WR64_COMPANION_TEST_PAIR_FAIL=<l, r or lr>, in a scripted run only: the
// left, the right or both riders get the trick wrong.
bool test_fail(bool left) {
    static const char* value = std::getenv("WR64_INPUT_SCRIPT") != nullptr ? std::getenv("WR64_COMPANION_TEST_PAIR_FAIL") : nullptr;
    return value != nullptr && std::strchr(value, left ? 'l' : 'r') != nullptr;
}

void ride_trick(Pair& p, uint32_t segment3, std::chrono::steady_clock::time_point now, float dt,
                const std::vector<Obstacle> obstacles[2], const Obstacle* first) {
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    const Sight& stage = kSights[kStage];
    if (p.step == TrickStep::Gather) {
        if (now - p.set_off > std::chrono::seconds(60)) {
            log_pair("gave up getting to the ramp");
            next_routine(p, now);
            return;
        }
        if (travel(p, dt, obstacles)) {
            p.step = TrickStep::Wait;
            p.waiting_since = now;
            log_pair("waiting their turn at the ramp");
        }
        for (Rider& r : p.riders) r.y = water_height_at(segment3, r.x, r.z);
        return;
    }
    if (p.step == TrickStep::Wait) {
        for (Rider& r : p.riders) {
            settle(r, stage.look_x, stage.look_z, dt);
            r.y = water_height_at(segment3, r.x, r.z);
        }
        // Off once the first rider is well away from where they will cross
        // its line, two seconds at least.
        const bool clear = first == nullptr || std::hypot(first->x - kCrossingX, first->z - kCrossingZ) > 3500.0f;
        if (now - p.waiting_since < std::chrono::seconds(2) || !clear) return;
        p.step = TrickStep::Run;
        // Lanes by where they are: the one further across, the far lane. The
        // rider's right is (-z, x) of the way it goes.
        const float right_x = -kOverZ, right_z = kOverX;
        const float across0 = (p.riders[0].x - kOtherCrestX) * kAcrossX + (p.riders[0].z - kOtherCrestZ) * kAcrossZ;
        const float across1 = (p.riders[1].x - kOtherCrestX) * kAcrossX + (p.riders[1].z - kOtherCrestZ) * kAcrossZ;
        for (int i = 0; i < 2; ++i) {
            Rider& r = p.riders[i];
            r.lane = ((i == 0) == (across0 > across1)) ? kLane : -kLane;
            const bool right = r.lane * (kAcrossX * right_x + kAcrossZ * right_z) > 0.0f;
            r.roll = right ? g_pose.x_right : -g_pose.x_right;
            r.failed = chance()() % 100u < 5u || test_fail(!right);
        }
        if (log) {
            auto side = [&](const Rider& r) { return r.lane * (kAcrossX * right_x + kAcrossZ * right_z) > 0.0f ? "right" : "left"; };
            std::fprintf(stderr, "[wr64-companion] the pair: off up the ramp, the first on the %s (rolling %+.0f)%s, "
                         "the second on the %s (rolling %+.0f)%s; the hull's x to the rider's %s\n",
                         side(p.riders[0]), p.riders[0].roll, p.riders[0].failed ? " going wrong" : "",
                         side(p.riders[1]), p.riders[1].roll, p.riders[1].failed ? " going wrong" : "",
                         g_pose.x_right > 0.0f ? "right" : "left");
            std::fflush(stderr);
        }
        return;
    }

    // The run: level with each other, each in its lane, over the crest.
    float along[2];
    for (int i = 0; i < 2; ++i) {
        along[i] = (p.riders[i].x - kOtherCrestX) * kOverX + (p.riders[i].z - kOtherCrestZ) * kOverZ;
    }
    bool done = true;
    for (int i = 0; i < 2; ++i) {
        Rider& r = p.riders[i];
        const float water = water_height_at(segment3, r.x, r.z);
        if (r.landed) {
            if (!recover(r, segment3, now, dt)) done = false;
            continue;
        }
        done = false;
        if (r.airborne) {
            // Flying, rolling, then down onto the water.
            r.air += dt;
            r.x += std::cos(r.heading) * r.speed * dt;
            r.z += std::sin(r.heading) * r.speed * dt;
            r.rise -= kGravity * dt;
            r.y += r.rise * dt;
            r.climb = r.rise / std::max(r.speed, 1.0f);
            const float k = std::clamp((r.air - kRollStart) / kRollLength, 0.0f, 1.0f);
            r.bank = r.roll * 6.2831853f * (r.failed ? 0.72f : 1.0f) * k * k * (3.0f - 2.0f * k);
            if (r.y <= water && r.rise < 0.0f) {
                r.airborne = false;
                r.landed = true;
                r.y = water;
                r.climb = 0.0f;
                if (r.failed) {
                    r.crash = Crash::Thrown;
                    r.crash_at = now;
                    r.body_x = r.x; r.body_y = water + 40.0f; r.body_z = r.z;
                    r.body_hx = std::cos(r.heading); r.body_hz = std::sin(r.heading);
                    r.body_speed = r.speed * 0.6f;
                    r.body_rise = 220.0f;
                    r.body_spin = 0.0f;
                }
                else {
                    r.bank = 0.0f;
                }
                if (log) {
                    std::fprintf(stderr, "[wr64-companion] the pair: the %s rider %s\n", i == 0 ? "first" : "second",
                                 r.failed ? "bailed the landing" : "landed it");
                    std::fflush(stderr);
                }
            }
            continue;
        }
        // On the water or the ramp's face: steering along its lane, a point
        // ahead, its speed matched to the other's place along the way.
        const float ahead = along[i] + 500.0f;
        steer(r, kOtherCrestX + kOverX * ahead + kAcrossX * r.lane, kOtherCrestZ + kOverZ * ahead + kAcrossZ * r.lane,
              std::clamp(kSpeed + 1.5f * (along[1 - i] - along[i]), 700.0f, 1150.0f), dt, {});
        const float s = (r.x - kOtherCrestX) * kOverX + (r.z - kOtherCrestZ) * kOverZ;
        const float across = (r.x - kOtherCrestX) * kAcrossX + (r.z - kOtherCrestZ) * kAcrossZ;
        if (s >= -kRampHalfLength && s < 0.0f && std::fabs(across) < kRampHalfWidth) {
            r.on_ramp = true;
            r.y = std::max(water, face_height(kRampCrest + s));
            r.climb = (face_height(kRampCrest + s + 10.0f) - face_height(kRampCrest + s - 10.0f)) / 20.0f;
        }
        else if (r.on_ramp && s >= 0.0f) {
            r.on_ramp = false;
            r.airborne = true;
            r.air = 0.0f;
            r.y = kRampTop;
            r.rise = kPop;
        }
        else if (s >= 0.0f) {
            // Past the crest without having climbed it: no trick this time.
            r.landed = true;
            r.y = water;
        }
        else {
            r.on_ramp = false;
            r.y = water;
            r.climb = 0.0f;
        }
    }
    if (done) {
        log_pair("both back on and riding");
        next_routine(p, now);
    }
}

// The pair's riding this frame: `player` and `first` where the player and
// the first rider are, if they are about.
void ride_pair(uint32_t segment3, std::chrono::steady_clock::time_point now, float dt,
               const Obstacle* player, const Obstacle* first) {
    Pair& p = g_pair;
    if (p.routine != PairRoutine::Trick && now >= p.change_at) p.change = true;
    std::vector<Obstacle> obstacles[2];
    for (int i = 0; i < 2; ++i) {
        obstacles[i] = fixed_obstacles();
        if (player != nullptr) obstacles[i].push_back(*player);
        if (first != nullptr) obstacles[i].push_back(*first);
        const Rider& other = p.riders[1 - i];
        obstacles[i].push_back({ other.x, other.z, 180.0f, false });
    }
    if (p.routine == PairRoutine::Trick) {
        ride_trick(p, segment3, now, dt, obstacles, first);
        return;
    }
    if (p.routine == PairRoutine::Sights) ride_sights(p, now, dt, obstacles);
    else ride_weave(p, now, dt, obstacles);
    for (Rider& r : p.riders) {
        r.y = water_height_at(segment3, r.x, r.z);
        r.climb = 0.0f;
    }
}

}  // namespace

// From the settings' Free Roam tab.
void set_companions(bool on) { g_wanted.store(on, std::memory_order_relaxed); }

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
    // Facing along +x the rider's right is +z: to the right of (fx, fz) is (-fz, fx).
    g_pose.x_right = m[0][0] * -m[2][2] + m[0][2] * m[2][0] >= 0.0f ? 1.0f : -1.0f;
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
    // without -- brings the three racers the player is not, in an order by
    // chance (the first alone, the other two the pair), and starts their
    // riding again.
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last{};
    const Clock::time_point now = Clock::now();
    const bool fresh = g_racers[0].character < 0 || now - last > std::chrono::seconds(2);
    if (fresh) {
        const int player = player_character(rdram);
        std::vector<int> others;
        for (int c = 0; c < kCharacters; ++c) if (c != player) others.push_back(c);
        std::shuffle(others.begin(), others.end(), chance());
        for (int i = 0; i < 3; ++i) {
            g_racers[i].character = others[i];
            g_racers[i].ready = load_racer(rdram, others[i], i);
        }
        std::fprintf(stderr, "[wr64-companion] character %d rides with the player (character %d)%s; %d and %d ride together%s\n",
                     others[0], player, g_racers[0].ready ? "" : " (its models did not load)", others[1], others[2],
                     g_racers[1].ready && g_racers[2].ready ? "" : " (their models did not load)");
        std::fflush(stderr);
    }
    last = now;
    const bool first_ready = g_racers[0].ready, pair_ready = g_racers[1].ready && g_racers[2].ready;
    if (!first_ready && !pair_ready) return;

    // Their riding this frame, each keeping clear of the player (as last
    // seen riding), the pair of the first too.
    static Clock::time_point last_ride{};
    const float dt = std::clamp(std::chrono::duration<float>(now - last_ride).count(), 0.0f, 0.1f);
    last_ride = now;
    const Obstacle player{ g_pose.hull.m[3][0], g_pose.hull.m[3][2], 450.0f, true };
    if (fresh) {
        g_stunt = word_at(rdram, kGameModes) == kStuntMode;
        start_drive(now);
        start_pair(now);
    }
    if (first_ready) drive(segments[3], now, dt, { player });
    const Drive& d = g_drive;
    const Obstacle first{ d.x, d.z, 450.0f, true };
    if (pair_ready) ride_pair(segments[3], now, dt, &player, first_ready ? &first : nullptr);

    // WR64_COMPANION_TEST_OFFSET=<dx>,<dz>, in a scripted run only: drawn
    // that far from where they ride, to bring them before a test's camera.
    static const std::pair<float, float> offset = [] {
        std::pair<float, float> o{ 0.0f, 0.0f };
        const char* value = std::getenv("WR64_COMPANION_TEST_OFFSET");
        if (value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr) std::sscanf(value, "%f,%f", &o.first, &o.second);
        return o;
    }();
    // Draws racer `slot` with its jet ski on `hull` and its rider on `body`
    // (or not, once under), as the game draws the player's (its list at
    // segment 2): the parts' matrices are the jet ski's three and the head,
    // chest and hips, then each limb's twice, as it is and mirrored (7, 9
    // ... 17); the others first with back faces culled, then the mirrored with
    // front faces culled -- the mirror turns them inside out -- leaving the
    // state as the game's list leaves it.
    auto draw = [&](int slot, Matrix hull, Matrix body, bool body_shown) {
        for (Matrix* m : { &hull, &body }) { m->m[3][0] += offset.first; m->m[3][2] += offset.second; }
        const uint32_t base = kSlots[slot].matrices + uint32_t(b) * kMatrixStride;
        const uint32_t* lists = g_racers[slot].lists;
        constexpr uint32_t kCullFront = 0x1000, kCullBack = 0x2000;
        for (const bool mirrored : { false, true }) {
            out.emplace_back(0xB6000000u, mirrored ? kCullBack : kCullFront);
            out.emplace_back(0xB7000000u, mirrored ? kCullFront : kCullBack);
            for (int p = 0; p < kParts; ++p) {
                if ((p >= 7 && (p - 6) % 2 == 1) != mirrored) continue;
                // The jet ski's parts (0 to 2) on its hull; the rider's on the body.
                if (p >= 3 && !body_shown) continue;
                const uint32_t address = base + uint32_t(p) * 64;
                write_matrix(rdram, address, multiply(g_pose.parts[p], p < 3 ? hull : body));
                out.emplace_back(0x01020040u, 0x80000000u | address);
                out.emplace_back(0x06000000u, lists[p < 6 ? p : 6 + (p - 6) / 2]);
            }
        }
    };

    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    static int frames = 0;
    const bool report = log && frames++ % 40 == 0;

    if (first_ready) {
        const float hx = std::cos(d.heading), hz = std::sin(d.heading);
        Matrix hull = frame_matrix(d.x, d.y, d.z, hx, hz, std::atan(d.climb), d.bank, d.flip);
        // After a bail the rider apart from it, and gone once under.
        Matrix body = d.crash == Crash::None ? hull : frame_matrix(d.body_x, d.body_y, d.body_z, d.body_hx, d.body_hz, 0.0f, 0.0f, d.body_spin);
        const bool body_shown = d.crash == Crash::None || d.body_y > water_height_at(segments[3], d.body_x, d.body_z) - 80.0f;
        // The spray round its hull, on the water and moving (src/body_splash.cpp).
        body_splash_companion(0, !d.airborne && d.crash != Crash::Waiting, d.x, d.z,
                              water_height_at(segments[3], d.x, d.z), hx, hz, d.speed);
        // WR64_COMPANION_TEST_BESIDE=1, in a scripted run only: drawn beside the
        // player's jet ski instead, 400 units ahead and 200 to its left.
        static const bool beside = std::getenv("WR64_COMPANION_TEST_BESIDE") != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
        if (beside) {
            hull = g_pose.hull;
            const float side = 200.0f / g_pose.scale, ahead = 400.0f / g_pose.scale;
            for (int c2 = 0; c2 < 3; ++c2) hull.m[3][c2] += hull.m[0][c2] * side + hull.m[2][c2] * ahead;
            body = hull;
        }
        if (report) {
            std::fprintf(stderr, "[wr64-companion] at %.0f %.0f %.0f, %s, speed %.0f, heading %.2f\n", d.x, d.y, d.z,
                         d.routine == Routine::Stunts ? "stunts" : d.routine == Routine::Lap ? "lap" : "practising",
                         d.speed, d.heading);
            std::fflush(stderr);
        }
        draw(0, hull, body, body_shown);
    }

    if (pair_ready) {
        for (int i = 0; i < 2; ++i) {
            const Rider& r = g_pair.riders[i];
            const float hx = std::cos(r.heading), hz = std::sin(r.heading);
            const Matrix hull = frame_matrix(r.x, r.y, r.z, hx, hz, std::atan(r.climb), r.bank, 0.0f);
            const Matrix body = r.crash == Crash::None ? hull : frame_matrix(r.body_x, r.body_y, r.body_z, r.body_hx, r.body_hz, 0.0f, 0.0f, r.body_spin);
            const bool body_shown = r.crash == Crash::None || r.body_y > water_height_at(segments[3], r.body_x, r.body_z) - 80.0f;
            body_splash_companion(1 + i, !r.airborne && r.crash != Crash::Waiting, r.x, r.z,
                                  water_height_at(segments[3], r.x, r.z), hx, hz, r.speed);
            draw(1 + i, hull, body, body_shown);
        }
        if (log) {
            // How near they come: to each other, to the first rider, to the
            // player; the nearest yet to each other, when it changes.
            const Rider& a = g_pair.riders[0];
            const Rider& c = g_pair.riders[1];
            const float apart = std::hypot(a.x - c.x, a.z - c.z);
            auto nearer = [&](float x, float z) { return std::min(std::hypot(a.x - x, a.z - z), std::hypot(c.x - x, c.z - z)); };
            static float closest = 1e9f;
            if (fresh) closest = 1e9f;
            const bool closer = apart < closest - 10.0f && apart < 300.0f;
            closest = std::min(closest, apart);
            if (report || closer) {
                std::fprintf(stderr, "[wr64-companion] the pair at %.0f %.0f and %.0f %.0f, %s, speeds %.0f %.0f; apart %.0f%s, "
                             "from the first %.0f, from the player %.0f (at %.0f %.0f)\n", a.x, a.z, c.x, c.z,
                             g_pair.routine == PairRoutine::Weave ? "weaving" : g_pair.routine == PairRoutine::Trick ? "on the trick"
                             : g_pair.parked ? "stopped" : "sightseeing",
                             a.speed, c.speed, apart, closer ? " (nearest yet)" : "",
                             first_ready ? nearer(d.x, d.z) : -1.0f, nearer(player.x, player.z), player.x, player.z);
                std::fflush(stderr);
            }
        }
    }
}

}  // namespace wr64

// Fellow riders out on the water, having fun while you warm up: the three
// racers the player did not pick. On Dolphin Park one rides a loop round the
// park, hitting the ramp at full speed and barrel-rolling over it, or laps
// the park's edge; the other two ride together (the pair, below), touring
// the park's sights and weaving round each other in the open water. All
// three swerve away from the player, and the pair from the first. On the
// other courses, in a free roam, the first laps the buoys' line and the
// pair weave along it, both resting at quiet spots beside it now and then
// (until the courses have sights of their own); there the first keeps
// clear of the pair too.
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
// kPairArea (kSlots), with their spray; each chunk unpacked straight from the
// cartridge.
//
// What each does, and when, is up to a mind of its own (src/rider_minds.cpp):
// the first rider and the pair each have a personality -- a daredevil, a
// sightseer, a social rider, a loner, a show-off, a practiser -- and wants
// that rise and fall, and when one tires of what it is doing it picks again:
// its routes and routines as above, or riding along with the player for a
// while (Follow), drifting somewhere quiet (Rest), or, having seen the
// player's big air, going over to watch (Watch).
//
// WR64_COMPANIONS=0 leaves the park to the player; WR64_COMPANION_LOG=1
// reports where it is now and then.

#include "recomp.h"
#include "wr64/journal.h"
#include "wr64/rider_minds.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

extern "C" int32_t wr64_ramp_pieces(void);

namespace wr64 {

// patches/free_roam.cpp: whether this race is a free roam.
bool free_roam_active(const uint8_t* rdram);
// src/seafloor.cpp: the seafloor's height, on a course with one.
bool seafloor_height_at(uint32_t course, float x, float z, float* height);
// patches/water_all_round.cpp: the waves' height at a point.
float water_height_at(uint32_t segment3, float x, float z);
// src/body_splash.cpp: companion `index`'s jet ski this frame, for its spray.
void body_splash_companion(int index, bool on_water, float x, float z, float water, float ahead_x, float ahead_z,
                           float speed);

}  // namespace wr64

namespace recomp {
std::span<const uint8_t> get_rom();
}

namespace wr64 {

namespace {

constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };
constexpr int kParts = 18, kUniqueParts = 12;
// The first's matrices clear of the seafloor's lists (0x512000 to 0x53E000).
constexpr uint32_t kMatrices = 0x55E000, kMatrixStride = 0x800;
// Where the unpacked racer goes, above the game's 4 MB with the rest of the
// port's own data, and kOwned marking that area as ours. (A smaller chunk
// unpacks to at most 0x9F08 bytes; the dolphin's follows it, below.)
constexpr uint32_t kOwned = 0x53F000, kModel = 0x540000, kModelEnd = 0x54A000;
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
// More riders (the two other singles and the trio, more_riders_on): each
// drawn from one of the three racers' models already unpacked (there are
// only four characters), with matrices of its own in the pair's area, after
// the pair's and before their spray -- room for six.
constexpr uint32_t kExtraMatrices = 0x59A000, kExtraStride = 0x1000;
constexpr int kMaxExtras = 6;
// USA Rev A, physical: the characters' chunks (full, smaller, 4, 0 for each),
// their part templates (17 words, 0x44 bytes, three for each), and the
// player's rider's parts' lists as the game set them.
constexpr uint32_t kChunks = 0x0DABD0, kTemplates = 0x0DACD0, kTemplateSize = 0x44;
constexpr uint32_t kPlayerParts = 0x227A60;
constexpr int kCharacters = 4;
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

// Unpacks the MIO0 chunk at `rom` in the cartridge to physical `to`, at
// most `room` bytes; returns its length, or 0 if it is not one or does not
// fit. Read from the cartridge's own bytes, not through RDRAM: there's no
// spare room in the 8 MB for a copy (one at 0x530000 overwrote the
// seafloor's lists, one at 0x5F0000 the water's fog in first person).
uint32_t unpack_mio0(uint8_t* rdram, uint32_t rom, uint32_t to, uint32_t room) {
    const std::span<const uint8_t> cart = recomp::get_rom();
    auto byte = [&](uint32_t at) -> uint32_t { return at < cart.size() ? cart[at] : 0u; };
    auto word = [&](uint32_t at) { return (byte(at) << 24) | (byte(at + 1) << 16) | (byte(at + 2) << 8) | byte(at + 3); };
    if (rom + 16 > cart.size() || word(rom) != 0x4D494F30u) return 0;    // "MIO0"
    const uint32_t size = word(rom + 4);
    uint32_t pairs = rom + word(rom + 8), raw = rom + word(rom + 12);
    if (size == 0 || size > room) return 0;
    uint32_t bits = rom + 16, flags = 0, mask = 0, out = 0;
    while (out < size) {
        if (mask == 0) {
            flags = word(bits);
            bits += 4;
            mask = 0x80000000u;
        }
        if (flags & mask) {
            rdram[(to + out) ^ 3] = uint8_t(byte(raw++));
            ++out;
        }
        else {
            const uint32_t pair = (byte(pairs) << 8) | byte(pairs + 1);
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
    const uint32_t size = unpack_mio0(rdram, rom, model, kSlots[slot].model_end - model);
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
// Off its routes, as its mind chooses: riding along with the player, gone
// somewhere quiet to rest, gone to watch something, or taken with the
// dolphin (watching it, or following where it leads).
enum class Free { None, Follow, Rest, Watch, Dolphin };

// The dolphin as the riders see it (set each frame by the dolphin's own
// section, below): whether it is out, where, which way and how fast it
// swims, and whether it is on the move (leading) or performing in place.
struct DolphinSeen {
    bool out = false, moving = false;
    float x = 0, z = 0, heading = 0, speed = 0;
} g_dolphin_seen;
// A rider's way round land and ramps, away from the park (rider_waypoint,
// with the dolphin's route-finding below): the route it is following, where
// to, and when it was found.
struct Way {
    std::vector<std::pair<float, float>> route;
    float to_x = 1e9f, to_z = 1e9f;
    double planned = -1e9;
};
// The point to steer for on the way from (x, z) to (tx, tz): the target
// itself where the water between is open, else a point on a route round.
std::pair<float, float> rider_waypoint(Way& way, float x, float z, float tx, float tz);
// A rider's step on along `heading` at `speed`: away from the park, never
// out of the water onto land or into a ramp (g_wet_map) -- it slows there
// instead, and turns away as it steers on. (Its way round, above, keeps it
// off most of them; this is the last word, as the dolphin's is.)
void step_on(float& x, float& z, float heading, float& speed, float dt);
// A place `distance` from (x, z) towards (from_x, from_z), or failing that
// the nearest way round (x, z) to it, in the riders' water (away from the
// park): somewhere to stop and watch from that isn't on the sand.
std::pair<float, float> open_spot_by(float x, float z, float from_x, float from_z, float distance);

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
    Free free = Free::None;
    float spot_x = 0, spot_z = 0, look_x = 0, look_z = 0, side = 1;
    bool parked = false;
    Way way;
    // Its mind (g_minds), whether it leads the trio (no ramps: its two keep
    // in a wedge behind it), and its speed on its routes.
    int mind = 0;
    bool trio = false;
    float top = kSpeed;
};
// The single riders -- the first, and with more riders out two others --
// and the trio's leader.
constexpr int kDrives = 4, kTrioLeader = 3;
Drive g_drives[kDrives];
Drive& g_drive = g_drives[0];

// The player, read from its rider each frame: where, how fast and which way
// it goes, how high above the water, and whether its big air has been
// noticed yet.
struct Player {
    bool known = false;
    float x = 0, y = 0, z = 0, speed = 0, heading = 0, height = 0;
    bool aloft = false;
} g_player;

// The minds: the first rider's, the pair's (they go everywhere together),
// the two other singles', and the trio's (the same).
constexpr int kMinds = 5;
minds::Mind g_minds[kMinds];
// Which mind is whose, for the log and the journal.
const char* const kMindNames[kMinds] = { "the first rider", "the pair", "the second rider", "the third rider", "the trio" };
const char* const kMindKeys[kMinds] = { "rider", "pair", "rider2", "rider3", "trio" };
double seconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

// Where everyone is, for a line of the decisions journal (defined with the
// dolphin, below): `which` the rider deciding (0 the first, 1 the pair).
void journal_context(journal::Fields& f, int which);
// Whether this outing has the pair out; the two other singles and the trio.
bool pair_out();
bool more_out();

// A rider's mind's decision, `why` it came to it (started, chose, noticed,
// the dolphin): to the log (WR64_COMPANION_LOG=1) and the decisions journal
// -- its personality, what it now does and for how long, its wants and
// fondness, each activity it weighed (a choice), or how strongly it was
// drawn and the chance set against it (a notice, the dolphin).
void log_mind(int which, const char* why) {
    const minds::Mind& m = g_minds[which];
    const char* who = kMindNames[which];
    if (journal::on() && m.who != nullptr && (which == 0 || (which == 1 ? pair_out() : more_out()))) {
        journal::Fields f;
        f.add("who", kMindKeys[which]).add("personality", m.who->name).add("why", why);
        f.add("doing", minds::name(m.doing)).add("for", m.until - m.since);
        static const char* wants[] = { "thrill", "sights", "company", "show_off", "rest", "curiosity" };
        std::string w = "{";
        for (int i = 0; i < minds::kWants; ++i) {
            char item[40];
            std::snprintf(item, sizeof item, "%s\"%s\":%.2f", i ? "," : "", wants[i], m.want[i]);
            w += item;
        }
        f.raw("wants", w + "}").add("fondness", double(m.fondness));
        if (std::strcmp(why, "chose") == 0) {
            std::string list = "[";
            for (size_t i = 0; i < m.weighed.size(); ++i) {
                char item[96];
                std::snprintf(item, sizeof item, "%s{\"activity\":\"%s\",\"score\":%.3f,\"chance\":%.3f}", i ? "," : "",
                              minds::name(m.weighed[i].activity), m.weighed[i].score, m.weighed[i].chance);
                list += item;
            }
            f.raw("weighed", list + "]");
        }
        else if (std::strcmp(why, "started") != 0) {
            f.add("pull", double(m.pull)).add("roll", double(m.roll));
        }
        journal_context(f, which);
        journal::write("rider", f);
    }
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    if (!log) return;
    std::fprintf(stderr, "[wr64-companion] mind: %s (%s) goes for %s for %.0f s; wants thrill %.2f sights %.2f company %.2f "
                 "show-off %.2f rest %.2f curiosity %.2f\n", who, m.who->name, minds::name(m.doing), m.until - m.since,
                 m.want[minds::kThrill], m.want[minds::kSights], m.want[minds::kCompany], m.want[minds::kShowOff],
                 m.want[minds::kRest], m.want[minds::kCuriosity]);
    std::fflush(stderr);
}

constexpr float kLookahead = 900.0f, kTurnRate = 1.1f, kAccel = 450.0f, kSample = 50.0f;
constexpr float kLapRadius = 7000.0f;

// Whether this outing is Stunt Mode's (gGameModes, 11), set as it starts.
bool g_stunt = false;

// The course of this outing (D_800D8170), set as it starts. Away from
// Dolphin Park (course 0), in a free roam, the first rider laps the course
// along its buoys' line (g_course_lap: their middles, in order, as points
// kSample apart), rests or rides along with the player; the pair weave
// along the same line (g_lap_straight: how far each point is from the
// nearest corner of it, for their coming in onto it there), rest by it, or
// ride along.
uint32_t g_course = 0;
bool g_pair_out = false, g_more_out = false;
// The course's ramps, and the riders' map (both below).
constexpr float kRampRoom = 650.0f;
extern std::vector<std::pair<float, float>> g_ramps;
extern std::vector<float> g_ramp_angles;
struct WaterMap;
extern WaterMap g_rider_map;
bool map_open(const WaterMap& m, float x, float z);
// A ramp as ridden: its crest's middle, the way over it, and how far either
// side of its middle a jet ski is on it. All the park's ramps are one hump,
// the same either way (kFaceX/kFaceY from the crest back), 1000 across.
// Away from the park a ramp's face is its slope pieces' (`pieces`): `face`
// long before the crest, `top` high there, left at `launch` speed with a
// `pop` up off it.
struct Ramp {
    float x, z, over_x, over_z, half_width;
    float top = kRampTop, face = kRampCrest - kRampStart, pop = kPop;
    bool pieces = false;
    float launch = kSpeed;
};
// A ramp as the game meets it (away from the park): from its slope pieces,
// its crest's middle (the highest point on the line through its middle,
// the way it is ridden: where its last climb ends), how high that is, how
// far before it its face leaves the water, and the speed and pop it is
// left with.
struct RampShape { float crest_x, crest_z, top, face, pop, launch; };
extern std::vector<RampShape> g_ramp_shapes;
// The top of the ramps' slope pieces at (x, z), or `floor` if higher or
// there are none there.
float ramp_surface(float x, float z, float floor);
extern std::vector<Ramp> g_lap_ramps;
bool pair_out() { return g_pair_out; }
bool more_out() { return g_more_out; }
std::vector<std::pair<float, float>> g_course_lap;
std::vector<float> g_lap_straight;

// The buoy list (D_801AEE20, 0x104 bytes each, counted at D_801BB120): each
// one's middle at +0x00 and +0x08.
void build_course_lap(const uint8_t* rdram) {
    g_course_lap.clear();
    const uint32_t count = std::min<uint32_t>(word_at(rdram, 0x1BB120), 256);
    std::vector<std::pair<float, float>> buoys;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t b = 0x1AEE20 + i * 0x104;
        float x, z;
        const uint32_t wx = word_at(rdram, b), wz = word_at(rdram, b + 8);
        std::memcpy(&x, &wx, 4);
        std::memcpy(&z, &wz, 4);
        if (std::isfinite(x) && std::isfinite(z)) buoys.emplace_back(x, z);
    }
    const size_t n = buoys.size();
    for (size_t i = 0; n >= 3 && i < n; ++i) {
        const auto [ax, az] = buoys[i];
        const auto [bx, bz] = buoys[(i + 1) % n];
        const float length = std::hypot(bx - ax, bz - az);
        for (float t = 0.0f; t < length; t += kSample) g_course_lap.emplace_back(ax + (bx - ax) * t / length, az + (bz - az) * t / length);
    }
    // The course's ramps (the buoys' line runs over Sunny Beach's). One the
    // line meets going the way it is ridden (the way over it: the sine and
    // cosine of its angle, +0x48), the line is laid straight over, through
    // its middle, for the first rider to jump on its laps (g_lap_ramps); round
    // any other, each point within its room pushed out to its edge, away
    // from the ramp's middle -- or the other way, if that way is not open
    // water.
    g_lap_ramps.clear();
    std::vector<bool> over(g_ramps.size(), false);
    for (size_t k = 0; k < g_ramps.size() && g_course_lap.size() > 2; ++k) {
        const auto [rx, rz] = g_ramps[k];
        const float a = g_ramp_angles[k] * 0.0174533f, ox = std::sin(a), oz = std::cos(a);
        size_t near = 0;
        for (size_t i = 1; i < g_course_lap.size(); ++i) {
            if (std::hypot(g_course_lap[i].first - rx, g_course_lap[i].second - rz) <
                std::hypot(g_course_lap[near].first - rx, g_course_lap[near].second - rz)) near = i;
        }
        const auto& a0 = g_course_lap[(near + g_course_lap.size() - 4) % g_course_lap.size()];
        const auto& a1 = g_course_lap[(near + 4) % g_course_lap.size()];
        const float hx = a1.first - a0.first, hz = a1.second - a0.second, length = std::max(1.0f, std::hypot(hx, hz));
        if (std::hypot(g_course_lap[near].first - rx, g_course_lap[near].second - rz) < 400.0f && (hx * ox + hz * oz) / length > 0.9f) {
            over[k] = true;
            const RampShape& s = g_ramp_shapes[k];
            g_lap_ramps.push_back({ s.crest_x, s.crest_z, ox, oz, 400.0f, s.top, s.face, s.pop, true, s.launch });
        }
    }
    for (auto& [x, z] : g_course_lap) {
        for (size_t k = 0; k < g_ramps.size(); ++k) {
            const auto [rx, rz] = g_ramps[k];
            const float dx = x - rx, dz = z - rz, d = std::hypot(dx, dz), room = kRampRoom + 100.0f;
            if (over[k]) {
                // Straight over it, from 1500 before to 1000 past its crest.
                const float a = g_ramp_angles[k] * 0.0174533f, ox = std::sin(a), oz = std::cos(a);
                const float cx = g_ramp_shapes[k].crest_x, cz = g_ramp_shapes[k].crest_z;
                const float along = (x - cx) * ox + (z - cz) * oz, across = (z - cz) * ox - (x - cx) * oz;
                if (along > -1500.0f && along < 1000.0f && std::fabs(across) < 800.0f) { x = cx + ox * along; z = cz + oz * along; }
                continue;
            }
            if (d >= room) continue;
            const float ux = d > 1.0f ? dx / d : 0.0f, uz = d > 1.0f ? dz / d : -1.0f;
            if (map_open(g_rider_map, rx + ux * room, rz + uz * room) || !map_open(g_rider_map, rx - ux * room, rz - uz * room)) {
                x = rx + ux * room; z = rz + uz * room;
            }
            else { x = rx - ux * room; z = rz - uz * room; }
        }
    }
    // The corners: where the line turns more than 17 degrees over 100 units.
    const size_t m = g_course_lap.size();
    g_lap_straight.assign(m, 1e9f);
    auto heading = [&](size_t i) {
        const auto& a = g_course_lap[i % m];
        const auto& b = g_course_lap[(i + 1) % m];
        return std::atan2(b.second - a.second, b.first - a.first);
    };
    for (size_t i = 0; m >= 3 && i < m; ++i) {
        if (std::fabs(std::remainder(heading(i + 1) - heading(i + m - 1), 6.2831853f)) > 0.3f) g_lap_straight[i] = 0.0f;
    }
    // Twice round each way: the distance from the nearest corner.
    for (size_t k = 1; m >= 3 && k < 2 * m; ++k) {
        g_lap_straight[k % m] = std::min(g_lap_straight[k % m], g_lap_straight[(k - 1) % m] + kSample);
        const size_t j = (2 * m - k) % m;
        g_lap_straight[j] = std::min(g_lap_straight[j], g_lap_straight[(j + 1) % m] + kSample);
    }
    std::fprintf(stderr, "[wr64-companion] course %u: a lap along %zu buoys, %zu points\n", g_course, n, g_course_lap.size());
    std::fflush(stderr);
}

// The stunt loop's ramp, ridden east.
constexpr Ramp kLoopRamp[] = { { kRampCrest, (kRampNear + kRampFar) / 2.0f, 1.0f, 0.0f, (kRampFar - kRampNear) / 2.0f - 100.0f } };
// Away from the park: the course's ramps the buoys' line meets head on,
// jumped on the first rider's laps (build_course_lap).
std::vector<Ramp> g_lap_ramps;
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
    if (g_course != 0) return g_course_lap;
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

// Single `index` (0 the first; the trio's leader last), with mind `mind`.
void start_drive(Drive& d, int index, int mind, std::chrono::steady_clock::time_point now) {
    d = Drive{};
    d.mind = mind;
    d.trio = index == kTrioLeader;
    d.top = d.trio ? 750.0f : kSpeed;
    d.last = now;
    d.routine = d.trio ? Routine::Lap : g_stunt ? Routine::Practice : Routine::Stunts;
    d.change_at = now + routine_length();
    // Its mind: already out on its first routine a while.
    minds::Mind& m = g_minds[mind];
    minds::start(m, minds::test_personality(mind), chance(), seconds(now));
    m.doing = d.trio ? minds::Activity::Lap : g_stunt ? minds::Activity::Practice : minds::Activity::Stunts;
    m.until = seconds(now) + std::chrono::duration<double>(routine_length()).count() * 0.5;
    log_mind(mind, "started");
    if (g_course != 0 || index > 0) {
        // Away from the park a third of the way round the buoys' line, out
        // of the player's way; the others spread on round their routes.
        if (g_course != 0) d.routine = Routine::Lap;
        m.doing = d.routine == Routine::Lap ? minds::Activity::Lap : m.doing;
        const auto& points = route(d.routine);
        if (points.empty()) return;
        const size_t at = (points.size() / 3 + size_t(index) * points.size() / 4) % points.size();
        const auto& p = points[at];
        const auto& q = points[(at + 1) % points.size()];
        d.x = p.first; d.z = p.second; d.heading = std::atan2(q.second - p.second, q.first - p.first); d.speed = d.top;
        return;
    }
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

const std::vector<Obstacle>& fixed_obstacles();
// A quiet place to rest, away from the player and from wherever the pair
// are: a sight's spot, and which way to face there.
void rest_spot(float& x, float& z, float& look_x, float& look_z);


// A place `distance` from (x, z), towards (from_x, from_z).
std::pair<float, float> toward(float x, float z, float from_x, float from_z, float distance) {
    const float dx = from_x - x, dz = from_z - z, length = std::max(1.0f, std::hypot(dx, dz));
    return { x + dx / length * distance, z + dz / length * distance };
}

// The first rider starting activity `a`.
void begin_first(Drive& d, minds::Activity a) {
    d.free = Free::None;
    d.parked = false;
    switch (a) {
    case minds::Activity::Stunts: d.routine = Routine::Stunts; d.nearest = -1; break;
    case minds::Activity::Practice: d.routine = Routine::Practice; d.nearest = -1; break;
    case minds::Activity::Follow:
        d.free = Free::Follow;
        d.side = chance()() % 2u ? 1.0f : -1.0f;
        break;
    case minds::Activity::Rest:
        d.free = Free::Rest;
        rest_spot(d.spot_x, d.spot_z, d.look_x, d.look_z);
        if (std::getenv("WR64_COMPANION_LOG")) std::fprintf(stderr, "[wr64-companion] %s rests at %.0f %.0f, facing %.0f %.0f\n", kMindNames[d.mind], d.spot_x, d.spot_z, d.look_x, d.look_z);
        break;
    case minds::Activity::Watch: {
        const minds::Interest& w = g_minds[d.mind].watching;
        d.free = Free::Watch;
        std::tie(d.spot_x, d.spot_z) = toward(w.x, w.z, d.x, d.z, 1000.0f);
        d.look_x = w.x;
        d.look_z = w.z;
        break;
    }
    case minds::Activity::Dolphin: d.free = Free::Dolphin; break;
    default: d.routine = Routine::Lap; d.nearest = -1; break;
    }
}

// Time for something else: the rider's mind chooses (the trio's leader
// never the ramps).
void choose_first(Drive& d, double t) {
    minds::Options o;
    o.open = g_course != 0 || d.trio ? std::vector<minds::Activity>{ minds::Activity::Lap, minds::Activity::Follow, minds::Activity::Rest }
           : g_stunt ? std::vector<minds::Activity>{ minds::Activity::Practice, minds::Activity::Follow, minds::Activity::Rest }
                     : std::vector<minds::Activity>{ minds::Activity::Stunts, minds::Activity::Lap, minds::Activity::Follow,
                                                     minds::Activity::Rest };
    o.player_distance = g_player.known ? std::hypot(g_player.x - d.x, g_player.z - d.z) : -1.0f;
    o.player_moving = g_player.speed > 200.0f;
    begin_first(d, minds::choose(g_minds[d.mind], o, chance(), t));
    log_mind(d.mind, "chose");
}

// The first rider off its routes: alongside the player, a little behind,
// matching its speed; or to a spot to rest or watch from, slowing to a stop
// there and turning to face what it came for (the player, if it is
// watching the player).
void free_ride(Drive& d, float water, float dt, const std::vector<Obstacle>& player) {
    const Player& pl = g_player;
    std::vector<Obstacle> obstacles = fixed_obstacles();
    float tx = d.spot_x, tz = d.spot_z, top = 900.0f;
    if (d.free == Free::Follow) {
        const float fx = std::cos(pl.heading), fz = std::sin(pl.heading);
        const float slot_x = pl.x - fz * d.side * 380.0f - fx * 250.0f, slot_z = pl.z + fx * d.side * 380.0f - fz * 250.0f;
        tx = slot_x + fx * 400.0f;
        tz = slot_z + fz * 400.0f;
        const float ahead = (slot_x - d.x) * std::cos(d.heading) + (slot_z - d.z) * std::sin(d.heading);
        top = std::clamp(pl.speed + 1.2f * ahead, 250.0f, 1300.0f);
        obstacles.push_back({ pl.x, pl.z, 260.0f, true });
    }
    else if (d.free == Free::Dolphin && g_dolphin_seen.moving) {
        // Following the dolphin: 500 behind it, matching its speed.
        const DolphinSeen& o = g_dolphin_seen;
        const float fx = std::cos(o.heading), fz = std::sin(o.heading);
        const float slot_x = o.x - fx * 500.0f, slot_z = o.z - fz * 500.0f;
        tx = slot_x + fx * 300.0f;
        tz = slot_z + fz * 300.0f;
        const float ahead = (slot_x - d.x) * std::cos(d.heading) + (slot_z - d.z) * std::sin(d.heading);
        top = std::clamp(o.speed + 1.2f * ahead, 200.0f, 1300.0f);
        obstacles.push_back({ o.x, o.z, 450.0f, true });
        obstacles.insert(obstacles.end(), player.begin(), player.end());
        d.parked = false;
    }
    else {
        if (d.free == Free::Dolphin) {
            // Watching the dolphin perform: from 800 away on this side of
            // it, facing it; moving up if it has moved on.
            const DolphinSeen& o = g_dolphin_seen;
            const float from = std::hypot(d.spot_x - o.x, d.spot_z - o.z);
            if (from < 500.0f || from > 1200.0f) {
                std::tie(d.spot_x, d.spot_z) = toward(o.x, o.z, d.x, d.z, 800.0f);
                d.parked = false;
            }
            tx = d.spot_x;
            tz = d.spot_z;
            d.look_x = o.x;
            d.look_z = o.z;
        }
        obstacles.insert(obstacles.end(), player.begin(), player.end());
        if (d.free == Free::Watch && pl.known && std::hypot(pl.x - g_minds[d.mind].watching.x, pl.z - g_minds[d.mind].watching.z) < 3000.0f) {
            d.look_x = pl.x;
            d.look_z = pl.z;
        }
        const float left = std::hypot(tx - d.x, tz - d.z);
        if (!d.parked && left < 150.0f) d.parked = true;
        top = std::min(900.0f, std::max(200.0f, left * 0.9f));
    }
    // The trio's leader no faster than its two can keep up.
    if (d.trio) top = std::min(top, 1000.0f);
    float want_heading;
    if (d.parked) {
        // Coming to rest, turning slowly to face what it came for.
        d.speed = std::max(0.0f, d.speed - 500.0f * dt);
        want_heading = std::atan2(d.look_z - d.z, d.look_x - d.x);
        const float diff = std::remainder(want_heading - d.heading, 6.2831853f);
        const float step = std::clamp(diff, -0.4f * dt, 0.4f * dt);
        d.heading = std::remainder(d.heading + step, 6.2831853f);
        d.turning += ((dt > 0.0f ? step / dt : 0.0f) - d.turning) * std::min(1.0f, 6.0f * dt);
    }
    else {
        std::tie(tx, tz) = rider_waypoint(d.way, d.x, d.z, tx, tz);
        const Avoid av = avoid(d.x, d.z, d.heading, d.speed, obstacles);
        want_heading = std::atan2(tz - d.z, tx - d.x) + av.turn;
        const float diff = std::remainder(want_heading - d.heading, 6.2831853f);
        const float step = std::clamp(diff, -1.6f * dt, 1.6f * dt);
        d.heading = std::remainder(d.heading + step, 6.2831853f);
        d.turning += ((dt > 0.0f ? step / dt : 0.0f) - d.turning) * std::min(1.0f, 6.0f * dt);
        const float wanted = top * av.slow * (std::fabs(diff) > 1.0f ? 0.6f : 1.0f);
        d.speed = d.speed < wanted ? std::min(wanted, d.speed + kAccel * dt) : std::max(wanted, d.speed - 700.0f * dt);
    }
    step_on(d.x, d.z, d.heading, d.speed, dt);
    d.bank = g_pose.x_right * kLean * std::clamp(d.turning * d.speed / (kSpeed * kSpeed / kRadius), -1.0f, 1.0f);
    d.flip = 0.0f;
    d.on_ramp = false;
    d.y = water;
    d.climb = 0.0f;
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

// Whether the first rider, lapping away from the park, is on its run at a
// ramp it jumps there: lined up with it from 1500 before its crest to 300 past.
bool run_at_ramp(const Drive& d) {
    if (d.routine != Routine::Lap || g_course == 0 || d.trio) return false;
    for (const Ramp& r : g_lap_ramps) {
        const float rx = d.x - r.x, rz = d.z - r.z;
        const float s = rx * r.over_x + rz * r.over_z, across = rz * r.over_x - rx * r.over_z;
        if (s > -1500.0f && s < 300.0f && std::fabs(across) < r.half_width + 200.0f) return true;
    }
    return false;
}

void drive(Drive& d, uint32_t segment3, std::chrono::steady_clock::time_point now, float dt, const std::vector<Obstacle>& obstacles) {
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    const float water = water_height_at(segment3, d.x, d.z);

    // Time for something else, as its mind sees it (not mid-jump).
    const double t = seconds(now);
    minds::feel(g_minds[d.mind], t, dt);
    if (!d.airborne && d.crash == Crash::None && !d.on_ramp && minds::restless(g_minds[d.mind], t)) choose_first(d, t);

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
                    std::fprintf(stderr, "[wr64-companion] %s bailed the landing\n", kMindNames[d.mind]);
                    std::fflush(stderr);
                }
            }
            else {
                d.bank = d.flip = 0.0f;
            }
        }
        return;
    }

    // Off its routes.
    if (d.free != Free::None) {
        free_ride(d, water, dt, obstacles);
        return;
    }

    // On the water: steering for a point ahead on its route, up to speed,
    // swerving round (and slowing for) the player.
    const Avoid av = avoid(d.x, d.z, d.heading, d.speed, obstacles);
    const float top = d.top * av.slow;
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
    const auto& on = points[(d.nearest + int(kLookahead / kSample)) % n];
    // Back to its route from afar, the way round any land (away from the
    // park) -- but not on the run at a ramp it jumps on its laps.
    const bool at_ramp = run_at_ramp(d);
    const auto target = at_ramp ? on : rider_waypoint(d.way, d.x, d.z, on.first, on.second);
    const float want = std::atan2(target.second - d.z, target.first - d.x) + av.turn;
    const float diff = std::remainder(want - d.heading, 6.2831853f);
    const float step = std::clamp(diff, -kTurnRate * dt, kTurnRate * dt);
    d.heading = std::remainder(d.heading + step, 6.2831853f);
    d.turning += ((dt > 0.0f ? step / dt : 0.0f) - d.turning) * std::min(1.0f, 6.0f * dt);
    if (at_ramp) {
        d.x += std::cos(d.heading) * d.speed * dt;
        d.z += std::sin(d.heading) * d.speed * dt;
    }
    else step_on(d.x, d.z, d.heading, d.speed, dt);
    // Into the turn: a heading growing is a turn to the right, and a bank towards the hull's x.
    d.bank = g_pose.x_right * kLean * std::clamp(d.turning / (kSpeed / kRadius), -1.0f, 1.0f);
    d.flip = 0.0f;

    // A ramp: up its face, heading over it on a run that rides it, and off
    // the crest into the air with a stunt picked. (`along`, how far past the
    // crest, is measured as the stunt loop's ramp's x.)
    bool on_face = false, past_crest = false;
    float along = 0.0f;
    const Ramp* hit = nullptr;
    float along_crest = 0.0f;
    const bool lap_ramps = d.routine == Routine::Lap && g_course != 0;
    const Ramp* ramps = d.routine == Routine::Stunts ? kLoopRamp : d.routine == Routine::Practice ? kStuntRamps
                      : lap_ramps ? g_lap_ramps.data() : nullptr;
    const int ramp_count = d.trio ? 0 : d.routine == Routine::Stunts ? 1 : d.routine == Routine::Practice ? 4
                         : lap_ramps ? int(g_lap_ramps.size()) : 0;
    for (int i = 0; i < ramp_count; ++i) {
        const Ramp& r = ramps[i];
        const float rx = d.x - r.x, rz = d.z - r.z;
        const float s = rx * r.over_x + rz * r.over_z, across = rz * r.over_x - rx * r.over_z;
        const bool lined_up = std::cos(d.heading) * r.over_x + std::sin(d.heading) * r.over_z > 0.95f &&
                              std::fabs(across) < r.half_width;
        if (!lined_up) continue;
        along = kRampCrest + s;
        if (s >= -r.face && s < 0.0f) on_face = true;
        else if (s >= 0.0f && s < 200.0f) past_crest = true;
        if (on_face || past_crest) { hit = &r; along_crest = s; break; }
    }
    if (on_face && hit->pieces) {
        d.on_ramp = true;
        // Slowing up the face, to its launch speed at the crest.
        d.speed = std::min(d.speed, hit->launch + (d.top - hit->launch) * std::clamp(-along_crest / hit->face, 0.0f, 1.0f));
        const float hx = std::cos(d.heading) * 10.0f, hz = std::sin(d.heading) * 10.0f;
        d.y = ramp_surface(d.x, d.z, water);
        d.climb = (ramp_surface(d.x + hx, d.z + hz, water) - ramp_surface(d.x - hx, d.z - hz, water)) / 20.0f;
    }
    else if (on_face) {
        d.on_ramp = true;
        d.y = std::max(water, face_height(along));
        d.climb = (face_height(along + 10.0f) - face_height(along - 10.0f)) / 20.0f;
    }
    else if (d.on_ramp && past_crest) {
        d.on_ramp = false;
        d.airborne = true;
        d.air = 0.0f;
        d.y = hit->top;
        d.rise = hit->pop;
        if (hit->pieces) d.speed = hit->launch;
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
            std::fprintf(stderr, "[wr64-companion] %s off the ramp: %s%s\n", kMindNames[d.mind], names[d.stunt], d.failed ? ", going wrong" : "");
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
// Sightseeing: off side by side to one of the park's sights (kParkSights) --
// the piers, the island, the stone arches, either ramp -- by chance, the
// last straight run in towards it, then stopped abreast, facing it, for 10
// to 30 seconds; then on to another.
//
// (Away from the park: no sights yet, only the quiet spots by the buoys'
// line to rest at; the weave goes along that line; no trick.)
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

enum class PairRoutine { Sights, Weave, Trick, Follow, Watch, Dolphin };
enum class TrickStep { Gather, Wait, Run };
struct Rider {
    float x = 0, y = 0, z = 0, heading = 0, speed = 0, turning = 0, bank = 0, climb = 0;
    Way way;
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
    bool resting = false;   // at a sight to rest, staying as long as its mind wants
    float watch_x = 0, watch_z = 0;   // watching: what, and from where
    float spot_x = 0, spot_z = 0;
    Rider riders[2];
} g_pair;

constexpr float kPairSpeed = 900.0f, kPairTurnRate = 2.3f;
// Sightseeing: how far apart they stop, and the straight run in.
constexpr float kAbreast = 260.0f, kRunIn = 500.0f;

struct Sight { const char* name; float x, z, look_x, look_z; };
constexpr Sight kParkSights[] = {
    { "the piers", 150, 1150, -450, 350 },
    { "the island", 650, 4800, 100, 5190 },
    { "the stone arches", 3250, 2450, 3250, 3600 },
    { "the ramp", 1450, 2350, 1441, 3619 },
    { "the other ramp", -1900, 2900, -1142, 4199 },
    // Not a sight: where they gather for the trick, facing the other ramp.
    { "the start of a jump", 900, 2700, -1142, 4199 },
};
constexpr int kStage = int(sizeof(kParkSights) / sizeof(kParkSights[0])) - 1;
// The sights the user marked on a ride round each course (F2, marks.txt,
// 2026-11-02): where they stopped, and the way they looked (degrees, 0
// along +x, 90 along +z: the mark's facing, which was then written 180
// degrees out, turned round; at the tents, the way the camera looked).
struct MarkedSight { uint32_t course; const char* name; float x, z, look; };
constexpr MarkedSight kMarkedSights[] = {
    { 1, "the stands", 760, 645, -61 },
    { 1, "the tents", 3469, -397, 40 },
    { 1, "the ramp", -907, -2220, 115 },
    { 1, "the open water south of the track", 2092, -3506, 77 },
    { 1, "the island's south-east corner", -12847, -187, 109 },
    { 1, "the northern cove", 11509, 784, -130 },
    { 1, "the south-west beach", 13155, -2622, 84 },
};
// This outing's: the park's; or away from it the course's marked sights
// (the first g_marked), then the quiet spots by the buoys' line
// (build_course_spots), to rest at.
std::vector<Sight> g_sights(std::begin(kParkSights), std::end(kParkSights));
int g_marked = 0;
// Away from the park the last is where they gather for the trick, if the
// course has a ramp for it (g_trick.stage).
// (`pieces`: its face is its slope pieces', `face` long, `top` high at the
// crest, with a `pop` off it; otherwise the park's.)
struct TrickRamp {
    float crest_x, crest_z, over_x, over_z;
    int stage;
    float top = kRampTop, face = kRampCrest - kRampStart, pop = kPop;
    bool pieces = false;
    float launch = kSpeed;
};
TrickRamp g_trick{};
int sight_count() { return g_course == 0 ? kStage : int(g_sights.size()) - (g_trick.stage >= 0 ? 1 : 0); }
// When each sight was last visited (seconds, the minds' clock): riders go
// where nobody has been for longest, so all of a course gets seen.
std::vector<double> g_sight_seen;
void seen_sight(int sight, double t) {
    if (sight >= 0 && sight < int(g_sight_seen.size())) g_sight_seen[size_t(sight)] = t;
}
double since_seen(int sight, double t) {
    return sight >= 0 && sight < int(g_sight_seen.size()) ? std::min(600.0, t - g_sight_seen[size_t(sight)]) : 600.0;
}

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
// The mines some courses lay in the water (Sunny Beach's four in a square
// west of its ramp, the user's F2 mark, 2026-11-03): in the list of things
// floating after the buoys' count (D_801BB120), 0x18 apart from 0x801BB138
// (x, y, z, a radius of 25, then the kind: 3 a mine, 1 an edge buoy), read
// as the course starts; kept clear of by kMineRoom.
constexpr uint32_t kMines = 0x1BB138, kMineSize = 0x18, kMineKind = 3;
constexpr float kMineRoom = 300.0f;
std::vector<std::pair<float, float>> g_mines;

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
// Away from the park: the course's ramps (g_ramps, below).
std::vector<Obstacle> g_ramp_obstacles;
const std::vector<Obstacle>& fixed_obstacles() {
    if (g_course != 0) return g_ramp_obstacles;
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
    const Sight& s = g_sights[sight];
    float fx = s.look_x - s.x, fz = s.look_z - s.z;
    const float length = std::hypot(fx, fz);
    fx /= length; fz /= length;
    const float side = (i == 0 ? 0.5f : -0.5f) * kAbreast;
    return { s.x - fz * side, s.z + fx * side, fx, fz };
}

// The weave's loop's line `s` units round: the place, the way on, and how
// far it is to the nearer end of the straight it is on (0 on the turns).
struct LoopPlace { float x, z, tx, tz, straight; };
// The weave's loop's length: away from the park, the buoys' line's.
float weave_loop() { return g_course == 0 || g_course_lap.empty() ? kWeaveLoop : float(g_course_lap.size()) * kSample; }
LoopPlace loop_place(float s) {
    if (g_course != 0 && !g_course_lap.empty()) {
        const size_t n = g_course_lap.size();
        const float u = std::fmod(s / kSample, float(n)), w = u < 0.0f ? u + float(n) : u;
        const size_t i = size_t(w) % n;
        const float t = w - std::floor(w);
        const auto& a = g_course_lap[i];
        const auto& b = g_course_lap[(i + 1) % n];
        const float length = std::max(1e-3f, std::hypot(b.first - a.first, b.second - a.second));
        return { a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t,
                 (b.first - a.first) / length, (b.second - a.second) / length, g_lap_straight[i] };
    }
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
    float swing = (i == 0 ? 1.0f : -1.0f) * kWeaveWidth * t * t * (3.0f - 2.0f * t) *
                  std::sin(6.2831853f * s / kWeaveWave);
    // Away from the park, narrower where the swing would leave open water
    // (the shallows by Sunny Beach's sandbar, a ramp's room).
    if (g_course != 0) {
        for (int k = 0; k < 3 && !map_open(g_rider_map, c.x - c.tz * swing, c.z + c.tx * swing); ++k) swing *= 0.4f;
    }
    return { c.x - c.tz * swing, c.z + c.tx * swing };
}

// Moves rider `r` on along its heading, leaning into its turn.
// (`straight`: on, whatever is there -- the run at a ramp.)
void move(Rider& r, float dt, bool straight = false) {
    if (straight) {
        r.x += std::cos(r.heading) * r.speed * dt;
        r.z += std::sin(r.heading) * r.speed * dt;
    }
    else step_on(r.x, r.z, r.heading, r.speed, dt);
    r.bank = g_pose.x_right * kLean * std::clamp(r.turning * r.speed / (kSpeed * kSpeed / kRadius), -1.0f, 1.0f);
}

// Turns rider `r` towards (tx, tz), aside from `obstacles`, bringing it up
// or down towards `top` units a second, and moves it on.
void steer(Rider& r, float tx, float tz, float top, float dt, const std::vector<Obstacle>& obstacles, bool straight = false) {
    if (!straight) std::tie(tx, tz) = rider_waypoint(r.way, r.x, r.z, tx, tz);
    const Avoid av = avoid(r.x, r.z, r.heading, r.speed, obstacles);
    const float want = std::atan2(tz - r.z, tx - r.x) + av.turn;
    const float diff = std::remainder(want - r.heading, 6.2831853f);
    const float step = std::clamp(diff, -kPairTurnRate * dt, kPairTurnRate * dt);
    r.heading = std::remainder(r.heading + step, 6.2831853f);
    r.turning += ((dt > 0.0f ? step / dt : 0.0f) - r.turning) * std::min(1.0f, 6.0f * dt);
    // Slower round a sharp turn.
    const float wanted = top * av.slow * (std::fabs(diff) > 1.0f ? 0.6f : 1.0f);
    r.speed = r.speed < wanted ? std::min(wanted, r.speed + kAccel * dt) : std::max(wanted, r.speed - 700.0f * dt);
    move(r, dt, straight);
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

// Whether sight `sight` is there to see: in Stunt Mode the park's ramps are not.
bool sight_there(int sight);

bool rider_water(const uint8_t* rdram, uint32_t course, float x, float z);
bool inside_boundary(const uint8_t* rdram, float x, float z);

// Away from the park: eight quiet spots, evenly round the buoys' line, each
// 400 or 650 to one side of it, facing on along it towards a point on it
// 1200 ahead -- to rest and watch the riders go by. Beside the line, not
// out from it, as the land is near it on some courses (Sunny Beach's
// sandbar); in water, with room for the pair abreast and their run in.
void build_course_spots(const uint8_t* rdram) {
    g_sights.clear();
    for (const MarkedSight& m : kMarkedSights) {
        if (m.course != g_course) continue;
        const float a = m.look / 57.29578f;
        g_sights.push_back({ m.name, m.x, m.z, m.x + std::cos(a) * 1200.0f, m.z + std::sin(a) * 1200.0f });
    }
    g_marked = int(g_sights.size());
    const size_t n = g_course_lap.size();
    for (int k = 0; n >= 2 && k < 8; ++k) {
        const size_t at = n * size_t(k) / 8;
        const auto [x, z] = g_course_lap[at];
        const auto [nx, nz] = g_course_lap[(at + 1) % n];
        const auto [lx, lz] = g_course_lap[(at + size_t(1200.0f / kSample)) % n];
        const float length = std::max(1.0f, std::hypot(nx - x, nz - z));
        const float ax = -(nz - z) / length, az = (nx - x) / length;
        for (const float off : { 400.0f, -400.0f, 650.0f, -650.0f }) {
            const float sx = x + ax * off, sz = z + az * off;
            const float fl = std::max(1.0f, std::hypot(lx - sx, lz - sz)), fx = (lx - sx) / fl, fz = (lz - sz) / fl;
            const float side = kAbreast / 2.0f + 100.0f, back = kRunIn + 200.0f;
            const float checks[5][2] = { { 0.0f, 0.0f }, { -fz * side, fx * side }, { fz * side, -fx * side },
                                         { -fx * back / 2.0f, -fz * back / 2.0f }, { -fx * back, -fz * back } };
            bool open = true;
            for (const auto& c : checks) {
                if (!rider_water(rdram, g_course, sx + c[0], sz + c[1])) open = false;
            }
            if (!open) continue;
            g_sights.push_back({ "a quiet spot by the buoys", sx, sz, lx, lz });
            break;
        }
    }
    std::fprintf(stderr, "[wr64-companion] course %u: %d marked sights, %zu quiet spots by the buoys\n", g_course, g_marked, g_sights.size() - size_t(g_marked));
    // The trick, over the course's first ramp, ridden the way it goes (the
    // sine and cosine of its angle), if the water before it is open: they
    // gather 2000 before its crest, facing it.
    g_trick.stage = -1;
    if (!g_ramps.empty()) {
        const float a = g_ramp_angles[0] * 0.0174533f, ox = std::sin(a), oz = std::cos(a);
        const RampShape& shape = g_ramp_shapes[0];
        const float cx = shape.crest_x, cz = shape.crest_z;
        const float sx = cx - ox * 2000.0f, sz = cz - oz * 2000.0f;
        bool open = true;
        for (const float back : { 0.0f, 300.0f, 700.0f }) {
            for (const float side : { -200.0f, 0.0f, 200.0f }) {
                if (!rider_water(rdram, g_course, sx - ox * back + oz * side, sz - oz * back - ox * side)) open = false;
            }
        }
        if (open) {
            g_trick = { cx, cz, ox, oz, int(g_sights.size()), shape.top, shape.face, shape.pop, true, shape.launch };
            g_sights.push_back({ "the start of a jump", sx, sz, cx, cz });
        }
        std::fprintf(stderr, "[wr64-companion] course %u: the pair's trick %s\n", g_course, open ? "over its ramp" : "not here (no open water before its ramp)");
    }
    g_sight_seen.assign(g_sights.size(), -1e9);
    std::fflush(stderr);
}

void rest_spot(float& x, float& z, float& look_x, float& look_z) {
    float best = -1.0f;
    const double t = seconds(std::chrono::steady_clock::now());
    int chosen = -1;
    for (int s = 0; s < sight_count(); ++s) {
        if (!sight_there(s) || s == g_pair.sight) continue;
        const Spot spot = sight_spot(s, 0);
        const float away = g_player.known ? std::hypot(spot.x - g_player.x, spot.z - g_player.z) : 0.0f;
        const float score = float(since_seen(s, t)) * 10.0f + std::min(away, 6000.0f) * 0.3f + float(chance()() % 1500u);
        if (score > best) {
            chosen = s;
            best = score;
            x = spot.x - spot.fx * 300.0f;
            z = spot.z - spot.fz * 300.0f;
            look_x = g_sights[s].look_x;
            look_z = g_sights[s].look_z;
        }
    }
    seen_sight(chosen, t);
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
    const float wx = g_sights[sight].x - a.x, wz = g_sights[sight].z - a.z;
    p.side = (c.z - a.z) * wx - (c.x - a.x) * wz >= 0.0f ? 1.0f : -1.0f;
}

// Whether sight `sight` is there to see: in Stunt Mode the park's ramps are not.
bool sight_there(int sight) { return g_course != 0 || !g_stunt || (sight != 3 && sight != 4); }

// Off to a sight, by chance not the one they are at.
void pick_sight(Pair& p, std::chrono::steady_clock::time_point now) {
    const int choices = g_course != 0 && !p.resting && g_marked > 0 ? g_marked : sight_count();
    const int from = p.sight;
    const double t = seconds(now);
    int to = 0;
    float best = -1.0f;
    for (int c = 0; c < choices; ++c) {
        if ((c == from && choices > 1) || !sight_there(c)) continue;
        const float score = float(since_seen(c, t)) + float(chance()() % 120u);
        if (score > best) { best = score; to = c; }
    }
    seen_sight(to, t);
    head_for(p, to, now);
    char what[96];
    std::snprintf(what, sizeof what, "off to see %s", g_sights[p.sight].name);
    log_pair(what);
}

// Onto the weave, the formation's place set some way on round the loop from
// the nearest to them.
void start_weave(Pair& p, std::chrono::steady_clock::time_point now) {
    p.change_at = now + pair_routine_length();
    const float mx = (p.riders[0].x + p.riders[1].x) / 2.0f, mz = (p.riders[0].z + p.riders[1].z) / 2.0f;
    float best = 0.0f, nearest = 1e30f;
    for (float s = 0.0f; s < weave_loop(); s += 50.0f) {
        const LoopPlace c = loop_place(s);
        const float d = std::hypot(c.x - mx, c.z - mz);
        if (d < nearest) { nearest = d; best = s; }
    }
    p.along = best + 1500.0f;
    p.routine = PairRoutine::Weave;
    for (Rider& r : p.riders) r.near = r.stopped = false;
    log_pair(g_course == 0 ? "off to weave between the piers and the ramps" : "off to weave along the buoys");
}

// Off to gather for the trick.
void start_trick(Pair& p, std::chrono::steady_clock::time_point now) {
    p.routine = PairRoutine::Trick;
    p.step = TrickStep::Gather;
    for (Rider& r : p.riders) {
        r.on_ramp = r.airborne = r.landed = r.failed = false;
        r.crash = Crash::None;
    }
    head_for(p, g_trick.stage, now);
    log_pair(g_course == 0 ? "off to jump the other ramp together" : "off to jump the ramp together");
}

// Watching: side by side, some way off what caught their eye, facing it.
void start_watch(Pair& p) {
    p.routine = PairRoutine::Watch;
    const minds::Interest& w = g_minds[1].watching;
    p.watch_x = w.x;
    p.watch_z = w.z;
    const float mx = (p.riders[0].x + p.riders[1].x) / 2.0f, mz = (p.riders[0].z + p.riders[1].z) / 2.0f;
    std::tie(p.spot_x, p.spot_z) = open_spot_by(w.x, w.z, mx, mz, 1100.0f);
    for (Rider& r : p.riders) r.near = r.stopped = false;
    log_pair("off to watch the player");
}

// What next, as their mind chooses (in Stunt Mode, no trick: its ramp is
// not there; a rest is a long stop at a sight).
void next_routine(Pair& p, std::chrono::steady_clock::time_point now) {
    p.change = false;
    minds::Options o;
    // Away from the park, sights only where the course has marked ones, and
    // no trick (its ramp is the park's).
    if (g_course != 0) {
        o.open = { minds::Activity::Weave, minds::Activity::Follow, minds::Activity::Rest };
        if (g_marked > 0) o.open.push_back(minds::Activity::Sights);
    }
    else o.open = { minds::Activity::Sights, minds::Activity::Weave, minds::Activity::Follow, minds::Activity::Rest };
    if ((!g_stunt && g_course == 0) || (g_course != 0 && g_trick.stage >= 0)) o.open.push_back(minds::Activity::Trick);
    const float mx = (p.riders[0].x + p.riders[1].x) / 2.0f, mz = (p.riders[0].z + p.riders[1].z) / 2.0f;
    o.player_distance = g_player.known ? std::hypot(g_player.x - mx, g_player.z - mz) : -1.0f;
    o.player_moving = g_player.speed > 200.0f;
    const minds::Activity a = minds::choose(g_minds[1], o, chance(), seconds(now));
    log_mind(1, "chose");
    p.resting = false;
    switch (a) {
    case minds::Activity::Rest:
        p.resting = true;
        [[fallthrough]];
    case minds::Activity::Sights:
        p.routine = PairRoutine::Sights;
        p.change_at = now + pair_routine_length();
        pick_sight(p, now);
        break;
    case minds::Activity::Weave: start_weave(p, now); break;
    case minds::Activity::Trick: start_trick(p, now); break;
    case minds::Activity::Follow:
        p.routine = PairRoutine::Follow;
        log_pair("off to ride along with the player");
        break;
    default: start_weave(p, now); break;
    }
}

void start_pair(std::chrono::steady_clock::time_point now) {
    Pair& p = g_pair;
    p = Pair{};
    p.change_at = now + pair_routine_length();
    // Their mind: already stopped at a sight a while.
    minds::start(g_minds[1], minds::test_personality(1), chance(), seconds(now));
    // (Away from the park, resting at a quiet spot: there are no sights yet.)
    g_minds[1].doing = g_course == 0 ? minds::Activity::Sights : minds::Activity::Rest;
    p.resting = g_course != 0;
    g_minds[1].until = seconds(now) + std::chrono::duration<double>(pair_routine_length()).count() * 0.5;
    log_mind(1, "started");
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
        return value != nullptr && value[0] >= '0' && value[0] < '0' + sight_count() && std::getenv("WR64_INPUT_SCRIPT") != nullptr
                   ? value[0] - '0' : -1;
    }();
    // (WR64_COMPANION_TEST_PAIR=trick, scripted: from the ramp's, off to
    // the trick at once.)
    static const bool test_trick = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_PAIR");
        return value != nullptr && value[0] == 't' && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
    }();
    const bool trick = test_trick && g_trick.stage >= 0;
    if (sight_count() <= 0) return;
    p.sight = trick ? (g_course == 0 ? 3 : 0) : test_sight >= 0 && test_sight < sight_count() ? test_sight : int(chance()() % uint32_t(sight_count()));
    while (!sight_there(p.sight)) p.sight = int(chance()() % uint32_t(sight_count()));
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
    const Sight& sight = g_sights[p.sight];

    // Side by side on the way: the second keeps kAbreast to the first's
    // side, level with it, until the first is nearly there; the first eases
    // off while the second is behind its place.
    const Rider& first = p.riders[0];
    const float fx = std::cos(first.heading), fz = std::sin(first.heading);
    // (Away from the park, on the other side where its side is shallows.)
    if (g_course != 0 && !map_open(g_rider_map, first.x - fz * p.side * kAbreast, first.z + fx * p.side * kAbreast) &&
        map_open(g_rider_map, first.x + fz * p.side * kAbreast, first.z - fx * p.side * kAbreast)) {
        p.side = -p.side;
    }
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
    const Sight& sight = g_sights[p.sight];
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
        seen_sight(p.sight, seconds(now));
        const double rest = g_minds[1].until - seconds(now);
        p.leave_at = now + std::chrono::milliseconds(int((p.resting ? std::max(5.0, rest) : double(uniform(10.0f, 30.0f))) * 1000.0));
        char what[96];
        std::snprintf(what, sizeof what, p.resting ? "stopped to rest by %s" : "stopped to look at %s", sight.name);
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
    p.along = std::fmod(p.along + kWeaveSpeed * dt * std::clamp(1.0f - (far - 300.0f) / 3000.0f, 0.3f, 1.0f), weave_loop());
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
    const Sight& stage = g_sights[g_trick.stage];
    const float crest_x = g_trick.crest_x, crest_z = g_trick.crest_z, over_x = g_trick.over_x, over_z = g_trick.over_z;
    const float across_x = over_z, across_z = -over_x;
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
        // (Away from the park, from the ramp: the first rider jumps it on its laps.)
        const bool clear = first == nullptr || (g_course == 0 ? std::hypot(first->x - kCrossingX, first->z - kCrossingZ) > 3500.0f
                                                              : std::hypot(first->x - crest_x, first->z - crest_z) > 3000.0f);
        if (now - p.waiting_since < std::chrono::seconds(2) || !clear) return;
        p.step = TrickStep::Run;
        // Lanes by where they are: the one further across, the far lane. The
        // rider's right is (-z, x) of the way it goes.
        const float right_x = -over_z, right_z = over_x;
        const float across0 = (p.riders[0].x - crest_x) * across_x + (p.riders[0].z - crest_z) * across_z;
        const float across1 = (p.riders[1].x - crest_x) * across_x + (p.riders[1].z - crest_z) * across_z;
        for (int i = 0; i < 2; ++i) {
            Rider& r = p.riders[i];
            r.lane = ((i == 0) == (across0 > across1)) ? kLane : -kLane;
            const bool right = r.lane * (across_x * right_x + across_z * right_z) > 0.0f;
            r.roll = right ? g_pose.x_right : -g_pose.x_right;
            r.failed = chance()() % 100u < 5u || test_fail(!right);
        }
        if (log) {
            auto side = [&](const Rider& r) { return r.lane * (across_x * right_x + across_z * right_z) > 0.0f ? "right" : "left"; };
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
        along[i] = (p.riders[i].x - crest_x) * over_x + (p.riders[i].z - crest_z) * over_z;
    }
    bool done = true;
    for (int i = 0; i < 2; ++i) {
        Rider& r = p.riders[i];
        const float water = water_height_at(segment3, r.x, r.z);
        if (r.landed) {
            // Away from the park, down and still on: riding on, clear of
            // what is about (Sunny Beach's mines are just beyond), not
            // slowing to a stop.
            if (g_course != 0 && r.crash == Crash::None) {
                // (Out to the side of its lane, round them, not between.)
                const float on = along[i] + 800.0f;
                steer(r, crest_x + over_x * on + across_x * r.lane * 3.0f, crest_z + over_z * on + across_z * r.lane * 3.0f, 850.0f, dt,
                      obstacles[i]);
                r.y = water_height_at(segment3, r.x, r.z);
                r.climb = 0.0f;
                continue;
            }
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
        // (Straight there: not round the ramp, as the way round land would.)
        steer(r, crest_x + over_x * ahead + across_x * r.lane, crest_z + over_z * ahead + across_z * r.lane,
              std::clamp(kSpeed + 1.5f * (along[1 - i] - along[i]), 700.0f, 1150.0f), dt, {}, true);
        const float s = (r.x - crest_x) * over_x + (r.z - crest_z) * over_z;
        const float across = (r.x - crest_x) * across_x + (r.z - crest_z) * across_z;
        if (s >= -g_trick.face && s < 0.0f && std::fabs(across) < kRampHalfWidth) {
            r.on_ramp = true;
            if (g_trick.pieces) {
                r.speed = std::min(r.speed, g_trick.launch + (kSpeed - g_trick.launch) * std::clamp(-s / g_trick.face, 0.0f, 1.0f));
                const float hx = over_x * 10.0f, hz = over_z * 10.0f;
                r.y = ramp_surface(r.x, r.z, water);
                r.climb = (ramp_surface(r.x + hx, r.z + hz, water) - ramp_surface(r.x - hx, r.z - hz, water)) / 20.0f;
            }
            else {
                r.y = std::max(water, face_height(kRampCrest + s));
                r.climb = (face_height(kRampCrest + s + 10.0f) - face_height(kRampCrest + s - 10.0f)) / 20.0f;
            }
        }
        else if (r.on_ramp && s >= 0.0f) {
            r.on_ramp = false;
            r.airborne = true;
            r.air = 0.0f;
            r.y = g_trick.top;
            r.rise = g_trick.pop;
            if (g_trick.pieces) r.speed = g_trick.launch;
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

// Riding along with the player, one either side of it, a little behind,
// matching its speed; or watching: side by side, some way off, stopping and
// turning to face the player.
void ride_along(Pair& p, std::chrono::steady_clock::time_point now, float dt, const std::vector<Obstacle> obstacles[2]) {
    if (p.change || !g_player.known) {
        next_routine(p, now);
        return;
    }
    const Player& pl = g_player;
    const float fx = std::cos(pl.heading), fz = std::sin(pl.heading);
    for (int i = 0; i < 2; ++i) {
        Rider& r = p.riders[i];
        const float side = i == 0 ? -1.0f : 1.0f;
        if (p.routine == PairRoutine::Follow) {
            const float slot_x = pl.x - fz * side * 420.0f - fx * 300.0f, slot_z = pl.z + fx * side * 420.0f - fz * 300.0f;
            const float ahead = (slot_x - r.x) * std::cos(r.heading) + (slot_z - r.z) * std::sin(r.heading);
            std::vector<Obstacle> near = fixed_obstacles();
            near.push_back({ pl.x, pl.z, 260.0f, true });
            near.push_back({ p.riders[1 - i].x, p.riders[1 - i].z, 180.0f, false });
            steer(r, slot_x + fx * 400.0f, slot_z + fz * 400.0f, std::clamp(pl.speed + 1.2f * ahead, 250.0f, 1300.0f), dt, near);
            continue;
        }
        // Watching: abreast across the line to what they watch.
        const float lx = p.watch_x - p.spot_x, lz = p.watch_z - p.spot_z, length = std::max(1.0f, std::hypot(lx, lz));
        const float sx = p.spot_x - lz / length * side * kAbreast / 2.0f, sz = p.spot_z + lx / length * side * kAbreast / 2.0f;
        const float left = std::hypot(sx - r.x, sz - r.z);
        if (!r.stopped && left < 150.0f) r.stopped = true;
        const bool there = std::hypot(pl.x - p.watch_x, pl.z - p.watch_z) < 3000.0f;
        if (r.stopped) settle(r, there ? pl.x : p.watch_x, there ? pl.z : p.watch_z, dt);
        else steer(r, sx, sz, std::min(kPairSpeed, std::max(200.0f, left * 0.9f)), dt, obstacles[i]);
    }
}

// Taken with the dolphin (below): when it swims on, following side by side
// 600 behind it, matching its speed; when it performs in place, abreast
// some 850 off on this side of it, stopped and facing it, moving up if it
// has moved on.
void start_pair_dolphin(Pair& p) {
    if (p.routine == PairRoutine::Trick) return;
    p.routine = PairRoutine::Dolphin;
    p.change = p.parked = false;
    p.spot_x = p.spot_z = 1e9f;
    for (Rider& r : p.riders) r.near = r.stopped = false;
    log_pair("off to watch the dolphin");
}

void ride_dolphin(Pair& p, std::chrono::steady_clock::time_point now, float dt, const std::vector<Obstacle> obstacles[2]) {
    const DolphinSeen& o = g_dolphin_seen;
    if (p.change || !o.out) {
        next_routine(p, now);
        return;
    }
    const float mx = (p.riders[0].x + p.riders[1].x) / 2.0f, mz = (p.riders[0].z + p.riders[1].z) / 2.0f;
    if (o.moving) {
        const float fx = std::cos(o.heading), fz = std::sin(o.heading);
        p.spot_x = p.spot_z = 1e9f;
        for (int i = 0; i < 2; ++i) {
            Rider& r = p.riders[i];
            r.stopped = false;
            const float side = (i == 0 ? -0.5f : 0.5f) * kAbreast;
            const float slot_x = o.x - fx * 600.0f - fz * side, slot_z = o.z - fz * 600.0f + fx * side;
            const float ahead = (slot_x - r.x) * std::cos(r.heading) + (slot_z - r.z) * std::sin(r.heading);
            std::vector<Obstacle> near = obstacles[i];
            near.push_back({ o.x, o.z, 450.0f, true });
            steer(r, slot_x + fx * 300.0f, slot_z + fz * 300.0f, std::clamp(o.speed + 1.2f * ahead, 200.0f, 1300.0f), dt, near);
        }
        return;
    }
    const float from = std::hypot(p.spot_x - o.x, p.spot_z - o.z);
    if (from < 500.0f || from > 1300.0f) {
        std::tie(p.spot_x, p.spot_z) = open_spot_by(o.x, o.z, mx, mz, 850.0f);
        for (Rider& r : p.riders) r.stopped = false;
    }
    const float lx = o.x - p.spot_x, lz = o.z - p.spot_z, length = std::max(1.0f, std::hypot(lx, lz));
    for (int i = 0; i < 2; ++i) {
        Rider& r = p.riders[i];
        const float side = i == 0 ? -1.0f : 1.0f;
        const float sx = p.spot_x - lz / length * side * kAbreast / 2.0f, sz = p.spot_z + lx / length * side * kAbreast / 2.0f;
        const float left = std::hypot(sx - r.x, sz - r.z);
        if (!r.stopped && left < 150.0f) r.stopped = true;
        if (r.stopped) settle(r, o.x, o.z, dt);
        else steer(r, sx, sz, std::min(kPairSpeed, std::max(200.0f, left * 0.9f)), dt, obstacles[i]);
    }
}

// ---- more riders ---------------------------------------------------------
//
// With more riders out (more_riders_on): two more singles, each with a mind
// of its own as the first rider's (its routes and ramps, resting, riding
// along, watching), and the trio: a leader with a mind (laps, resting,
// riding along, watching -- no ramps) and two keeping a wedge behind it,
// kWedgeBack back and kWedgeSide either side, coming up alongside it when it
// stops and facing what it faces.
//
// Off by default away from Dolphin Park (WR64_MORE_RIDERS=0 none,
// =everywhere on every course).
bool more_riders_on(uint32_t course) {
    static const int setting = [] {
        const char* value = std::getenv("WR64_MORE_RIDERS");
        if (value == nullptr) return 1;
        return std::strcmp(value, "0") == 0 ? 0 : std::strcmp(value, "everywhere") == 0 ? 2 : 1;
    }();
    return setting == 2 || (setting == 1 && course == 0);
}

constexpr float kWedgeBack = 300.0f, kWedgeSide = 280.0f;
Rider g_wings[2];

// Wing `i`'s place in the wedge behind the leader `l` (+1 to the side its
// heading turns towards for the first), or beside it once it stops.
std::pair<float, float> wedge_place(const Drive& l, int i, bool stopped) {
    const float fx = std::cos(l.heading), fz = std::sin(l.heading);
    const float side = i == 0 ? 1.0f : -1.0f;
    const float back = stopped ? 60.0f : kWedgeBack;
    return { l.x - fx * back - fz * side * kWedgeSide, l.z - fz * back + fx * side * kWedgeSide };
}

void start_wings() {
    const Drive& l = g_drives[kTrioLeader];
    for (int i = 0; i < 2; ++i) {
        Rider& r = g_wings[i];
        r = Rider{};
        std::tie(r.x, r.z) = wedge_place(l, i, false);
        r.heading = l.heading;
        r.speed = l.speed;
    }
}

void ride_wings(uint32_t segment3, float dt, const std::vector<Obstacle>& others) {
    const Drive& l = g_drives[kTrioLeader];
    const bool stopped = l.free != Free::None && l.free != Free::Follow && l.parked && l.speed < 150.0f;
    const float fx = std::cos(l.heading), fz = std::sin(l.heading);
    for (int i = 0; i < 2; ++i) {
        Rider& r = g_wings[i];
        const auto [sx, sz] = wedge_place(l, i, stopped);
        std::vector<Obstacle> obstacles = fixed_obstacles();
        obstacles.insert(obstacles.end(), others.begin(), others.end());
        obstacles.push_back({ l.x, l.z, 200.0f, false });
        obstacles.push_back({ g_wings[1 - i].x, g_wings[1 - i].z, 180.0f, false });
        const float left = std::hypot(sx - r.x, sz - r.z);
        if (stopped && left < 120.0f) settle(r, l.look_x, l.look_z, dt);
        else if (stopped) steer(r, sx, sz, std::clamp(left * 0.9f, 120.0f, 700.0f), dt, obstacles);
        else {
            // Up to its place and on with the leader: faster when the place
            // is ahead of it, slower when behind.
            const float ahead = (sx - r.x) * std::cos(r.heading) + (sz - r.z) * std::sin(r.heading);
            steer(r, sx + fx * 300.0f, sz + fz * 300.0f, std::clamp(l.speed + 1.2f * ahead, 150.0f, 1250.0f), dt, obstacles);
        }
        r.y = water_height_at(segment3, r.x, r.z);
        r.climb = 0.0f;
    }
}

// What the pair keep clear of besides: the other singles and the trio.
std::vector<Obstacle> g_crowd;

// The pair's riding this frame: `player` and `first` where the player and
// the first rider are, if they are about.
void ride_pair(uint32_t segment3, std::chrono::steady_clock::time_point now, float dt,
               const Obstacle* player, const Obstacle* first) {
    Pair& p = g_pair;
    minds::feel(g_minds[1], seconds(now), dt);
    if (p.routine != PairRoutine::Trick && minds::restless(g_minds[1], seconds(now))) p.change = true;
    std::vector<Obstacle> obstacles[2];
    for (int i = 0; i < 2; ++i) {
        obstacles[i] = fixed_obstacles();
        if (player != nullptr) obstacles[i].push_back(*player);
        if (first != nullptr) obstacles[i].push_back(*first);
        obstacles[i].insert(obstacles[i].end(), g_crowd.begin(), g_crowd.end());
        const Rider& other = p.riders[1 - i];
        obstacles[i].push_back({ other.x, other.z, 180.0f, false });
    }
    if (p.routine == PairRoutine::Trick) {
        ride_trick(p, segment3, now, dt, obstacles, first);
        return;
    }
    if (p.routine == PairRoutine::Sights) ride_sights(p, now, dt, obstacles);
    else if (p.routine == PairRoutine::Dolphin) ride_dolphin(p, now, dt, obstacles);
    else if (p.routine == PairRoutine::Follow || p.routine == PairRoutine::Watch) ride_along(p, now, dt, obstacles);
    else ride_weave(p, now, dt, obstacles);
    for (Rider& r : p.riders) {
        r.y = water_height_at(segment3, r.x, r.z);
        r.climb = 0.0f;
    }
}

// The player this frame, from its rider (the human's slot, D_800D48DC; its
// place at +0x44): how fast and which way it goes, measured from frame to
// frame, and its height above the water. Its big air -- more than 160 units
// up -- is news (an Interest), which each mind may go and watch.
void note_player(const uint8_t* rdram, uint32_t segment3, float dt, std::chrono::steady_clock::time_point now) {
    const uint32_t slot = word_at(rdram, 0x0D48DC);
    if (slot > 3) return;
    const uint32_t rider = 0x192690 + slot * 0x1718;
    auto real = [&](uint32_t a) { float v; const uint32_t w = word_at(rdram, rider + a); std::memcpy(&v, &w, 4); return v; };
    const float x = real(0x44), y = real(0x48), z = real(0x4C);
    if (!std::isfinite(x) || !std::isfinite(z)) return;
    Player& pl = g_player;
    if (pl.known && dt > 0.0f) {
        const float moved = std::hypot(x - pl.x, z - pl.z);
        if (moved < 200.0f) {
            pl.speed += (moved / dt - pl.speed) * std::min(1.0f, 4.0f * dt);
            if (moved > 1.0f) pl.heading = std::atan2(z - pl.z, x - pl.x);
        }
    }
    pl.known = true;
    pl.x = x; pl.y = y; pl.z = z;
    pl.height = y - water_height_at(segment3, x, z);
    if (pl.height < 40.0f) pl.aloft = false;
    // WR64_COMPANION_TEST_AIR=<seconds>[,<x>,<z>], in a scripted run only:
    // the player's big air, as if, that long into the outing (and there).
    static float test_x = 0.0f, test_z = 0.0f;
    static int test_place = 0;
    static const float test_air = [] {
        const char* value = std::getenv("WR64_COMPANION_TEST_AIR");
        float t = -1.0f;
        if (value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr) test_place = std::sscanf(value, "%f,%f,%f", &t, &test_x, &test_z);
        return t;
    }();
    static std::chrono::steady_clock::time_point outing{};
    static bool tested = false;
    if (outing.time_since_epoch().count() == 0 || now - outing > std::chrono::hours(1)) outing = now;
    if (test_air > 0.0f && !tested && now - outing > std::chrono::duration<float>(test_air)) {
        tested = true;
        pl.aloft = false;
        pl.height = 220.0f;
    }
    if (pl.aloft || pl.height < 160.0f) return;
    pl.aloft = true;
    const double t = seconds(now);
    minds::Interest air;
    air.x = test_place == 3 && test_air > 0.0f ? test_x : x;
    air.z = test_place == 3 && test_air > 0.0f ? test_z : z;
    air.strength = std::clamp(pl.height / 250.0f, 0.5f, 1.0f); air.until = t + 15.0; air.what = "the player's big air";
    static const bool log = std::getenv("WR64_COMPANION_LOG") != nullptr;
    if (log) {
        std::fprintf(stderr, "[wr64-companion] the player is in the air, %.0f up at %.0f %.0f\n", pl.height, x, z);
        std::fflush(stderr);
    }
    for (int i = 0; i < (g_more_out ? kDrives : 1); ++i) {
        Drive& d = g_drives[i];
        if (!d.airborne && d.crash == Crash::None && !d.on_ramp &&
            minds::notice(g_minds[d.mind], air, std::hypot(air.x - d.x, air.z - d.z), chance(), t)) {
            begin_first(d, minds::Activity::Watch);
            log_mind(d.mind, "noticed the player's big air");
        }
    }
    Pair& p = g_pair;
    const float mx = (p.riders[0].x + p.riders[1].x) / 2.0f, mz = (p.riders[0].z + p.riders[1].z) / 2.0f;
    if (p.routine != PairRoutine::Trick && minds::notice(g_minds[1], air, std::hypot(air.x - mx, air.z - mz), chance(), t)) {
        p.change = false;
        p.parked = false;
        start_watch(p);
        log_mind(1, "noticed the player's big air");
    }
}

// ---- the dolphin ---------------------------------------------------------
//
// Dolphin Park's dolphin, out on the other courses (docs/DOLPHIN.md). Its
// models and animations are the park's animal bank -- an MIO0 chunk in the
// cartridge at kDolphinBank, 0xC21C bytes unpacked, which the game loads at
// segment 8 + 0x60000 there -- unpacked here into kDolphinModel, its own
// addresses being segment 8 counted from the bank's start. It is drawn as
// the game draws it (func_8009FCB0): the animals' setup list (0x800E6CA0),
// then its five parts, each on a matrix of its pose in this frame of the
// animation (func_801E4C60) times the dolphin's own, with a call of the
// part's list (the fourth part's list drawn twice, for both flippers).
//
// An animation (the bank's list of eight at kAnimations): its frame count,
// its poses (12 bytes each: the part's place, three halves; then the way its
// y points and a way across it, three signed bytes each, in 127ths), and
// for each part, the pose it takes in each frame. A part's matrix: y the
// first way, x across the second and y, z across y and x; the place last.
constexpr uint32_t kDolphinBank = 0x3E17A0, kDolphinBankSize = 0xC21C;
// Its area: after the first racer's model (kModelEnd), before its matrices
// (0x55E000) -- not at 0x5E0000, inside first person's water fog (0x5C0000
// to 0x640000), which going under in first person overwrote.
constexpr uint32_t kDolphinArea = 0x54A000, kDolphinModel = 0x54B000, kDolphinModelEnd = 0x558000;
constexpr uint32_t kDolphinMatrices = 0x558000, kDolphinAreaEnd = 0x559000;
constexpr uint32_t kDolphinMark = 0x444C5048u;     // "DLPH"
constexpr uint32_t kAnimations = 0xC1FC;
constexpr int kDolphinAnimations = 8;
struct DolphinPart { uint32_t frames_at, list; };
constexpr DolphinPart kDolphinParts[5] = {
    { 0x08, 0x2AB8 }, { 0x18, 0x3008 }, { 0x14, 0x31A8 }, { 0x0C, 0x2E60 }, { 0x10, 0x2E60 },
};
// Its size, as the game draws it (func_8009FCB0's usual 0.15), and the
// game's animals' frames a second.
constexpr float kDolphinScale = 0.15f, kDolphinFps = 30.0f;

bool g_dolphin_ready = false;

int16_t half_at(const uint8_t* rdram, uint32_t address) {
    int16_t v;
    std::memcpy(&v, rdram + ((address & 0x7FFFFF) ^ 2), 2);
    return v;
}
int8_t signed_byte_at(const uint8_t* rdram, uint32_t address) { return int8_t(byte_at(rdram, address)); }

// The bank unpacked and its parts' lists set to where it lies; whether
// they are all there.
bool load_dolphin(uint8_t* rdram) {
    if (!claim(rdram, kDolphinArea, kDolphinAreaEnd, kDolphinMark)) return false;
    const uint32_t size = unpack_mio0(rdram, kDolphinBank, kDolphinModel, kDolphinModelEnd - kDolphinModel);
    if (size != kDolphinBankSize) return false;
    for (const DolphinPart& p : kDolphinParts) {
        if (word_at(rdram, kDolphinModel + p.list) != 0xBB000001u) return false;
    }
    for (const uint32_t list : { 0x2AB8u, 0x3008u, 0x31A8u, 0x2E60u }) relocate(rdram, kDolphinModel, list, size);
    return true;
}

// Animation `animation`'s frame count.
int dolphin_frames(const uint8_t* rdram, int animation) {
    const uint32_t a = kDolphinModel + (word_at(rdram, kDolphinModel + kAnimations + uint32_t(animation) * 4) & 0xFFFFFF);
    return std::max<int>(1, half_at(rdram, a));
}

// Part `part`'s matrix in frame `frame` of animation `animation`.
Matrix dolphin_part(const uint8_t* rdram, int animation, int frame, int part) {
    auto at = [&](uint32_t address) { return kDolphinModel + (word_at(rdram, address) & 0xFFFFFF); };
    const uint32_t a = at(kDolphinModel + kAnimations + uint32_t(animation) * 4);
    const uint32_t poses = at(a + 4), frames = at(a + kDolphinParts[part].frames_at);
    const uint32_t p = poses + uint32_t(half_at(rdram, frames + uint32_t(frame) * 2)) * 12;
    float y[3], across[3];
    for (int i = 0; i < 3; ++i) {
        y[i] = float(signed_byte_at(rdram, p + 6 + uint32_t(i))) / 127.0f;
        across[i] = float(signed_byte_at(rdram, p + 9 + uint32_t(i))) / 127.0f;
    }
    auto cross = [](const float a[3], const float b[3], float out[3]) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
        const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        if (length > 1e-6f) for (int i = 0; i < 3; ++i) out[i] /= length;
    };
    float x[3], z[3];
    cross(across, y, x);
    cross(y, x, z);
    Matrix m{};
    for (int i = 0; i < 3; ++i) { m.m[0][i] = x[i]; m.m[1][i] = y[i]; m.m[2][i] = z[i]; }
    m.m[3][0] = half_at(rdram, p);
    m.m[3][1] = half_at(rdram, p + 2);
    m.m[3][2] = half_at(rdram, p + 4);
    m.m[3][3] = 1.0f;
    return m;
}

// The dolphin's own matrix: at (x, y, z), its nose along (hx, hz), pitched
// up by `pitch`, at the game's size.
Matrix dolphin_matrix(float x, float y, float z, float hx, float hz, float pitch) {
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float f[3] = { hx * cp, sp, hz * cp };
    const float u[3] = { -hx * sp, cp, -hz * sp };
    const float r[3] = { u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0] };
    Matrix m{};
    for (int i = 0; i < 3; ++i) {
        m.m[0][i] = r[i] * kDolphinScale;
        m.m[1][i] = u[i] * kDolphinScale;
        m.m[2][i] = f[i] * kDolphinScale;
    }
    m.m[3][0] = x; m.m[3][1] = y; m.m[3][2] = z; m.m[3][3] = 1.0f;
    return m;
}

// How deep the water must be for the dolphin.
constexpr float kDolphinDepth = 150.0f;

// Whether (x, z) is water the dolphin may swim in: inside the course's
// push-back boundary -- the game's rectangles, which D_801C1F80 points to:
// four floats each, x from and to, z from and to, ended by one whose x from
// and to are the same (func_80074264) -- and at least kDolphinDepth deep by
// the course's sea floor (src/seafloor.cpp), where it has one.
bool inside_boundary(const uint8_t* rdram, float x, float z) {
    auto real_at = [&](uint32_t a) { float v; const uint32_t w = word_at(rdram, a); std::memcpy(&v, &w, 4); return v; };
    const uint32_t rects = word_at(rdram, 0x1C1F80);
    if ((rects >> 24) != 0x80) return false;
    for (uint32_t r = rects & 0xFFFFFF, i = 0; i < 64; ++i, r += 16) {
        const float x0 = real_at(r), x1 = real_at(r + 4), z0 = real_at(r + 8), z1 = real_at(r + 12);
        if (x0 == x1) break;
        if (x >= x0 && x <= x1 && z >= z0 && z <= z1) return true;
    }
    return false;
}

bool dolphin_water(const uint8_t* rdram, uint32_t course, float x, float z) {
    if (!inside_boundary(rdram, x, z)) return false;
    float floor = 0.0f;
    return !seafloor_height_at(course, x, z, &floor) || floor < -kDolphinDepth;
}

// And a jet ski's: water enough to float in, by the course's sea floor
// where it has one -- out past the boundary too, as a free roam goes (the
// user's sights include beaches far out) -- or else inside the boundary.
bool rider_water(const uint8_t* rdram, uint32_t course, float x, float z) {
    float floor = 0.0f;
    if (seafloor_height_at(course, x, z, &floor)) return floor < -25.0f;
    return inside_boundary(rdram, x, z);
}

// Draws the dolphin on `body`, in frame `frame` of animation `animation`,
// into frame buffer `b`'s matrices.
void draw_dolphin(uint8_t* rdram, int b, const Matrix& body, int animation, int frame,
                  std::vector<std::pair<uint32_t, uint32_t>>& out) {
    out.emplace_back(0x06000000u, 0x800E6CA0u);
    for (int p = 0; p < 5; ++p) {
        const uint32_t address = kDolphinMatrices + uint32_t(b) * 0x200 + uint32_t(p) * 64;
        write_matrix(rdram, address, multiply(dolphin_part(rdram, animation, frame, p), body));
        out.emplace_back(0x01020040u, 0x80000000u | address);
        out.emplace_back(0x06000000u, 0x80000000u | (kDolphinModel + kDolphinParts[p].list));
    }
}

// ---- the dolphin's mind ----------------------------------------------------
//
// It wants people's attention -- the player's, or a rider's (docs/DOLPHIN.md).
//
// Roaming, it picks someone it hasn't just given up on and seeks them out:
// to a stage in front of them -- ahead of them, if they are on the move --
// and there it performs, one act after another, each different from the
// last: a leap, its dance, its call. Nobody biting within 12 to 30 seconds,
// it gives up on them (for a while) and seeks someone else.
//
// Once they pay attention -- a rider taken with it (its mind's Dolphin), or
// the player slowed and facing it, or riding along behind it -- it
// entertains them: its interest in them, 30 to 60 seconds, runs down only
// while they pay attention; each new act may please a rider into watching
// longer (minds::dolphin_act), and buys a watching player 10 seconds more of
// the dolphin's (up to a minute of interest in all, the first 30 to 60
// seconds included). Bored, it swims off; a rider still
// taken with it follows it for 3 to 10 seconds, then gives up. If they lose
// interest first, it chases them for 3 to 10 seconds, still performing; won
// back, they start afresh, and if not it gives up on them. (Lost interest:
// not paying attention for 4 seconds -- an act can take it out of sight.)
//
// Whoever it gave up on, or got bored of, is left alone for a minute.
//
// Its animations (the bank's eight): 0 and 3 the swim, 1 a leap, 2 a high
// leap, 5 the dance; 4, 6 and 7 tricks of 85 frames.
enum class DolphinAct { Swim, Leap, Dance, Call };
enum class DolphinPhase { Roam, Seek, Entertain, Chase, Leave };
constexpr int kSwimAnimation = 0, kLeapAnimation = 1, kHighLeapAnimation = 2, kDanceAnimation = 5, kCallAnimation = 4;
constexpr float kDolphinSwim = 750.0f, kDolphinLeap = 900.0f, kDolphinTurn = 1.6f;
// The people it may seek: the player, and the first rider.
constexpr int kAudience = 3;

struct Dolphin {
    float x = 0, y = 0, z = 0, heading = 0, speed = 0;
    DolphinPhase phase = DolphinPhase::Roam;
    DolphinAct act = DolphinAct::Swim;
    int animation = kSwimAnimation;
    double act_since = 0, act_until = 0;
    int target = -1;
    double phase_until = 0;       // Roam: when it next looks for someone; Seek, Chase: when it gives up; Leave: when it's away
    bool arrived = false;         // Seek: come near its stage before them (its giving-up clock starts then)
    // Its try at someone, for the decisions journal: when it set off for
    // them, when it first won their attention, how often it won it, its acts
    // and those enjoyed, and the seconds they paid attention.
    double episode_since = 0, won_at = -1, attended_for = 0;
    int wins = 0, acts = 0, enjoyed = 0;
    double interest = 0;          // Entertain: its interest left, in seconds
    double bonus = 0;             // and what more a watching player can still win it (up to a minute in all)
    double unattended = 0;        // seconds the one it's with hasn't paid attention
    double shunned_until[kAudience] = {};
    float goal_x = 0, goal_z = 0;
    float stage_x = 0, stage_z = 0;   // the last stage it found in open water
    bool staged = false;
    bool placed = false;
} g_dolphin;

const char* phase_name(DolphinPhase p) {
    switch (p) {
    case DolphinPhase::Roam: return "roaming";
    case DolphinPhase::Seek: return "seeking";
    case DolphinPhase::Entertain: return "entertaining";
    case DolphinPhase::Chase: return "chasing";
    case DolphinPhase::Leave: return "leaving";
    }
    return "?";
}
const char* audience_name(int t) { return t == 0 ? "the player" : t == 1 ? "the rider" : t == 2 ? "the pair" : "nobody"; }

void log_dolphin(const char* what) {
    static const bool log = std::getenv("WR64_DOLPHIN_LOG") != nullptr;
    if (!log) return;
    const Dolphin& o = g_dolphin;
    std::fprintf(stderr, "[wr64-dolphin] %s: %s %s at %.0f %.0f\n", what, phase_name(o.phase), audience_name(o.target), o.x, o.z);
    std::fflush(stderr);
}

// Where audience member `t` is, which way it faces and how fast it goes;
// whether it is about.
bool audience_place(int t, float& x, float& z, float& fx, float& fz, float& speed) {
    if (t == 0) {
        if (!g_player.known || !g_pose.ready) return false;
        x = g_player.x; z = g_player.z; speed = g_player.speed;
        // Which way the jet ski points (its hull's z), not just where it went.
        const float (*h)[4] = g_pose.hull.m;
        const float length = std::max(1e-3f, std::hypot(h[2][0], h[2][2]));
        fx = h[2][0] / length; fz = h[2][2] / length;
        return true;
    }
    if (t == 1) {
        if (!g_racers[0].ready || g_course == 0) return false;
        const Drive& d = g_drive;
        x = d.x; z = d.z; speed = d.speed; fx = std::cos(d.heading); fz = std::sin(d.heading);
        return d.crash == Crash::None;
    }
    if (t == 2) {
        if (!g_pair_out || g_course == 0) return false;
        const Pair& p = g_pair;
        x = (p.riders[0].x + p.riders[1].x) / 2.0f; z = (p.riders[0].z + p.riders[1].z) / 2.0f;
        speed = (p.riders[0].speed + p.riders[1].speed) / 2.0f;
        fx = std::cos(p.riders[0].heading); fz = std::sin(p.riders[0].heading);
        return p.routine != PairRoutine::Trick && p.riders[0].crash == Crash::None && p.riders[1].crash == Crash::None;
    }
    return false;
}

// Whether audience member `t` is paying the dolphin attention.
bool attending(int t) {
    const Dolphin& o = g_dolphin;
    if (t == 1 || t == 2) return g_minds[t - 1].doing == minds::Activity::Dolphin;
    float x, z, fx, fz, speed;
    if (!audience_place(t, x, z, fx, fz, speed)) return false;
    const float dx = o.x - x, dz = o.z - z, distance = std::hypot(dx, dz);
    if (distance > 2000.0f || distance < 1.0f) return false;
    const float facing = (dx * fx + dz * fz) / distance;
    // Watching: slowed almost to a stop, facing it (within 60 degrees).
    if (speed < 300.0f) return facing > 0.5f;
    // Following: behind it, going its way.
    return facing > 0.7f && std::cos(o.heading) * fx + std::sin(o.heading) * fz > 0.5f;
}

// Starts act `act` (its animation, for as long as one round of it lasts;
// the swim for 3 to 6 seconds) -- and, if it is a performance and the one it
// is with is a rider, offers it to the rider's mind.
void start_act(const uint8_t* rdram, DolphinAct act, double now) {
    Dolphin& o = g_dolphin;
    o.act = act;
    o.animation = act == DolphinAct::Leap ? (chance()() % 3u == 0 ? kHighLeapAnimation : kLeapAnimation)
                : act == DolphinAct::Dance ? kDanceAnimation : act == DolphinAct::Call ? kCallAnimation : kSwimAnimation;
    o.act_since = now;
    o.act_until = act == DolphinAct::Swim ? now + uniform(3.0f, 6.0f) : now + dolphin_frames(rdram, o.animation) / kDolphinFps;
    if (act == DolphinAct::Swim || o.target < 0) return;
    float x, z, fx, fz, speed;
    if (!audience_place(o.target, x, z, fx, fz, speed)) return;
    const float distance = std::hypot(o.x - x, o.z - z);
    static const char* acts[] = { "swim", "leap", "dance", "call" };
    ++o.acts;
    journal::Fields f;
    f.add("act", acts[int(act)]).add("animation", o.animation).add("phase", phase_name(o.phase)).add("for", audience_name(o.target));
    f.add("distance", double(distance)).add("attending", attending(o.target));
    if (o.target == 1 || o.target == 2) {
        const int which = o.target - 1;
        Drive& d = g_drive;
        minds::Mind& m = g_minds[which];
        const bool was = m.doing == minds::Activity::Dolphin;
        const double until = m.until;
        const bool took = minds::dolphin_act(m, distance, chance(), now);
        // Taken with it: when the roll falls under how much it likes the
        // dolphin (its personality's liking, by its curiosity now).
        f.add("rider_likes", double(m.pull)).add("roll", double(m.roll));
        f.add("response", took ? (was ? "enjoyed it" : "taken with it") : was ? "unmoved" : "ignored it");
        if (took) f.add("rider_interest_added", m.until - std::max(until, now));
        if (took && was) ++o.enjoyed;
        if (took) {
            if (!was && which == 0 && !d.airborne && !d.on_ramp && d.crash == Crash::None) begin_first(d, minds::Activity::Dolphin);
            if (!was && which == 1) start_pair_dolphin(g_pair);
            log_mind(which, "the dolphin");
        }
    }
    else if (o.phase == DolphinPhase::Entertain && attending(0)) {
        const double more = std::min(10.0, o.bonus);
        o.interest += more;
        o.bonus -= more;
        if (more > 0) ++o.enjoyed;
        f.add("interest_added", more);
    }
    journal_context(f, 0);
    journal::write("dolphin_act", f);
}

// The next act: on the move, a leap or a stretch of swimming; stopped,
// any performance but the last one.
void next_act(const uint8_t* rdram, bool moving, double now) {
    const DolphinAct last = g_dolphin.act;
    DolphinAct act;
    if (moving) act = last == DolphinAct::Leap ? DolphinAct::Swim : (chance()() % 2u ? DolphinAct::Leap : DolphinAct::Swim);
    else {
        const DolphinAct choices[3] = { DolphinAct::Leap, DolphinAct::Dance, DolphinAct::Call };
        do act = choices[chance()() % 3u]; while (act == last);
    }
    start_act(rdram, act, now);
}

// Picks someone to seek: anyone about it hasn't just given up on, the nearer
// the likelier.
int pick_audience(double now) {
    const Dolphin& o = g_dolphin;
    float best = -1.0f;
    int chosen = -1;
    for (int t = 0; t < kAudience; ++t) {
        float x, z, fx, fz, speed;
        if (now < o.shunned_until[t] || !audience_place(t, x, z, fx, fz, speed)) continue;
        const float score = uniform(0.0f, 1.0f) / (1.0f + std::hypot(x - o.x, z - o.z) / 4000.0f);
        if (score > best) { best = score; chosen = t; }
    }
    return chosen;
}

void set_phase(DolphinPhase phase, int target, double until, const char* why) {
    Dolphin& o = g_dolphin;
    const double now = seconds(std::chrono::steady_clock::now());
    const DolphinPhase was = o.phase;
    const int had = o.target;
    const bool trying = was == DolphinPhase::Seek || was == DolphinPhase::Entertain || was == DolphinPhase::Chase;
    if (trying && (phase == DolphinPhase::Roam || phase == DolphinPhase::Leave) && had >= 0) {
        // How the try went.
        journal::Fields f;
        f.add("with", audience_name(had));
        if ((had == 1 || had == 2) && g_minds[had - 1].who != nullptr) f.add("personality", g_minds[had - 1].who->name);
        f.add("ended", why).add("lasted", now - o.episode_since);
        f.add("won", o.wins > 0).add("wins", o.wins);
        if (o.won_at >= 0) f.add("to_attention", o.won_at - o.episode_since);
        f.add("attention", o.attended_for).add("acts", o.acts).add("enjoyed", o.enjoyed);
        journal_context(f, 0);
        journal::write("dolphin_try", f);
    }
    if (phase == DolphinPhase::Seek) {
        o.staged = false;
        o.episode_since = now;
        o.won_at = -1;
        o.attended_for = 0;
        o.wins = o.acts = o.enjoyed = 0;
    }
    if (phase == DolphinPhase::Entertain) {
        if (o.won_at < 0) o.won_at = now;
        ++o.wins;
    }
    o.phase = phase;
    o.target = target;
    o.phase_until = until;
    o.unattended = 0;
    o.arrived = false;
    log_dolphin(why);
    journal::Fields f;
    f.add("why", why).add("phase", phase_name(phase)).add("with", audience_name(target));
    if (phase == DolphinPhase::Entertain) f.add("interest", o.interest).add("bonus_left", o.bonus);
    if (until > now) f.add("gives_up_in", until - now);
    journal_context(f, 0);
    journal::write("dolphin", f);
}

// The course's open water, as the dolphin finds its way through it: squares
// of kCell across the push-back boundary's extent, each open if its middle
// is water the dolphin may swim in (built once a course); and its route,
// the squares' middles from where it is to where it heads, found by a
// breadth-first search (eight ways round) -- to the open square nearest
// its goal, if the goal's own isn't open or can't be reached.
constexpr float kCell = 200.0f;
struct WaterMap {
    uint32_t course = ~0u;
    float x0 = 0, z0 = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> open;
} g_water_map;
std::vector<std::pair<float, float>> g_route;
float g_route_x = 1e9f, g_route_z = 1e9f;
double g_route_at = -1e9;

// Squares open where `open` says, over the boundary's extent and `beyond` it.
void build_map(const uint8_t* rdram, WaterMap& m, bool (*open)(const uint8_t*, uint32_t, float, float), float beyond = 0.0f) {
    m = WaterMap{};
    m.course = g_course;
    auto real_at = [&](uint32_t a) { float v; const uint32_t w = word_at(rdram, a); std::memcpy(&v, &w, 4); return v; };
    const uint32_t rects = word_at(rdram, 0x1C1F80);
    if ((rects >> 24) != 0x80) return;
    float x1 = -1e9f, z1 = -1e9f;
    m.x0 = m.z0 = 1e9f;
    for (uint32_t r = rects & 0xFFFFFF, i = 0; i < 64; ++i, r += 16) {
        const float a = real_at(r), b = real_at(r + 4);
        if (a == b) break;
        m.x0 = std::min(m.x0, a); x1 = std::max(x1, b);
        m.z0 = std::min(m.z0, real_at(r + 8)); z1 = std::max(z1, real_at(r + 12));
    }
    if (x1 <= m.x0 || z1 <= m.z0) return;
    m.x0 -= beyond; m.z0 -= beyond; x1 += beyond; z1 += beyond;
    m.w = std::min(400, int((x1 - m.x0) / kCell) + 1);
    m.h = std::min(400, int((z1 - m.z0) / kCell) + 1);
    m.open.assign(size_t(m.w) * size_t(m.h), 0);
    for (int j = 0; j < m.h; ++j)
        for (int i = 0; i < m.w; ++i)
            m.open[size_t(j) * size_t(m.w) + size_t(i)] = open(rdram, g_course, m.x0 + (float(i) + 0.5f) * kCell, m.z0 + (float(j) + 0.5f) * kCell);
}
void build_water_map(const uint8_t* rdram) { build_map(rdram, g_water_map, dolphin_water); }

void plan_route(const WaterMap& m, float fx, float fz, float tx, float tz, std::vector<std::pair<float, float>>& route) {
    route.clear();
    if (m.w == 0) return;
    auto cell = [&](float x, float z) {
        return std::pair<int, int>{ std::clamp(int((x - m.x0) / kCell), 0, m.w - 1), std::clamp(int((z - m.z0) / kCell), 0, m.h - 1) };
    };
    auto [si, sj] = cell(fx, fz);
    const auto [gi, gj] = cell(tx, tz);
    // Starting in a closed square (a rider in the shallows), from the
    // nearest open one, within ten squares: else it could go nowhere, and
    // headed straight for its goal over the land.
    if (!m.open[size_t(sj * m.w + si)]) {
        float nearest = 1e9f;
        int ni = si, nj = sj;
        for (int dj = -10; dj <= 10; ++dj)
            for (int di = -10; di <= 10; ++di) {
                const int i = si + di, j = sj + dj;
                if (i < 0 || j < 0 || i >= m.w || j >= m.h || !m.open[size_t(j * m.w + i)]) continue;
                const float d = std::hypot(float(di), float(dj));
                if (d < nearest) { nearest = d; ni = i; nj = j; }
            }
        si = ni; sj = nj;
    }
    std::vector<int> from(m.open.size(), -1);
    std::vector<int> queue{ sj * m.w + si };
    from[size_t(queue[0])] = queue[0];
    int best = queue[0];
    float best_distance = std::hypot(float(si - gi), float(sj - gj));
    for (size_t q = 0; q < queue.size(); ++q) {
        const int c = queue[q], ci = c % m.w, cj = c / m.w;
        if (ci == gi && cj == gj) { best = c; break; }
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                const int ni = ci + di, nj = cj + dj;
                if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= m.w || nj >= m.h) continue;
                const int n = nj * m.w + ni;
                if (from[size_t(n)] >= 0 || !m.open[size_t(n)]) continue;
                // Not across a corner of land.
                if (di != 0 && dj != 0 && (!m.open[size_t(cj * m.w + ni)] || !m.open[size_t(nj * m.w + ci)])) continue;
                from[size_t(n)] = c;
                queue.push_back(n);
                const float d = std::hypot(float(ni - gi), float(nj - gj));
                if (d < best_distance) { best_distance = d; best = n; }
            }
    }
    for (int c = best; ; c = from[size_t(c)]) {
        route.emplace_back(m.x0 + (float(c % m.w) + 0.5f) * kCell, m.z0 + (float(c / m.w) + 0.5f) * kCell);
        if (from[size_t(c)] == c) break;
    }
    std::reverse(route.begin(), route.end());
}

// The riders' map, away from the park: water enough to float in
// (rider_water), and clear of the course's ramps (kind 1 in the objects,
// D_801BC940: 0xC4 bytes each, counted at D_801BFA40; x +0x24, z +0x2C) by
// kRampRoom. Without it they rode straight over Sunny Beach's sandbar and
// through its ramp.
WaterMap g_rider_map;
std::vector<std::pair<float, float>> g_ramps;
std::vector<float> g_ramp_angles;
std::vector<RampShape> g_ramp_shapes;

// The ramps' slope pieces (patches/ramp_pieces.cpp), as the game meets the
// jet ski with them (func_8007FAE4): each a solid box from a corner `o`
// back along three axes, a point inside when (o - p) . axis[i] is in
// [0, length[i]) for each.
struct SlopePiece { float o[3], axis[3][3], length[3]; };
std::vector<SlopePiece> g_slope_pieces;


void read_slope_pieces(const uint8_t* rdram) {
    g_slope_pieces.clear();
    const uint32_t base = uint32_t(wr64_ramp_pieces()) & 0x1FFFFFFFu;
    const uint32_t count = std::min<uint32_t>(word_at(rdram, 0x1C0578), 256);
    for (uint32_t i = 0; i < count; ++i) {
        float v[15];
        for (int k = 0; k < 15; ++k) {
            const uint32_t w = *reinterpret_cast<const uint32_t*>(rdram + base + i * 0x40 + k * 4);
            std::memcpy(&v[k], &w, 4);
        }
        SlopePiece p{};
        for (int k = 0; k < 3; ++k) {
            p.o[k] = v[k];
            p.length[k] = v[12 + k];
            for (int j = 0; j < 3; ++j) p.axis[k][j] = v[3 + k * 3 + j];
        }
        g_slope_pieces.push_back(p);
    }
}

float ramp_surface(float x, float z, float floor) {
    float best = floor;
    for (const SlopePiece& p : g_slope_pieces) {
        // Each axis leaves a range of heights inside; the top of where all
        // three meet.
        float low = -1e9f, high = 1e9f;
        bool inside = true;
        for (int k = 0; k < 3 && inside; ++k) {
            const float* a = p.axis[k];
            const float c = (p.o[0] - x) * a[0] + p.o[1] * a[1] + (p.o[2] - z) * a[2];
            if (std::fabs(a[1]) < 1e-6f) { inside = c >= 0.0f && c < p.length[k]; continue; }
            const float y0 = c / a[1], y1 = (c - p.length[k]) / a[1];
            low = std::max(low, std::min(y0, y1));
            high = std::min(high, std::max(y0, y1));
        }
        if (inside && low < high) best = std::max(best, high);
    }
    return best;
}

// Each ramp's crest and face along the line through its middle, the way it
// is ridden, from the pieces. Off the crest a jet ski keeps on at the angle
// of the face's last climb (Sunny Beach's: 0.56 up for 1 along), and at
// the speed that brings it down kReach past the crest: room for a roll,
// and short of Sunny Beach's mines.
void measure_ramps() {
    g_ramp_shapes.clear();
    for (size_t k = 0; k < g_ramps.size(); ++k) {
        const auto [rx, rz] = g_ramps[k];
        const float a = g_ramp_angles[k] * 0.0174533f, ox = std::sin(a), oz = std::cos(a);
        float best_s = 0.0f, top = -1e9f;
        for (float s = -1500.0f; s <= 1500.0f; s += 5.0f) {
            const float h = ramp_surface(rx + ox * s, rz + oz * s, -1e9f);
            if (h > top) { top = h; best_s = s; }
        }
        RampShape shape{ rx + ox * best_s, rz + oz * best_s, kRampTop, kRampCrest - kRampStart, kPop, kSpeed };
        if (top > 0.0f) {
            float s = best_s;
            while (s > best_s - 1500.0f && ramp_surface(rx + ox * (s - 10.0f), rz + oz * (s - 10.0f), -1e9f) > 0.0f) s -= 10.0f;
            constexpr float kReach = 1300.0f;
            const float before = ramp_surface(rx + ox * (best_s - 20.0f), rz + oz * (best_s - 20.0f), -1e9f);
            const float slope = std::clamp((top - before) / 20.0f, 0.1f, 1.0f);
            // Down at kReach: top + slope * reach = gravity/2 * t^2.
            const float t = std::sqrt((top + slope * kReach) / (0.5f * kGravity));
            shape.top = top;
            shape.face = best_s - s;
            shape.launch = std::clamp(kReach / t, 500.0f, kSpeed);
            shape.pop = slope * shape.launch;
        }
        g_ramp_shapes.push_back(shape);
        std::fprintf(stderr, "[wr64-companion] ramp %zu: crest at %.0f %.0f, %.0f up, its face %.0f long, left at %.0f with a pop of %.0f\n", k,
                     shape.crest_x, shape.crest_z, shape.top, shape.face, shape.launch, shape.pop);
    }
}
// (With a margin of 150 all round, so routes keep off the beaches' edges.)
bool rider_open(const uint8_t* rdram, uint32_t course, float x, float z) {
    for (const auto& [rx, rz] : g_ramps) {
        if (std::hypot(x - rx, z - rz) < kRampRoom) return false;
    }
    for (const auto& [mx, mz] : g_mines) {
        if (std::hypot(x - mx, z - mz) < kMineRoom) return false;
    }
    // (Nor on a ramp's pieces, whose corners reach out past kRampRoom.)
    for (const float dx : { -150.0f, 0.0f, 150.0f }) {
        for (const float dz : { -150.0f, 0.0f, 150.0f }) {
            if (ramp_surface(x + dx, z + dz, -1e9f) > -1e8f) return false;
        }
    }
    return rider_water(rdram, course, x, z) && rider_water(rdram, course, x + 150.0f, z) && rider_water(rdram, course, x - 150.0f, z) &&
           rider_water(rdram, course, x, z + 150.0f) && rider_water(rdram, course, x, z - 150.0f);
}
// Where a rider may be at all, away from the park: water to float in, clear
// of the ramps' decks (no margin: sights by the shore are reached).
WaterMap g_wet_map;
bool rider_wet(const uint8_t* rdram, uint32_t course, float x, float z) {
    for (const auto& [rx, rz] : g_ramps) {
        if (std::hypot(x - rx, z - rz) < 500.0f) return false;
    }
    for (const auto& [mx, mz] : g_mines) {
        if (std::hypot(x - mx, z - mz) < 120.0f) return false;
    }
    if (ramp_surface(x, z, -1e9f) > -1e8f) return false;
    return rider_water(rdram, course, x, z);
}

void build_rider_map(const uint8_t* rdram) {
    g_ramps.clear();
    g_ramp_angles.clear();
    g_ramp_obstacles.clear();
    const uint32_t count = std::min<uint32_t>(word_at(rdram, 0x1BFA40), 64);
    if (std::getenv("WR64_OBJECT_DUMP")) {
        auto f = [&](uint32_t a) { float v; const uint32_t w = word_at(rdram, a); std::memcpy(&v, &w, 4); return v; };
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t o = 0x1BC940 + i * 0xC4;
            std::fprintf(stderr, "[wr64-dump] object %u kind %u at (%.0f, %.0f, %.0f) angle %.0f\n", i, word_at(rdram, o + 0x4C), f(o + 0x24),
                         f(o + 0x28), f(o + 0x2C), f(o + 0x48));
            std::fprintf(stderr, "[wr64-dump] object %u words:", i);
            for (uint32_t k = 0; k < 0xC4; k += 4) std::fprintf(stderr, " %08X", word_at(rdram, o + k));
            std::fprintf(stderr, "\n");
        }
        // The ramps' slope pieces (patches/ramp_pieces.cpp), 64 bytes each.
        const uint32_t pieces = std::min<uint32_t>(word_at(rdram, 0x1C0578), 256);
        auto high = [&](uint32_t a) { return *reinterpret_cast<const uint32_t*>(rdram + a); };
        for (uint32_t i = 0; i < pieces; ++i) {
            const uint32_t o = 0x800000 + i * 0x40;
            std::fprintf(stderr, "[wr64-dump] piece %u:", i);
            for (uint32_t k = 0; k < 0x40; k += 4) { float v; const uint32_t w = high(o + k); std::memcpy(&v, &w, 4); std::fprintf(stderr, " %.2f", v); }
            std::fprintf(stderr, "  |");
            for (uint32_t k = 0; k < 0x40; k += 4) std::fprintf(stderr, " %08X", high(o + k));
            std::fprintf(stderr, "\n");
        }
        // Anything placed near the mines (Sunny Beach, west of the ramp).
        for (uint32_t a = 0x100000; a + 12 < 0x400000; a += 4) {
            const float x = f(a), y = f(a + 4), z = f(a + 8);
            if (x > -4500.0f && x < -2700.0f && y > -60.0f && y < 80.0f && z > -1700.0f && z < -600.0f)
                std::fprintf(stderr, "[wr64-dump] near the mines: 0x%06X (%.0f, %.0f, %.0f)\n", a, x, y, z);
        }
        const uint32_t buoys = std::min<uint32_t>(word_at(rdram, 0x1BB120), 128);
        for (uint32_t i = 0; i < buoys; ++i) {
            const uint32_t o = 0x1AEE20 + i * 0x104;
            std::fprintf(stderr, "[wr64-dump] buoy %u:", i);
            for (uint32_t k = 0; k < 0x104; k += 4) std::fprintf(stderr, " %08X", word_at(rdram, o + k));
            std::fprintf(stderr, "\n");
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t o = 0x1BC940 + i * 0xC4;
        if (word_at(rdram, o + 0x4C) != 1) continue;
        float x, z;
        const uint32_t wx = word_at(rdram, o + 0x24), wz = word_at(rdram, o + 0x2C);
        std::memcpy(&x, &wx, 4);
        std::memcpy(&z, &wz, 4);
        g_ramps.emplace_back(x, z);
        float angle;
        const uint32_t wa = word_at(rdram, o + 0x48);
        std::memcpy(&angle, &wa, 4);
        g_ramp_angles.push_back(angle);
        g_ramp_obstacles.push_back({ x, z, kRampRoom, true });
    }
    read_slope_pieces(rdram);
    measure_ramps();
    // The mines: entries while they look like places in the water.
    g_mines.clear();
    for (uint32_t i = 0; i < 128; ++i) {
        const uint32_t o = kMines + i * kMineSize;
        float x, y, z;
        const uint32_t wx = word_at(rdram, o), wy = word_at(rdram, o + 4), wz = word_at(rdram, o + 8);
        std::memcpy(&x, &wx, 4); std::memcpy(&y, &wy, 4); std::memcpy(&z, &wz, 4);
        if (!(std::fabs(x) < 30000.0f && std::fabs(z) < 30000.0f && std::fabs(y) < 100.0f) || (wx == 0 && wz == 0)) break;
        if (word_at(rdram, o + 0x10) != kMineKind) continue;
        g_mines.emplace_back(x, z);
        g_ramp_obstacles.push_back({ x, z, kMineRoom, true });
    }
    build_map(rdram, g_rider_map, rider_open, 8000.0f);
    build_map(rdram, g_wet_map, rider_wet, 8000.0f);
    std::fprintf(stderr, "[wr64-companion] course %u: the riders' map, %d by %d squares, %zu ramps, %zu mines\n", g_course,
                 g_rider_map.w, g_rider_map.h, g_ramps.size(), g_mines.size());
    for (const auto& [mx, mz] : g_mines) std::fprintf(stderr, "[wr64-companion] a mine at %.0f %.0f\n", mx, mz);
    std::fflush(stderr);
}

bool map_open(const WaterMap& m, float x, float z) {
    const int i = int(std::floor((x - m.x0) / kCell)), j = int(std::floor((z - m.z0) / kCell));
    return i >= 0 && j >= 0 && i < m.w && j < m.h && m.open[size_t(j) * size_t(m.w) + size_t(i)];
}

void step_on(float& x, float& z, float heading, float& speed, float dt) {
    // Brought almost to a stop and still heading in, it creeps on: the shore
    // tables are rough in places (Twilight City's, Southern Island's put
    // stretches of the buoys' line on land), and a rider held there for good
    // looks worse than one nosing over a corner of sand.
    const float nx = x + std::cos(heading) * speed * dt, nz = z + std::sin(heading) * speed * dt;
    if (g_course == 0 || g_wet_map.w == 0 || map_open(g_wet_map, nx, nz) || !map_open(g_wet_map, x, z) || speed < 120.0f) {
        x = nx;
        z = nz;
    }
    else speed *= 0.5f;
}
// Whether the water from (ax, az) to (bx, bz) is open, but for the first
// 150 units (it may be leaving a closed square) and the last 250 (a spot by
// the shore may be in one).
bool open_line(const WaterMap& m, float ax, float az, float bx, float bz) {
    const float length = std::hypot(bx - ax, bz - az);
    for (float d = 150.0f; d < length - 250.0f; d += 100.0f) {
        if (!map_open(m, ax + (bx - ax) * d / length, az + (bz - az) * d / length)) return false;
    }
    return true;
}

std::pair<float, float> open_spot_by(float x, float z, float from_x, float from_z, float distance) {
    const float a = std::atan2(from_z - z, from_x - x);
    for (int i = 0; i <= 16; ++i) {
        const float turn = a + float(i % 2 ? (i + 1) / 2 : -(i / 2)) * 0.3927f;   // 22.5 degrees a step
        const float sx = x + std::cos(turn) * distance, sz = z + std::sin(turn) * distance;
        if (g_course == 0 || g_rider_map.w == 0 || (map_open(g_rider_map, sx, sz) &&
            map_open(g_rider_map, sx - std::sin(turn) * kAbreast, sz + std::cos(turn) * kAbreast) &&
            map_open(g_rider_map, sx + std::sin(turn) * kAbreast, sz - std::cos(turn) * kAbreast))) return { sx, sz };
    }
    return toward(x, z, from_x, from_z, distance);
}

std::pair<float, float> rider_waypoint(Way& way, float x, float z, float tx, float tz) {
    const WaterMap& m = g_rider_map;
    if (g_course == 0 || m.w == 0 || m.course != g_course || open_line(m, x, z, tx, tz)) {
        way.route.clear();
        return { tx, tz };
    }
    const double now = seconds(std::chrono::steady_clock::now());
    if (way.route.empty() || std::hypot(tx - way.to_x, tz - way.to_z) > 400.0f || now - way.planned > 2.0) {
        plan_route(m, x, z, tx, tz, way.route);
        way.to_x = tx; way.to_z = tz; way.planned = now;
    }
    // The furthest of the next few points on it in open water from here (the
    // first, if none: from the shallows, the way out of them).
    std::pair<float, float> best = way.route.size() > 1 ? way.route[1] : !way.route.empty() ? way.route[0] : std::pair{ tx, tz };
    for (size_t i = 1; i < way.route.size() && i < 12; ++i) {
        if (open_line(m, x, z, way.route[i].first, way.route[i].second)) best = way.route[i];
    }
    return best;
}

// Somewhere to roam to: open water by chance, within 3000 of it.
void pick_goal(const uint8_t* rdram) {
    Dolphin& o = g_dolphin;
    for (int i = 0; i < 20; ++i) {
        const float a = uniform(0.0f, 6.2831853f), r = uniform(800.0f, 3000.0f);
        const float gx = o.x + std::cos(a) * r, gz = o.z + std::sin(a) * r;
        if (dolphin_water(rdram, g_course, gx, gz)) { o.goal_x = gx; o.goal_z = gz; return; }
    }
    o.goal_x = o.x; o.goal_z = o.z;
}

// The dolphin this frame: its mind, then its swimming.
void dolphin_frame(const uint8_t* rdram, uint32_t segment3, std::chrono::steady_clock::time_point when, float dt) {
    Dolphin& o = g_dolphin;
    const double now = seconds(when);
    if (!o.placed) {
        // Somewhere along the buoys' line, in open water.
        o = Dolphin{};
        for (int i = 0; i < 40 && !g_course_lap.empty(); ++i) {
            const auto& p = g_course_lap[chance()() % g_course_lap.size()];
            o.x = p.first; o.z = p.second;
            if (dolphin_water(rdram, g_course, o.x, o.z)) break;
        }
        o.placed = true;
        if (g_water_map.course != g_course) build_water_map(rdram);
        g_route.clear();
        g_route_x = g_route_z = 1e9f;
        pick_goal(rdram);
        start_act(rdram, DolphinAct::Swim, now);
        set_phase(DolphinPhase::Roam, -1, now + uniform(3.0f, 8.0f), "out");
    }

    // Its mind.
    float tx = 0, tz = 0, tfx = 1, tfz = 0, tspeed = 0;
    const bool there = o.target >= 0 && audience_place(o.target, tx, tz, tfx, tfz, tspeed);
    const bool attended = there && attending(o.target);
    switch (o.phase) {
    case DolphinPhase::Roam:
        if (now >= o.phase_until) {
            const int t = pick_audience(now);
            if (t >= 0) set_phase(DolphinPhase::Seek, t, now + 90.0, "off to find someone");
            else o.phase_until = now + uniform(3.0f, 8.0f);
        }
        break;
    case DolphinPhase::Seek:
        if (!there) set_phase(DolphinPhase::Roam, -1, now + uniform(2.0f, 5.0f), "lost them");
        else if (attended) {
            o.interest = uniform(30.0f, 60.0f);
            o.bonus = 60.0 - o.interest;
            set_phase(DolphinPhase::Entertain, o.target, 0, "has their attention");
        }
        else if (now >= o.phase_until) {
            o.shunned_until[o.target] = now + 60.0;
            set_phase(DolphinPhase::Roam, -1, now + uniform(2.0f, 5.0f), "gave up on them");
        }
        else if (!o.arrived && o.staged && std::hypot(o.stage_x - o.x, o.stage_z - o.z) < 900.0f) {
            // There: 12 to 30 seconds to catch their eye. (Getting there,
            // a minute and a half at most.)
            o.arrived = true;
            o.phase_until = now + uniform(12.0f, 30.0f);
        }
        break;
    case DolphinPhase::Entertain:
        if (!there) { set_phase(DolphinPhase::Roam, -1, now + uniform(2.0f, 5.0f), "lost them"); break; }
        if (attended) { o.interest -= dt; o.unattended = 0; o.attended_for += dt; }
        else o.unattended += dt;
        if (o.interest <= 0) {
            // Bored of them: off it goes; a rider still taken with it trails
            // after it a little, then gives up.
            const int was = o.target;
            o.shunned_until[was] = now + 60.0;
            if ((was == 1 || was == 2) && g_minds[was - 1].doing == minds::Activity::Dolphin && g_minds[was - 1].until > now)
                g_minds[was - 1].until = now + uniform(3.0f, 10.0f);
            pick_goal(rdram);
            set_phase(DolphinPhase::Leave, -1, now + uniform(8.0f, 14.0f), "bored of them");
        }
        else if (o.unattended > 4.0) set_phase(DolphinPhase::Chase, o.target, now + uniform(3.0f, 10.0f), "they lost interest; after them");
        break;
    case DolphinPhase::Chase:
        if (!there) set_phase(DolphinPhase::Roam, -1, now + uniform(2.0f, 5.0f), "lost them");
        else if (attended) {
            o.interest = uniform(30.0f, 60.0f);
            o.bonus = 60.0 - o.interest;
            set_phase(DolphinPhase::Entertain, o.target, 0, "won them back");
        }
        else if (now >= o.phase_until) {
            o.shunned_until[o.target] = now + 60.0;
            pick_goal(rdram);
            set_phase(DolphinPhase::Leave, -1, now + uniform(6.0f, 10.0f), "gave up on them");
        }
        break;
    case DolphinPhase::Leave:
        if (now >= o.phase_until) set_phase(DolphinPhase::Roam, -1, now + uniform(2.0f, 6.0f), "looking about");
        break;
    }

    // Where it heads: with someone, a stage 700 in front of them (900 ahead
    // if they are on the move); chasing, right after them; otherwise its goal.
    float gx = o.goal_x, gz = o.goal_z;
    bool perform = false;
    if (o.target >= 0 && there) {
        // The stage: the first spot in open water that far ahead of them or
        // a little further, straight ahead or a little to either side;
        // failing all, the last one found.
        const float ahead = o.phase == DolphinPhase::Chase ? 300.0f : tspeed > 300.0f ? 900.0f + tspeed * 0.5f : 700.0f;
        bool found = false;
        for (int i = 0; i < 12 && !found; ++i) {
            const float distance = ahead + float(i / 3) * 250.0f, turn = float(i % 3 == 2 ? -1 : i % 3) * 0.35f;
            const float c = std::cos(turn), sn = std::sin(turn);
            const float fx = tfx * c - tfz * sn, fz = tfx * sn + tfz * c;
            if (dolphin_water(rdram, g_course, tx + fx * distance, tz + fz * distance)) {
                o.stage_x = tx + fx * distance;
                o.stage_z = tz + fz * distance;
                o.staged = found = true;
            }
        }
        if (o.staged) { gx = o.stage_x; gz = o.stage_z; }
        else { gx = o.x; gz = o.z; }
        if (o.phase != DolphinPhase::Chase) perform = std::hypot(gx - o.x, gz - o.z) < 450.0f || tspeed > 300.0f;
    }
    else if (std::hypot(gx - o.x, gz - o.z) < 300.0f) {
        pick_goal(rdram);
        gx = o.goal_x; gz = o.goal_z;
    }
    const bool on_the_move = std::hypot(gx - o.x, gz - o.z) > 450.0f || tspeed > 300.0f || o.phase == DolphinPhase::Chase;

    // Its acts: performing for someone, one after another; otherwise swimming,
    // with a leap now and then.
    if (now >= o.act_until) {
        if (perform || o.phase == DolphinPhase::Chase) next_act(rdram, on_the_move, now);
        else start_act(rdram, chance()() % 4u == 0 ? DolphinAct::Leap : DolphinAct::Swim, now);
    }
    const bool in_place = o.act == DolphinAct::Dance || o.act == DolphinAct::Call;

    // Its swimming: towards where it heads, turned aside from shallows and
    // the course's edge (the first heading either side of the one it wants
    // whose way ahead is open water), stopping for a performance in place.
    const float to_goal = std::hypot(gx - o.x, gz - o.z);
    // Its route there, found again when the goal has moved or every few
    // seconds; it steers for a point a few squares on along it.
    if (std::hypot(gx - g_route_x, gz - g_route_z) > 400.0f || now - g_route_at > 3.0) {
        plan_route(g_water_map, o.x, o.z, gx, gz, g_route);
        g_route_x = gx; g_route_z = gz; g_route_at = now;
    }
    while (g_route.size() > 1 && std::hypot(g_route[0].first - o.x, g_route[0].second - o.z) < 300.0f) g_route.erase(g_route.begin());
    float aim_x = gx, aim_z = gz;
    if (g_route.size() > 1) {
        const auto& p = g_route[std::min<size_t>(2, g_route.size() - 1)];
        aim_x = p.first; aim_z = p.second;
    }
    float want = to_goal > 1.0f ? std::atan2(aim_z - o.z, aim_x - o.x) : o.heading;
    // Clear of the jet skis, as the riders keep clear of each other.
    std::vector<Obstacle> skis;
    for (int t = 0; t < kAudience; ++t) {
        float ax, az, afx, afz, aspeed;
        if (audience_place(t, ax, az, afx, afz, aspeed)) skis.push_back({ ax, az, 450.0f, true });
    }
    const Avoid clear = avoid(o.x, o.z, o.heading, o.speed, skis);
    want += clear.turn;
    if (in_place && o.target >= 0 && there) want = std::atan2(tz - o.z, tx - o.x) + 1.5707963f;  // side on to them
    for (int i = 0; i <= 12; ++i) {
        const float h = want + (i % 2 ? 1.0f : -1.0f) * float((i + 1) / 2) * 0.4363323f;
        if (dolphin_water(rdram, g_course, o.x + std::cos(h) * 400.0f, o.z + std::sin(h) * 400.0f) &&
            dolphin_water(rdram, g_course, o.x + std::cos(h) * 900.0f, o.z + std::sin(h) * 900.0f)) {
            want = h;
            break;
        }
    }
    const float diff = std::remainder(want - o.heading, 6.2831853f);
    o.heading = std::remainder(o.heading + std::clamp(diff, -kDolphinTurn * dt, kDolphinTurn * dt), 6.2831853f);
    const float top = in_place ? 0.0f
                    : o.act == DolphinAct::Leap ? (on_the_move ? kDolphinLeap : 300.0f)
                    : !on_the_move ? std::min(250.0f, to_goal)
                    : o.target >= 0 && tspeed > 300.0f ? std::clamp(tspeed + 150.0f, kDolphinSwim, 1350.0f) : kDolphinSwim;
    o.speed += std::clamp(top - o.speed, -600.0f * dt, 450.0f * dt);
    const float nx = o.x + std::cos(o.heading) * o.speed * dt, nz = o.z + std::sin(o.heading) * o.speed * dt;
    if (dolphin_water(rdram, g_course, nx, nz) || !dolphin_water(rdram, g_course, o.x, o.z)) { o.x = nx; o.z = nz; }
    else o.speed *= 0.5f;
    o.y = water_height_at(segment3, o.x, o.z) - 10.0f;

    static const bool log = std::getenv("WR64_DOLPHIN_LOG") != nullptr;
    static double reported = 0;
    if (log && now - reported > 2.0) {
        reported = now;
        static const char* acts[] = { "swim", "leap", "dance", "call" };
        std::fprintf(stderr, "[wr64-dolphin]   at %.0f %.0f, %s, speed %.0f, heading for %.0f %.0f; %s at %.0f %.0f (speed %.0f), %s\n",
                     o.x, o.z, acts[int(o.act)], o.speed, gx, gz, audience_name(o.target), tx, tz, tspeed,
                     attended ? "attending" : "not attending");
        std::fflush(stderr);
    }

    DolphinSeen& seen = g_dolphin_seen;
    seen.out = true;
    seen.x = o.x; seen.z = o.z; seen.heading = o.heading; seen.speed = o.speed;
    seen.moving = !in_place && o.speed > 400.0f;
}

void journal_context(journal::Fields& f, int which) {
    f.add("course", int(g_course));
    if (g_player.known) f.raw("player", "{\"x\":" + std::to_string(int(g_player.x)) + ",\"z\":" + std::to_string(int(g_player.z)) +
                                          ",\"speed\":" + std::to_string(int(g_player.speed)) + "}");
    if (g_racers[0].ready) {
        const Drive& d = g_drive;
        f.raw("rider", "{\"x\":" + std::to_string(int(d.x)) + ",\"z\":" + std::to_string(int(d.z)) + ",\"speed\":" +
                           std::to_string(int(d.speed)) + ",\"doing\":\"" + minds::name(g_minds[0].doing) + "\"}");
    }
    if (g_pair_out && g_course != 0 && g_rider_map.w > 0) {
        std::string each = "[";
        for (int i = 0; i < 2; ++i) {
            const Rider& r = g_pair.riders[i];
            each += std::string(i ? "," : "") + "{\"x\":" + std::to_string(int(r.x)) + ",\"z\":" + std::to_string(int(r.z)) +
                    ",\"in_water\":" + (map_open(g_rider_map, r.x, r.z) ? "true" : "false") + "}";
        }
        f.raw("pair_riders", each + "]");
    }
    if (which == 1 || g_pair_out) {
        const Pair& p = g_pair;
        f.raw("pair", "{\"x\":" + std::to_string(int((p.riders[0].x + p.riders[1].x) / 2)) + ",\"z\":" +
                          std::to_string(int((p.riders[0].z + p.riders[1].z) / 2)) + "}");
    }
    if (g_more_out) {
        for (int i = 1; i < kDrives; ++i) {
            const Drive& d = g_drives[i];
            f.raw(kMindKeys[d.mind], "{\"x\":" + std::to_string(int(d.x)) + ",\"z\":" + std::to_string(int(d.z)) + ",\"speed\":" +
                                         std::to_string(int(d.speed)) + ",\"doing\":\"" + minds::name(g_minds[d.mind].doing) + "\"}");
        }
    }
    if (g_dolphin_seen.out) {
        const Dolphin& o = g_dolphin;
        f.raw("dolphin", "{\"x\":" + std::to_string(int(o.x)) + ",\"z\":" + std::to_string(int(o.z)) + ",\"phase\":\"" +
                             phase_name(o.phase) + "\",\"with\":\"" + audience_name(o.target) + "\"}");
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
    if (!enabled() || !g_pose.ready) return;
    if (course != 0 && !free_roam_active(rdram)) return;
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
    const bool fresh = g_racers[0].character < 0 || now - last > std::chrono::seconds(2) || course != g_course;
    if (fresh) {
        g_course = course;
        const int player = player_character(rdram);
        std::vector<int> others;
        for (int c = 0; c < kCharacters; ++c) if (c != player) others.push_back(c);
        std::shuffle(others.begin(), others.end(), chance());
        for (int i = 0; i < 3; ++i) {
            g_racers[i].character = others[i];
            g_racers[i].ready = load_racer(rdram, others[i], i);
        }
        if (course != 0) {
            build_rider_map(rdram);
            build_course_lap(rdram);
            build_course_spots(rdram);
        }
        else {
            if (std::getenv("WR64_OBJECT_DUMP")) { build_rider_map(rdram); g_ramps.clear(); g_ramp_angles.clear(); g_ramp_obstacles.clear(); }
            g_sights.assign(std::begin(kParkSights), std::end(kParkSights));
            g_trick = { kOtherCrestX, kOtherCrestZ, kOverX, kOverZ, kStage };
            g_sight_seen.assign(g_sights.size(), -1e9);
            g_mines.clear();
            g_slope_pieces.clear();
            g_ramp_shapes.clear();
        }
        std::fprintf(stderr, "[wr64-companion] character %d rides with the player (character %d)%s; %d and %d ride together%s\n",
                     others[0], player, g_racers[0].ready ? "" : " (its models did not load)", others[1], others[2],
                     g_racers[1].ready && g_racers[2].ready ? "" : " (their models did not load)");
        std::fflush(stderr);
    }
    last = now;
    // The dolphin, on the courses it suits: not Dolphin Park (the game has
    // its own), Twilight City (a city) or Glacier Coast (too cold).
    const bool dolphin_course = course != 0 && course != 6 && course != 7;
    if (fresh) {
        g_dolphin_ready = dolphin_course && load_dolphin(rdram);
        g_dolphin.placed = false;
        g_dolphin_seen = DolphinSeen{};
    }
    const bool first_ready = g_racers[0].ready && (course == 0 || !g_course_lap.empty());
    const bool pair_ready = g_racers[1].ready && g_racers[2].ready && (course == 0 || (!g_course_lap.empty() && !g_sights.empty()));
    g_pair_out = pair_ready;
    if (!first_ready && !pair_ready && !g_dolphin_ready) return;

    // Their riding this frame, each keeping clear of the player (as last
    // seen riding), the pair of the first too.
    static Clock::time_point last_ride{};
    const float dt = std::clamp(std::chrono::duration<float>(now - last_ride).count(), 0.0f, 0.1f);
    last_ride = now;
    const Obstacle player{ g_pose.hull.m[3][0], g_pose.hull.m[3][2], 450.0f, true };
    if (fresh) {
        g_stunt = word_at(rdram, kGameModes) == kStuntMode;
        g_player = Player{};
        start_drive(g_drives[0], 0, 0, now);
        start_pair(now);
        g_more_out = more_riders_on(course) && first_ready && pair_ready;
        if (g_more_out) {
            for (int i = 1; i < kDrives; ++i) start_drive(g_drives[i], i, i + 1, now);
            start_wings();
        }
    }
    note_player(rdram, segments[3], dt, now);
    // The dolphin is drawn after the riders: its setup list (0x800E6CA0)
    // leaves the state its own, which drew the riders grey and speckled.
    static std::vector<std::pair<uint32_t, uint32_t>> dolphin;
    dolphin.clear();
    if (g_dolphin_ready) {
        // WR64_DOLPHIN_TEST_ANIM=<0-7>, in a scripted run only: the dolphin
        // in that animation, round and round, 450 units ahead of the player
        // and turned across its way, instead of out on its own.
        static const int test_animation = [] {
            const char* value = std::getenv("WR64_DOLPHIN_TEST_ANIM");
            return value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr ? std::atoi(value) : -1;
        }();
        if (test_animation >= 0 && test_animation < kDolphinAnimations) {
            static Clock::time_point since = now;
            const int frame = int(std::chrono::duration<float>(now - since).count() * kDolphinFps) % dolphin_frames(rdram, test_animation);
            const float (*h)[4] = g_pose.hull.m;
            const float length = std::max(1e-3f, std::hypot(h[2][0], h[2][2]));
            const float fx = h[2][0] / length, fz = h[2][2] / length;
            const float x = h[3][0] + fx * 450.0f, z = h[3][2] + fz * 450.0f;
            draw_dolphin(rdram, b, dolphin_matrix(x, water_height_at(segments[3], x, z), z, -fz, fx, 0.0f), test_animation,
                         frame, dolphin);
        }
        else {
            dolphin_frame(rdram, segments[3], now, dt);
            const Dolphin& o = g_dolphin;
            const int frame = int((seconds(now) - o.act_since) * kDolphinFps) % dolphin_frames(rdram, o.animation);
            draw_dolphin(rdram, b, dolphin_matrix(o.x, o.y, o.z, std::cos(o.heading), std::sin(o.heading), 0.0f), o.animation,
                         frame, dolphin);
        }
    }
    // Everyone out besides the first rider and the pair: the other singles,
    // the trio's leader and its two, as obstacles (the trio's own leave
    // each other be).
    g_crowd.clear();
    if (g_more_out) {
        for (int i = 1; i < kDrives; ++i) g_crowd.push_back({ g_drives[i].x, g_drives[i].z, 300.0f, true });
        for (const Rider& r : g_wings) g_crowd.push_back({ r.x, r.z, 300.0f, true });
    }
    // Away from the park the first rider shares the buoys' line with the
    // pair, and keeps clear of them too (and of the others, wherever).
    if (first_ready) {
        std::vector<Obstacle> others{ player };
        if (pair_ready && course != 0) {
            for (const Rider& r : g_pair.riders) others.push_back({ r.x, r.z, 300.0f, true });
        }
        others.insert(others.end(), g_crowd.begin(), g_crowd.end());
        if (course != 0) for (const auto& [mx, mz] : g_mines) others.push_back({ mx, mz, kMineRoom, true });
        drive(g_drive, segments[3], now, dt, others);
    }
    const Drive& d = g_drive;
    const Obstacle first{ d.x, d.z, 450.0f, true };
    if (pair_ready) ride_pair(segments[3], now, dt, &player, first_ready ? &first : nullptr);
    // The other singles and the trio, each clear of everyone but its own.
    if (g_more_out) {
        std::vector<Obstacle> base{ player, first };
        for (const Rider& r : g_pair.riders) base.push_back({ r.x, r.z, 300.0f, true });
        for (int i = 1; i < kDrives; ++i) {
            std::vector<Obstacle> others = base;
            for (int k = 1; k < kDrives; ++k) {
                if (k != i) others.push_back({ g_drives[k].x, g_drives[k].z, 300.0f, true });
            }
            if (i != kTrioLeader) for (const Rider& r : g_wings) others.push_back({ r.x, r.z, 300.0f, true });
            // (Away from the park the trio's leader goes round the ramps.)
            if (i == kTrioLeader && course != 0) others.insert(others.end(), g_ramp_obstacles.begin(), g_ramp_obstacles.end());
            else if (course != 0) for (const auto& [mx, mz] : g_mines) others.push_back({ mx, mz, kMineRoom, true });
            drive(g_drives[i], segments[3], now, dt, others);
        }
        std::vector<Obstacle> others = base;
        for (int k = 1; k < kTrioLeader; ++k) others.push_back({ g_drives[k].x, g_drives[k].z, 300.0f, true });
        ride_wings(segments[3], dt, others);
    }

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
    // (An extra rider, `extra` from 0, has matrices of its own.)
    auto draw = [&](int slot, Matrix hull, Matrix body, bool body_shown, int extra = -1) {
        for (Matrix* m : { &hull, &body }) { m->m[3][0] += offset.first; m->m[3][2] += offset.second; }
        const uint32_t base = (extra < 0 ? kSlots[slot].matrices : kExtraMatrices + uint32_t(extra) * kExtraStride) +
                              uint32_t(b) * kMatrixStride;
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
    // Away from the park, any rider aground (in water too shallow, or on
    // land).
    if (report && course != 0 && g_rider_map.w > 0) {
        auto check = [&](const char* who, float x, float z) {
            if (!rider_water(rdram, course, x, z)) std::fprintf(stderr, "[wr64-companion] %s is aground at %.0f %.0f\n", who, x, z);
        };
        if (first_ready) check("the first rider", d.x, d.z);
        if (pair_ready) {
            check("the pair's first", g_pair.riders[0].x, g_pair.riders[0].z);
            check("the pair's second", g_pair.riders[1].x, g_pair.riders[1].z);
        }
    }

    // A single's hull and body, the frame's.
    auto single = [&](const Drive& s, Matrix& hull, Matrix& body, bool& body_shown) {
        hull = frame_matrix(s.x, s.y, s.z, std::cos(s.heading), std::sin(s.heading), std::atan(s.climb), s.bank, s.flip);
        // After a bail the rider apart from it, and gone once under.
        body = s.crash == Crash::None ? hull : frame_matrix(s.body_x, s.body_y, s.body_z, s.body_hx, s.body_hz, 0.0f, 0.0f, s.body_spin);
        body_shown = s.crash == Crash::None || s.body_y > water_height_at(segments[3], s.body_x, s.body_z) - 80.0f;
    };
    if (first_ready) {
        const float hx = std::cos(d.heading), hz = std::sin(d.heading);
        Matrix hull, body;
        bool body_shown;
        single(d, hull, body, body_shown);
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
                         d.free == Free::Follow ? "riding along" : d.free == Free::Rest ? "resting" : d.free == Free::Watch ? "watching"
                         : d.routine == Routine::Stunts ? "stunts" : d.routine == Routine::Lap ? "lap" : "practising",
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
                             : g_pair.routine == PairRoutine::Follow ? "riding along" : g_pair.routine == PairRoutine::Watch ? "watching"
                             : g_pair.parked ? "stopped" : "sightseeing",
                             a.speed, c.speed, apart, closer ? " (nearest yet)" : "",
                             first_ready ? nearer(d.x, d.z) : -1.0f, nearer(player.x, player.z), player.x, player.z);
                std::fflush(stderr);
            }
        }
    }
    // The other singles, the trio's leader and its two -- no spray yet
    // (src/body_splash.cpp has room for three) -- drawn from the racers'
    // models: the singles the pair's, the trio one of each.
    if (g_more_out) {
        static const int kModelOf[5] = { 1, 2, 0, 1, 2 };
        for (int i = 1; i < kDrives; ++i) {
            Matrix hull, body;
            bool body_shown;
            single(g_drives[i], hull, body, body_shown);
            draw(kModelOf[i - 1], hull, body, body_shown, i - 1);
        }
        for (int i = 0; i < 2; ++i) {
            const Rider& r = g_wings[i];
            const Matrix hull = frame_matrix(r.x, r.y, r.z, std::cos(r.heading), std::sin(r.heading), 0.0f, r.bank, 0.0f);
            draw(kModelOf[3 + i], hull, hull, true, 3 + i);
        }
        if (report) {
            for (int i = 1; i < kDrives; ++i) {
                const Drive& o = g_drives[i];
                std::fprintf(stderr, "[wr64-companion] %s at %.0f %.0f, %s, speed %.0f\n", kMindNames[o.mind], o.x, o.z,
                             o.free == Free::Follow ? "riding along" : o.free == Free::Rest ? (o.parked ? "resting" : "off to rest")
                             : o.free == Free::Watch ? "watching" : o.routine == Routine::Stunts ? "stunts" : o.routine == Routine::Lap ? "lap"
                             : "practising", o.speed);
            }
            const Drive& l = g_drives[kTrioLeader];
            std::fprintf(stderr, "[wr64-companion] the trio's wings %.0f and %.0f from their places\n",
                         std::hypot(wedge_place(l, 0, false).first - g_wings[0].x, wedge_place(l, 0, false).second - g_wings[0].z),
                         std::hypot(wedge_place(l, 1, false).first - g_wings[1].x, wedge_place(l, 1, false).second - g_wings[1].z));
            std::fflush(stderr);
        }
    }
    out.insert(out.end(), dolphin.begin(), dolphin.end());
}

}  // namespace wr64

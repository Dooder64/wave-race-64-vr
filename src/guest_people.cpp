// A guest course's people (src/guest_course.cpp, docs/GUEST_COURSES.md):
// characters of the other game, drawn from the pack's lists, posed here
// every frame and given something to do.
//
// The pack (tools/guest/greatbay_pack.py) holds each model's limbs (joint
// position, child, sibling, its matrix's slot), its animations (frames of
// the root's translation and each limb's rotation) and each person's list:
// every drawing limb under its slot's matrix, every item it can hold under
// one more. Here each limb's matrix is worked out as Majora's Mask's
// SkelAnime does -- the parent's, then the joint's translation, then
// rotations about z, y and x -- between two frames of its animation, placed
// where the person is (its heading, a tilt, the model's scale), and written
// as an N64 matrix with the scene's scale folded in (a matrix multiplied
// onto the scene's in the list was never seen).
//
// Great Bay's people:
//   - five Zoras, each with a mind of its own (as the riders have, docs/
//     RIDER_PERSONALITIES.md): wants that grow -- to roam, to rest, to be
//     near the others -- weighed by its personality when it next decides,
//     between treading water where it is and swimming off (somewhere new,
//     to another Zora, to see the player); a shy one darts off from a jet
//     ski passing close;
//   - Link, by turns fishing from the Marine Research Lab's landing
//     platform (a Deku Stick held out over the water) and swimming out to
//     dive and come up with a green rupee, held up as when one is found.
//     He changes between the two only where the player can see neither.
//
// WR64_PEOPLE_LOG=1 logs their decisions.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace wr64 {
namespace guest {
namespace people {
namespace {

constexpr float kPi = 3.14159265f;
constexpr uint32_t kBase = 0x400000;          // the pack (src/guest_course.cpp)

struct Mat { float m[4][4]; };               // column vectors: m[row][col]

Mat identity() {
    Mat r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
}
Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
Mat translate(float x, float y, float z) {
    Mat r = identity();
    r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
    return r;
}
Mat scale(float s) {
    Mat r = identity();
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}
Mat rot_x(float a) { Mat r = identity(); const float c = std::cos(a), s = std::sin(a); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }
Mat rot_y(float a) { Mat r = identity(); const float c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r; }
Mat rot_z(float a) { Mat r = identity(); const float c = std::cos(a), s = std::sin(a); r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r; }

void put_word(uint8_t* rdram, uint32_t address, uint32_t value) { std::memcpy(rdram + (address & 0x7FFFFC), &value, 4); }

// An N64 matrix (row vectors, so the transpose; integer halves, then
// fractions, two to a word) at `address`.
void write_matrix(uint8_t* rdram, uint32_t address, const Mat& m) {
    uint16_t whole[16], part[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const int32_t v = int32_t(std::lround(double(m.m[c][r]) * 65536.0));
            whole[r * 4 + c] = uint16_t(uint32_t(v) >> 16);
            part[r * 4 + c] = uint16_t(uint32_t(v) & 0xFFFF);
        }
    for (int k = 0; k < 8; ++k) {
        put_word(rdram, address + uint32_t(k) * 4, (uint32_t(whole[k * 2]) << 16) | whole[k * 2 + 1]);
        put_word(rdram, address + 32 + uint32_t(k) * 4, (uint32_t(part[k * 2]) << 16) | part[k * 2 + 1]);
    }
}

template <typename T> T read(const std::vector<uint8_t>& d, uint32_t at) {
    T v{};
    if (at + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + at, sizeof(T));
    return v;
}

struct Limb { int16_t x, y, z; uint8_t child, sibling; int8_t slot; };
struct Anim { uint32_t frames = 0, data = 0; };
struct Model {
    std::vector<Limb> limbs;
    uint32_t slots = 0;
    std::vector<Anim> anims;
    std::vector<uint32_t> hands;     // each item's limb
    float scale = 0.01f;
};
struct Person {
    uint32_t model = 0, matrices = 0;
    std::vector<uint32_t> item_calls;
    float home_x = 0, home_z = 0, home_yaw = 0;
};

// What the pack says, read once a ride.
struct People {
    bool ready = false;
    std::vector<Model> models;
    std::vector<Person> people;
    float water_x0 = 0, water_z0 = 0, water_cell = 100;
    uint32_t water_w = 0, water_h = 0, water_at = 0;
    float world_scale = 0.83f;
} g;

const std::vector<uint8_t>* g_data = nullptr;

bool water(float x, float z) {
    const int i = int((x - g.water_x0) / g.water_cell), j = int((z - g.water_z0) / g.water_cell);
    if (i < 0 || j < 0 || uint32_t(i) >= g.water_w || uint32_t(j) >= g.water_h) return false;
    return (*g_data)[g.water_at + uint32_t(j) * g.water_w + uint32_t(i)] != 0;
}

// Open water all the way from (ax, az) to (bx, bz) -- but for the first 200
// (it may be leaving the shallows).
bool water_line(float ax, float az, float bx, float bz) {
    const float length = std::hypot(bx - ax, bz - az);
    for (float d = 200.0f; d <= length; d += g.water_cell * 0.5f) {
        if (!water(ax + (bx - ax) * d / length, az + (bz - az) * d / length)) return false;
    }
    return true;
}

// WR64_PEOPLE_SHOW=<person>,<x>,<z> (Majora's Mask units), in a scripted
// run only: that person drawn there, whatever it is doing, to bring it
// before a test's camera.
struct Show { int person = -1; float x = 0, z = 0; };
const Show& show() {
    static const Show s = [] {
        Show v;
        const char* value = std::getenv("WR64_PEOPLE_SHOW");
        if (value != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr) std::sscanf(value, "%d,%f,%f", &v.person, &v.x, &v.z);
        return v;
    }();
    return s;
}

// A pose: animation `anim` at `frame` (fractional, wrapping), each joint
// between its two frames.
void pose(uint8_t* rdram, const Person& p, Mat place, int anim, float frame, bool hold_last,
          const std::array<bool, 4>& items) {
    const Model& m = g.models[p.model];
    if (show().person >= 0 && &p == &g.people[size_t(show().person)]) {
        place.m[0][3] = show().x;
        place.m[2][3] = show().z;
    }
    if (anim < 0 || size_t(anim) >= m.anims.size() || m.anims[anim].frames == 0) return;
    const Anim& a = m.anims[anim];
    const size_t joints = m.limbs.size() + 1;
    float f = frame;
    if (hold_last) f = std::min(f, float(a.frames - 1));
    else f = std::fmod(std::max(0.0f, f), float(a.frames));
    const uint32_t f0 = uint32_t(f) % a.frames, f1 = hold_last ? std::min(f0 + 1, a.frames - 1) : (f0 + 1) % a.frames;
    const float t = f - std::floor(f);
    auto joint = [&](size_t j, int axis) {
        const int16_t v0 = read<int16_t>(*g_data, a.data + uint32_t((f0 * joints + j) * 6 + axis * 2));
        const int16_t v1 = read<int16_t>(*g_data, a.data + uint32_t((f1 * joints + j) * 6 + axis * 2));
        return float(v0) + float(int16_t(uint16_t(v1 - v0))) * t;      // the short way round
    };
    const Mat world = mul(scale(g.world_scale), mul(place, scale(m.scale)));
    std::vector<Mat> limb_mats(m.limbs.size());
    // Depth first from limb 0, child then sibling (SkelAnime's order).
    struct Step { int limb; Mat parent; };
    std::vector<Step> stack{ { 0, identity() } };
    while (!stack.empty()) {
        const Step s = stack.back();
        stack.pop_back();
        if (s.limb == 0xFF || size_t(s.limb) >= m.limbs.size()) continue;
        const Limb& l = m.limbs[size_t(s.limb)];
        const float px = s.limb == 0 ? joint(0, 0) : float(l.x);
        const float py = s.limb == 0 ? joint(0, 1) : float(l.y);
        const float pz = s.limb == 0 ? joint(0, 2) : float(l.z);
        const float k = kPi / 32768.0f;
        const size_t j = size_t(s.limb) + 1;
        const Mat here = mul(s.parent, mul(translate(px, py, pz), mul(rot_z(joint(j, 2) * k), mul(rot_y(joint(j, 1) * k), rot_x(joint(j, 0) * k)))));
        limb_mats[size_t(s.limb)] = here;
        if (l.slot >= 0) write_matrix(rdram, kBase + p.matrices + uint32_t(l.slot) * 64, mul(world, here));
        static int told = 0;
        if (std::getenv("WR64_PEOPLE_LOG") != nullptr && l.slot == 0 && told++ < 3) {
            const Mat w = mul(world, here);
            std::fprintf(stderr, "[wr64-people] limb %d slot 0 at %.1f %.1f %.1f, scale %.4f; matrices at 0x%X\n", s.limb, w.m[0][3], w.m[1][3],
                         w.m[2][3], w.m[0][0], kBase + p.matrices);
        }
        stack.push_back({ l.sibling, s.parent });
        stack.push_back({ l.child, here });
    }
    // The items: in their limb's hand, on or off.
    for (size_t i = 0; i < m.hands.size() && i < p.item_calls.size(); ++i) {
        const uint32_t hand = m.hands[i];
        if (hand < limb_mats.size()) write_matrix(rdram, kBase + p.matrices + (m.slots + uint32_t(i)) * 64, mul(world, limb_mats[hand]));
        put_word(rdram, kBase + p.item_calls[i], i < items.size() && items[i] ? 0x06000000u : 0x00000000u);
    }
}

std::mt19937& rng() { static std::mt19937 r{ 2026u }; return r; }
float uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng()); }

bool logging() { static const bool on = std::getenv("WR64_PEOPLE_LOG") != nullptr; return on; }

// ---- the Zoras ------------------------------------------------------------

enum class ZoraDoing { Tread, Swim };
const char* zora_doing(ZoraDoing d) { return d == ZoraDoing::Tread ? "treading water" : "swimming"; }

struct Zora {
    float x = 0, z = 0, yaw = 0, speed = 0, frame = 0, tilt = 0;
    ZoraDoing doing = ZoraDoing::Tread;
    double until = 0;
    float tx = 0, tz = 0;
    const char* why = "";
    // Its personality (0 to 1 each), and its wants (growing as it goes).
    float wanderlust = 0.5f, laziness = 0.5f, sociability = 0.5f, curiosity = 0.5f, shyness = 0.5f;
    float want_roam = 0, want_rest = 0, want_company = 0;
};
std::vector<Zora> g_zoras;

constexpr float kZoraSwim = 260.0f;          // Majora's Mask units a second
constexpr float kZoraTurn = 1.4f;            // radians a second
constexpr float kZoraFloat = -22.0f;         // its origin below the surface, treading
constexpr float kZoraSwimTilt = -1.35f;      // face down, swimming

void zora_choose(Zora& z, int index, double now, float player_x, float player_z, bool player_known) {
    // Each way to go, its weight: what it wants, as it is.
    const float rest = 0.2f + z.want_rest * (0.5f + z.laziness);
    const float roam = 0.2f + z.want_roam * (0.5f + z.wanderlust);
    const float company = z.want_company * (0.3f + z.sociability);
    const float player_far = player_known ? std::hypot(player_x - z.x, player_z - z.z) : 1e9f;
    const float look = player_known && player_far < 4000.0f ? z.curiosity * (1.0f - player_far / 4000.0f) * 1.5f : 0.0f;
    const float total = rest + roam + company + look;
    float pick = uniform(0.0f, total);
    auto swim_to = [&](float x, float zz, const char* why) {
        if (!water_line(z.x, z.z, x, zz)) return false;
        z.doing = ZoraDoing::Swim;
        z.tx = x; z.tz = zz;
        z.why = why;
        z.until = now + 60.0;
        return true;
    };
    bool chose = false;
    if ((pick -= rest) < 0.0f) {
        chose = true;
    }
    else if ((pick -= roam) < 0.0f) {
        for (int k = 0; k < 12 && !chose; ++k) {
            const float a = uniform(-kPi, kPi), d = uniform(800.0f, 3000.0f);
            chose = swim_to(z.x + std::cos(a) * d, z.z + std::sin(a) * d, "somewhere new");
        }
        if (chose) z.want_roam = 0.0f;
    }
    else if ((pick -= company) < 0.0f) {
        const int other = (index + 1 + int(uniform(0.0f, float(g_zoras.size() - 1)))) % int(g_zoras.size());
        const Zora& o = g_zoras[size_t(other)];
        const float a = uniform(-kPi, kPi);
        chose = swim_to(o.x + std::cos(a) * 250.0f, o.z + std::sin(a) * 250.0f, "to another Zora");
        if (chose) z.want_company = 0.0f;
    }
    else {
        const float a = std::atan2(z.z - player_z, z.x - player_x);
        chose = swim_to(player_x + std::cos(a) * 700.0f, player_z + std::sin(a) * 700.0f, "to see the jet ski");
    }
    if (!chose || z.doing == ZoraDoing::Tread) {
        z.doing = ZoraDoing::Tread;
        z.why = "here";
        z.until = now + uniform(6.0f, 20.0f) * (0.5f + z.laziness);
        z.want_rest = 0.0f;
    }
    if (logging())
        std::fprintf(stderr, "[wr64-people] Zora %d at %.0f %.0f: %s %s (rest %.2f, roam %.2f, company %.2f, look %.2f)\n", index, z.x, z.z,
                     zora_doing(z.doing), z.why, rest, roam, company, look);
}

void zora_frame(uint8_t* rdram, Zora& z, int index, const Person& p, double now, float dt, float player_x, float player_z,
                float player_speed, bool player_known) {
    // Its wants grow with what it is not doing.
    if (z.doing == ZoraDoing::Tread) z.want_roam += dt * 0.05f * (0.3f + z.wanderlust);
    else z.want_rest += dt * 0.08f * (0.3f + z.laziness);
    z.want_company += dt * 0.03f * z.sociability;
    // A jet ski close and fast: a shy one darts away.
    const float player_far = player_known ? std::hypot(player_x - z.x, player_z - z.z) : 1e9f;
    if (player_far < 450.0f && player_speed > 300.0f && uniform(0.0f, 1.0f) < z.shyness * dt * 4.0f &&
        !(z.doing == ZoraDoing::Swim && std::strcmp(z.why, "away from the jet ski") == 0)) {
        const float a = std::atan2(z.z - player_z, z.x - player_x);
        const float x = z.x + std::cos(a) * 1500.0f, zz = z.z + std::sin(a) * 1500.0f;
        if (water_line(z.x, z.z, x, zz)) {
            z.doing = ZoraDoing::Swim; z.tx = x; z.tz = zz; z.why = "away from the jet ski"; z.until = now + 20.0;
            if (logging()) std::fprintf(stderr, "[wr64-people] Zora %d darts away from the jet ski\n", index);
        }
    }
    if (now > z.until) zora_choose(z, index, now, player_x, player_z, player_known);
    float want_yaw = z.yaw;
    if (z.doing == ZoraDoing::Swim) {
        const float dx = z.tx - z.x, dz = z.tz - z.z, left = std::hypot(dx, dz);
        if (left < 80.0f) {
            z.until = 0.0;                   // there: decide again
            z.doing = ZoraDoing::Tread;
        }
        // (Majora's Mask's yaw: 0 faces +z, the way it goes (sin, cos).)
        want_yaw = std::atan2(dx, dz);
        z.speed += (std::min(kZoraSwim, left) - z.speed) * std::min(1.0f, dt * 1.5f);
    }
    else {
        z.speed += (0.0f - z.speed) * std::min(1.0f, dt * 2.0f);
        // Treading, it faces the jet ski if it is near and it is curious,
        // else the nearest of the others close by.
        if (player_known && player_far < 1500.0f && z.curiosity > 0.3f) want_yaw = std::atan2(player_x - z.x, player_z - z.z);
        else {
            float best = 900.0f;
            for (const Zora& o : g_zoras) {
                const float d = std::hypot(o.x - z.x, o.z - z.z);
                if (&o != &z && d < best) { best = d; want_yaw = std::atan2(o.x - z.x, o.z - z.z); }
            }
        }
    }
    const float turn = std::remainder(want_yaw - z.yaw, 2.0f * kPi);
    z.yaw += std::clamp(turn, -kZoraTurn * dt, kZoraTurn * dt);
    const float nx = z.x + std::sin(z.yaw) * z.speed * dt, nz = z.z + std::cos(z.yaw) * z.speed * dt;
    if (water(nx, nz) || !water(z.x, z.z)) { z.x = nx; z.z = nz; }
    else if (z.speed > 20.0f) {
        // Blocked: it gives up and treads water a while before going on.
        z.speed *= 0.5f;
        z.doing = ZoraDoing::Tread;
        z.why = "here (blocked)";
        z.until = now + uniform(3.0f, 8.0f);
    }
    // Face down swimming, upright treading, eased between.
    const float tilt = z.doing == ZoraDoing::Swim && z.speed > 60.0f ? kZoraSwimTilt : 0.0f;
    z.tilt += (tilt - z.tilt) * std::min(1.0f, dt * 2.5f);
    const bool swimming = z.tilt < kZoraSwimTilt * 0.5f;
    z.frame += dt * 20.0f * (swimming ? std::max(0.6f, z.speed / kZoraSwim) : 1.0f);
    const float bob = std::sin(float(now) * 2.0f + float(index)) * 3.0f;
    const Mat place = mul(translate(z.x, kZoraFloat * (1.0f - z.tilt / kZoraSwimTilt) - 8.0f * (z.tilt / kZoraSwimTilt) + bob, z.z),
                          mul(rot_y(z.yaw), rot_x(-z.tilt)));
    pose(rdram, p, place, swimming ? 1 : 0, z.frame, false, {});
}

// ---- Link -------------------------------------------------------------------

enum class LinkStep { Fish, SwimOut, Dive, Under, Rise, Show, Tread, SwimBack };
const char* link_step(LinkStep s) {
    static const char* names[] = { "fishing", "swimming out", "diving", "under", "coming up", "holding up a rupee", "treading water", "swimming back" };
    return names[int(s)];
}
// Majora's Mask units. The platform: the landing platform by the Marine
// Research Lab (its slab at 15, x -2989 to -2789, z 3898 to 4298, at the
// foot of the lab's ladder), Link at its seaward edge facing out (+x); he
// swims from its edge to the water he dives in. (The first place, (1300,
// -1750), was nowhere near it: the user never saw him, 2026-11-03.)
constexpr float kFishX = -2820.0f, kFishY = 15.0f, kFishZ = 4098.0f, kFishYaw = kPi / 2.0f;
constexpr float kSwimFromX = -2740.0f, kSwimFromZ = 4098.0f;
constexpr float kDiveX = -2300.0f, kDiveZ = 3200.0f;
constexpr float kLinkSwim = 180.0f;

struct Link {
    LinkStep step = LinkStep::Fish;
    float x = kFishX, y = kFishY, z = kFishZ, yaw = kFishYaw, frame = 0;
    double since = 0, until = 0;
    float likes_fishing = 0.6f;          // his liking for the one over the other
    bool started = false;
} g_link;

void link_go(Link& l, LinkStep s, double now, double length) {
    l.step = s;
    l.since = now;
    l.until = now + length;
    l.frame = 0;
    if (logging()) std::fprintf(stderr, "[wr64-people] Link: %s\n", link_step(s));
}

void link_frame(uint8_t* rdram, Link& l, const Person& p, double now, float dt, float player_x, float player_z, bool player_known) {
    if (!l.started) {
        l.started = true;
        // (WR64_PEOPLE_LINK=dive, in a scripted run: straight out to dive.)
        static const bool dive = std::getenv("WR64_PEOPLE_LINK") != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr;
        if (dive) {
            l.x = kDiveX + 300.0f; l.y = -30.0f; l.z = kDiveZ;
            link_go(l, LinkStep::SwimOut, now, 60.0);
        }
        else link_go(l, LinkStep::Fish, now, uniform(15.0f, 35.0f));
    }
    // Whether he may change from one to the other here: in sight or not
    // (kept out of sight, the user never saw him change, 2026-11-03), from
    // the slab's edge into the water and back.
    auto unseen = [&](float, float, float, float) { return true; };
    (void)player_x; (void)player_z; (void)player_known;
    auto swim_toward = [&](float tx, float tz) {
        const float dx = tx - l.x, dz = tz - l.z, left = std::hypot(dx, dz);
        const float want = std::atan2(dx, dz);
        l.yaw += std::clamp(std::remainder(want - l.yaw, 2.0f * kPi), -1.5f * dt, 1.5f * dt);
        const float step = std::min(left, kLinkSwim * dt);
        l.x += std::sin(l.yaw) * step;
        l.z += std::cos(l.yaw) * step;
        return left < 40.0f;
    };
    int anim = 4;
    bool hold = false;
    std::array<bool, 4> items{};
    switch (l.step) {
        case LinkStep::Fish:
            l.x = kFishX; l.y = kFishY; l.z = kFishZ; l.yaw = kFishYaw;
            anim = 4; items[1] = true;
            if (now > l.until) {
                if (uniform(0.0f, 1.0f) > l.likes_fishing && unseen(kFishX, kFishZ, kSwimFromX, kSwimFromZ)) {
                    l.x = kSwimFromX; l.y = -30.0f; l.z = kSwimFromZ;
                    link_go(l, LinkStep::SwimOut, now, 60.0);
                }
                else l.until = now + uniform(15.0f, 35.0f);
            }
            break;
        case LinkStep::SwimOut:
            anim = 0; l.y = -30.0f;
            if (swim_toward(kDiveX, kDiveZ) || now > l.until) link_go(l, LinkStep::Dive, now, 2.2);
            break;
        case LinkStep::Dive:
            anim = 2; l.y = -30.0f - 260.0f * float(std::min(1.0, (now - l.since) / 2.2));
            if (now > l.until) link_go(l, LinkStep::Under, now, uniform(1.5f, 4.0f));
            break;
        case LinkStep::Under:
            anim = 1; l.y = -290.0f;
            if (now > l.until) link_go(l, LinkStep::Rise, now, 1.6);
            break;
        case LinkStep::Rise:
            anim = 3; hold = true; items[0] = true;
            l.y = -290.0f + 250.0f * float(std::min(1.0, (now - l.since) / 1.6));
            if (now > l.until) link_go(l, LinkStep::Show, now, 3.5);
            break;
        case LinkStep::Show:
            anim = 5; hold = true; items[0] = true; l.y = -40.0f;
            if (now > l.until) link_go(l, LinkStep::Tread, now, uniform(2.0f, 5.0f));
            break;
        case LinkStep::Tread:
            anim = 1; l.y = -30.0f;
            if (now > l.until) {
                // Back to the platform if he'd rather (and can go unseen);
                // else by the dive spot, down again or back; back where he
                // started, out again.
                const bool at_dive = std::hypot(l.x - kDiveX, l.z - kDiveZ) < 100.0f;
                if (uniform(0.0f, 1.0f) < l.likes_fishing * 0.6f && unseen(l.x, l.z, kFishX, kFishZ)) link_go(l, LinkStep::Fish, now, uniform(20.0f, 45.0f));
                else if (at_dive && uniform(0.0f, 1.0f) < 0.5f) link_go(l, LinkStep::Dive, now, 2.2);
                else if (at_dive) link_go(l, LinkStep::SwimBack, now, 60.0);
                else link_go(l, LinkStep::SwimOut, now, 60.0);
            }
            break;
        case LinkStep::SwimBack:
            anim = 0; l.y = -30.0f;
            if (swim_toward(kSwimFromX, kSwimFromZ) || now > l.until) link_go(l, LinkStep::Tread, now, uniform(3.0f, 8.0f));
            break;
    }
    l.frame += dt * 20.0f;
    // Fishing, still: the stick held out like a cane pole, not waved about
    // with the standing animation's arms (the user, 2026-11-03).
    if (l.step == LinkStep::Fish) l.frame = 0.0f;
    const Mat place = mul(translate(l.x, l.y, l.z), rot_y(l.yaw));
    pose(rdram, p, place, anim, l.frame, hold, items);
}

}  // namespace

// The pack read in (src/guest_course.cpp, once it is installed): its
// models, people and water. `tables` the pack's offsets of them.
void load(const std::vector<uint8_t>& data, uint32_t people, uint32_t people_at, uint32_t models, uint32_t models_at, float water_x0,
          float water_z0, float water_cell, uint32_t water_w, uint32_t water_h, uint32_t water_at, float world_scale) {
    g = People{};
    g_data = &data;
    for (uint32_t i = 0; i < models; ++i) {
        const uint32_t at = models_at + i * 32;
        Model m;
        const uint32_t limbs = read<uint32_t>(data, at), limb_at = read<uint32_t>(data, at + 4);
        m.slots = read<uint32_t>(data, at + 8);
        const uint32_t anims = read<uint32_t>(data, at + 12), anims_at = read<uint32_t>(data, at + 16);
        const uint32_t items = read<uint32_t>(data, at + 20);
        m.scale = read<float>(data, at + 24);
        const uint32_t items_at = read<uint32_t>(data, at + 28);
        for (uint32_t k = 0; k < limbs; ++k) {
            const uint32_t l = limb_at + k * 10;
            m.limbs.push_back({ read<int16_t>(data, l), read<int16_t>(data, l + 2), read<int16_t>(data, l + 4), data[l + 6], data[l + 7],
                                int8_t(data[l + 8]) });
        }
        for (uint32_t k = 0; k < anims; ++k) m.anims.push_back({ read<uint32_t>(data, anims_at + k * 8), read<uint32_t>(data, anims_at + k * 8 + 4) });
        for (uint32_t k = 0; k < items; ++k) m.hands.push_back(read<uint32_t>(data, items_at + k * 4));
        g.models.push_back(m);
    }
    for (uint32_t i = 0; i < people; ++i) {
        const uint32_t at = people_at + i * 28;
        Person p;
        p.model = read<uint32_t>(data, at);
        p.matrices = read<uint32_t>(data, at + 4);
        const uint32_t calls = read<uint32_t>(data, at + 8), calls_at = read<uint32_t>(data, at + 12);
        for (uint32_t k = 0; k < calls; ++k) p.item_calls.push_back(read<uint32_t>(data, calls_at + k * 4));
        p.home_x = read<float>(data, at + 16);
        p.home_z = read<float>(data, at + 20);
        p.home_yaw = read<float>(data, at + 24) * kPi / 32768.0f;
        g.people.push_back(p);
    }
    g.water_x0 = water_x0; g.water_z0 = water_z0; g.water_cell = water_cell;
    g.water_w = water_w; g.water_h = water_h; g.water_at = water_at;
    g.world_scale = world_scale;
    // Each Zora's personality, by turns (the riders' are too).
    g_zoras.clear();
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (const Person& p : g.people) {
        if (g.models[p.model].hands.empty()) {
            Zora z;
            z.x = p.home_x; z.z = p.home_z; z.yaw = p.home_yaw;
            z.wanderlust = u(rng()); z.laziness = u(rng()); z.sociability = u(rng()); z.curiosity = u(rng()); z.shyness = u(rng());
            z.until = 0.0;
            g_zoras.push_back(z);
        }
    }
    g_link = Link{};
    g_link.likes_fishing = 0.4f + 0.4f * u(rng());
    g.ready = !g.models.empty();
    std::fprintf(stderr, "[wr64-people] %zu models, %zu people, water %ux%u\n", g.models.size(), g.people.size(), water_w, water_h);
}

// Once a frame of the ride: the player's jet ski (Wave Race units) and how
// fast it goes.
void frame(uint8_t* rdram, double now, float dt, float player_x, float player_z, float player_speed, bool player_known) {
    if (!g.ready) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    // (Majora's Mask units here.)
    const float px = player_x / g.world_scale, pz = player_z / g.world_scale, pspeed = player_speed / g.world_scale;
    size_t zora = 0;
    for (const Person& p : g.people) {
        if (g.models[p.model].hands.empty()) {
            if (zora < g_zoras.size()) zora_frame(rdram, g_zoras[zora], int(zora), p, now, dt, px, pz, pspeed, player_known);
            ++zora;
        }
        else link_frame(rdram, g_link, p, now, dt, px, pz, player_known);
    }
}

}  // namespace people
}  // namespace guest
}  // namespace wr64

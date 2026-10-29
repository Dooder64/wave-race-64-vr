// The game's bow splash on the thrown rider's body as it skids.
//
// The game draws every splash and spray particle of a frame from one list it
// builds afresh each frame behind segment 5, called from the frame's top
// level as 0x05000000 (measured with WR64_3D_TRACE_VERTS): a setup call, an
// identity matrix, then for each particle its texture, a load of three or
// four vertices already in world space (also behind segment 5), and its
// triangles. It throws them up round a jet ski only while it is ridden, so a
// thrown rider's body has none.
//
// So while the player rides fast, each frame's particles round their jet ski
// are kept -- the commands as they are, the vertices relative to the hull
// (along its bow, across it, up from the water) -- for the last second. While
// the body skids (src/first_person.cpp), those frames are replayed in turn
// round it, turned the way it slides and fading as it slows or leaves the
// water, by a list of this file's own called just after the game's.
//
// The same kept frames are replayed round the fellow rider's jet ski on
// Dolphin Park (src/companions.cpp) as it rides, so it throws up the same
// bow splash -- once the player has ridden fast enough for some to be kept.
//
// Memory: for each frame buffer, at 0x640000 + 0x20000 b (above the water's
// fog, src/first_person.cpp), the list, then from +0x8000 its vertices; the
// companion's at 0x560000 + 0x10000 b, its vertices from +0x4000.
//
// WR64_BODY_SPLASH=0 leaves it out. WR64_SPLASH_LOG=1 reports what is kept.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wr64 {
namespace {

constexpr uint32_t kBase = 0x640000;
constexpr uint32_t kStride = 0x20000;
constexpr uint32_t kVertices = 0x8000;
constexpr uint32_t kMostCommands = kVertices / 8 - 1;
constexpr uint32_t kMostVertices = (kStride - kVertices) / 16;
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };
constexpr uint32_t kCompanionBase = 0x560000, kCompanionStride = 0x10000, kCompanionVertices = 0x4000;

// The trail the game leaves on the water behind a jet ski: not a splash.
constexpr uint32_t kWakeTexture = 0x0104E4C0;
// A particle is the jet ski's when all of it is this near the hull (units).
constexpr float kReach = 200.0f;
// Ridden at least this fast (units a second, about 45 km/h), the jet ski's
// splash is worth keeping; a second's worth is kept.
constexpr float kFastEnough = 400.0f;
constexpr size_t kKept = 30;
constexpr float kFrameTime = 1.0f / 30.0f;

uint32_t word(const uint8_t* rdram, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, rdram + (address & 0x7FFFFF), 4);
    return value;
}
float half(const uint8_t* rdram, uint32_t address) {
    int16_t value;
    std::memcpy(&value, rdram + ((address & 0x7FFFFF) ^ 2), 2);
    return float(value);
}
void put_half(uint8_t* rdram, uint32_t address, float value) {
    const int16_t v = int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
    std::memcpy(rdram + ((address & 0x7FFFFF) ^ 2), &v, 2);
}

struct KeptVertex {
    float along, across, up;
    uint8_t raw[16];
};
struct KeptCommand {
    uint32_t w0, w1;
    bool vertices;          // a vertex load: w1 is the index of its first kept vertex
};
struct KeptFrame {
    std::vector<KeptCommand> commands;
    std::vector<KeptVertex> vertices;
};

struct State {
    // This frame, from the swim (body_splash_note).
    bool noted = false, riding = false, skidding = false;
    float hull_x = 0, hull_z = 0, hull_water = 0, bow_x = 0, bow_z = 1;
    float body_x = 0, body_z = 0, body_water = 0, slide_x = 0, slide_z = 0, wet = 0;
    // The hull a frame ago, for its speed.
    float last_x = 0, last_z = 0;
    bool have_last = false;
    std::vector<KeptFrame> kept;
    size_t next_kept = 0, played = 0;
    float strength = 0;
    // The companion's jet ski this frame.
    bool companion = false;
    float comp_x = 0, comp_z = 0, comp_water = 0, comp_ahead_x = 1, comp_ahead_z = 0, comp_speed = 0, comp_strength = 0;
    size_t comp_played = 0;
} g;

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_BODY_SPLASH");
        return value == nullptr || value[0] != '0';
    }();
    return on;
}

// Keeps this frame's particles round the jet ski from the list at physical
// `list`.
void keep(const uint8_t* rdram, uint32_t list, uint32_t segment5) {
    static const bool log = std::getenv("WR64_SPLASH_LOG") != nullptr;
    const float side_x = -g.bow_z, side_z = g.bow_x;
    KeptFrame frame;
    uint32_t texture = 0;
    bool keeping = false;
    int triangles = 0;
    for (uint32_t pc = list, n = 0; n < 4096; pc += 8, ++n) {
        const uint32_t w0 = word(rdram, pc), w1 = word(rdram, pc + 4);
        const uint8_t op = uint8_t(w0 >> 24);
        if (op == 0xB8) break;
        if (op == 0x06) {
            // Calls of the game's static lists (the setup) are kept; anything
            // behind segment 5 is this frame's alone, and a branch ends it.
            if (((w0 >> 16) & 0xFF) != 0) break;
            if ((w1 >> 24) != 0x05) frame.commands.push_back({ w0, w1, false });
            continue;
        }
        if (op == 0xFD) texture = w1;
        if (op == 0x04) {
            const uint32_t count = (w0 >> 9) & 0x7F;
            uint32_t at;
            if ((w1 >> 24) == 0x05) at = segment5 + (w1 & 0xFFFFFF);
            else if (w1 & 0x80000000u) at = w1 & 0x7FFFFF;
            else { keeping = false; continue; }
            keeping = count > 0 && count <= 32 && texture != kWakeTexture;
            for (uint32_t i = 0; keeping && i < count; ++i) {
                const uint32_t v = at + i * 16;
                keeping = std::hypot(half(rdram, v) - g.hull_x, half(rdram, v + 4) - g.hull_z) < kReach;
            }
            if (!keeping) continue;
            frame.commands.push_back({ w0, uint32_t(frame.vertices.size()), true });
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t v = at + i * 16;
                const float dx = half(rdram, v) - g.hull_x, dz = half(rdram, v + 4) - g.hull_z;
                KeptVertex k{ dx * g.bow_x + dz * g.bow_z, dx * side_x + dz * side_z, half(rdram, v + 2) - g.hull_water, {} };
                std::memcpy(k.raw, rdram + v, 16);
                frame.vertices.push_back(k);
            }
            continue;
        }
        // Triangles of a particle left out are left out with it.
        if (op == 0xBF || op == 0xB1 || op == 0xB5) {
            if (!keeping) continue;
            ++triangles;
        }
        frame.commands.push_back({ w0, w1, false });
    }
    if (triangles == 0) return;
    if (log) {
        static int reported = 0;
        if (reported++ % 30 == 0) {
            std::fprintf(stderr, "[wr64-splash] kept %d triangles, %zu vertices, %zu commands\n",
                         triangles, frame.vertices.size(), frame.commands.size());
            std::fflush(stderr);
        }
    }
    if (g.kept.size() < kKept) g.kept.push_back(std::move(frame));
    else g.kept[g.next_kept] = std::move(frame);
    g.next_kept = (g.next_kept + 1) % kKept;
}

// Writes kept frame `frame` round (x, z), the water `water` high there,
// facing (ahead_x, ahead_z), faded by `fade`, as a list at `list` with its
// vertices at `vertices` (room for `most_vertices` and `most_commands`);
// returns the list's address, or 0.
uint32_t replay_at(uint8_t* rdram, const KeptFrame& frame, float x, float z, float water, float ahead_x, float ahead_z,
                   float fade, uint32_t list, uint32_t vertices, uint32_t most_vertices, uint32_t most_commands) {
    const float side_x = -ahead_z, side_z = ahead_x;
    if (frame.vertices.size() > most_vertices || frame.commands.size() > most_commands) return 0;
    for (size_t i = 0; i < frame.vertices.size(); ++i) {
        const KeptVertex& k = frame.vertices[i];
        const uint32_t v = vertices + uint32_t(i) * 16;
        std::memcpy(rdram + v, k.raw, 16);
        put_half(rdram, v, x + ahead_x * k.along + side_x * k.across);
        put_half(rdram, v + 2, water + k.up);
        put_half(rdram, v + 4, z + ahead_z * k.along + side_z * k.across);
        uint8_t& alpha = rdram[(v + 15) ^ 3];
        alpha = uint8_t(std::lround(float(alpha) * fade));
    }
    uint32_t at = list;
    auto command = [&](uint32_t w0, uint32_t w1) {
        reinterpret_cast<uint32_t*>(rdram + at)[0] = w0;
        reinterpret_cast<uint32_t*>(rdram + at)[1] = w1;
        at += 8;
    };
    for (const KeptCommand& c : frame.commands) {
        command(c.w0, c.vertices ? 0x80000000u | (vertices + c.w1 * 16) : c.w1);
    }
    command(0xB8000000u, 0);
    return 0x80000000u | list;
}

// Writes the next kept frame round the body for frame buffer `b`; returns
// the list's address, or 0.
uint32_t replay(uint8_t* rdram, int b) {
    if (g.kept.empty()) return 0;
    const float speed = std::hypot(g.slide_x, g.slide_z);
    // Fading in as the body touches the water, out as it skips up or slows.
    g.strength += ((g.wet > 0.0f ? 1.0f : 0.0f) - g.strength) * (g.wet > 0.0f ? 0.5f : 0.15f);
    const float fade = g.strength * std::clamp((speed - 35.0f) / 400.0f, 0.0f, 1.0f);
    if (fade < 0.02f) return 0;
    const float ahead_x = speed > 1.0f ? g.slide_x / speed : g.bow_x, ahead_z = speed > 1.0f ? g.slide_z / speed : g.bow_z;
    const KeptFrame& frame = g.kept[g.played++ % g.kept.size()];
    const uint32_t list = kBase + uint32_t(b) * kStride;
    return replay_at(rdram, frame, g.body_x, g.body_z, g.body_water, ahead_x, ahead_z, fade, list, list + kVertices,
                     kMostVertices, kMostCommands);
}

}  // namespace

// From src/companions.cpp, each frame it is drawn: its jet ski, whether on
// the water (not flying off the ramp), where, the water's height there, which
// way it heads, and how fast.
void body_splash_companion(bool on_water, float x, float z, float water, float ahead_x, float ahead_z, float speed) {
    g.companion = true;
    g.comp_strength += ((on_water ? 1.0f : 0.0f) - g.comp_strength) * (on_water ? 0.4f : 0.3f);
    g.comp_x = x; g.comp_z = z; g.comp_water = water;
    g.comp_ahead_x = ahead_x; g.comp_ahead_z = ahead_z; g.comp_speed = speed;
}

// For the display-list rewriter, just after the frame's call of the game's
// particles: the address of a list drawing the bow splash round the
// companion's jet ski, or 0. `segment3` is the frame's.
uint32_t body_splash_companion_list(uint8_t* rdram, uint32_t segment3) {
    if (!enabled() || !g.companion || g.kept.empty()) return 0;
    g.companion = false;
    int b = -1;
    for (int i = 0; i < 2; ++i) {
        if (segment3 == kFrameBuffers[i]) b = i;
    }
    if (b < 0) return 0;
    const float fade = g.comp_strength * std::clamp((g.comp_speed - 100.0f) / 500.0f, 0.0f, 1.0f);
    if (fade < 0.02f) return 0;
    const KeptFrame& frame = g.kept[g.comp_played++ % g.kept.size()];
    const uint32_t list = kCompanionBase + uint32_t(b) * kCompanionStride;
    return replay_at(rdram, frame, g.comp_x, g.comp_z, g.comp_water, g.comp_ahead_x, g.comp_ahead_z, fade, list,
                     list + kCompanionVertices, (kCompanionStride - kCompanionVertices) / 16, kCompanionVertices / 8 - 1);
}

// From the swim (src/first_person.cpp), each first-person race frame before
// the game's particles are drawn: whether the player rides (and where the
// hull is, the water's height there and which way the bow faces) and
// whether the body skids (where, the water's height there, its velocity
// across the water, and whether it touches the water: 1, or not: 0).
void body_splash_note(bool riding, float hull_x, float hull_z, float hull_water, float bow_x, float bow_z,
                      bool skidding, float body_x, float body_z, float body_water, float slide_x, float slide_z, float wet) {
    const float bow = std::hypot(bow_x, bow_z);
    g.noted = true;
    g.riding = riding;
    g.hull_x = hull_x; g.hull_z = hull_z; g.hull_water = hull_water;
    if (bow > 1e-4f) { g.bow_x = bow_x / bow; g.bow_z = bow_z / bow; }
    g.skidding = skidding;
    g.body_x = body_x; g.body_z = body_z; g.body_water = body_water;
    g.slide_x = slide_x; g.slide_z = slide_z; g.wet = wet;
    if (!skidding) g.strength = 0;
}

// For the display-list rewriter, just after the frame's call of the game's
// particles (the list at physical `list`): the address of a list to call
// next, drawing the bow splash round the skidding body, or 0. `segment5` and
// `segment3` are the frame's.
uint32_t body_splash_list(uint8_t* rdram, uint32_t list, uint32_t segment5, uint32_t segment3) {
    if (!enabled() || !g.noted) return 0;
    g.noted = false;
    int b = -1;
    for (int i = 0; i < 2; ++i) {
        if (segment3 == kFrameBuffers[i]) b = i;
    }
    const float speed = g.have_last ? std::hypot(g.hull_x - g.last_x, g.hull_z - g.last_z) / kFrameTime : 0.0f;
    g.last_x = g.hull_x; g.last_z = g.hull_z; g.have_last = true;
    if (g.riding && speed > kFastEnough && speed < 4000.0f) keep(rdram, list, segment5);
    if (g.skidding && b >= 0) return replay(rdram, b);
    return 0;
}

}  // namespace wr64

// VR: the sky all round the camera.
//
// What a race draws as its sky is three fans of six triangles, rebuilt every
// frame into the structure segment 6 points at (func_8004C998 writes the
// first two, func_8004C1D0 the third), and all three are placed on the patch
// of water the game builds its waves on, about 1300 units ahead of the camera
// -- not on the camera (measured with WR64_WATER_DUMP):
//
//   +0x000  the water's floor: a flat textured hexagon at the water level,
//           1500 units from its centre to a corner, that shows through the
//           translucent waves (list 0x010103F0);
//   +0x0E0  the sky's gradient: a tent from a point high above and off to one
//           side down to the same hexagon, its texture a strip running from
//           the horizon at the bottom to the top (loaded by the frame's own
//           list, drawn by list 0x010103B8);
//   +0x1C0  the clouds: a hexagon at their height, 3000 units to a corner,
//           fading with distance (list 0x01010470).
//
// So behind the camera there are no clouds, no floor under the waves, and the
// gradient is squeezed into the few hundred units between the camera and the
// tent's edge. Turning the camera does not move them, so they are not built
// again the way the waves are (patches/water_all_round.cpp): they are
// replaced, each frame, by the same three things centred on the camera, drawn
// with the game's own textures and state by lists of their own (the rewriter
// points segment 6 at them for the call). What the game's own carry is kept:
// the clouds' and the floor's texture coordinates, which are a linear
// function of position (the clouds' scroll), are measured from the game's
// vertices each frame and carried on. The gradient and the clouds' fade are
// taken as they are straight ahead -- for the gradient, how far away the
// tent is and which part of the gradient it shows, elevation by elevation;
// for the clouds, how their alpha falls off with distance and how far they
// reach -- and swept all the way round the camera.
//
// Triangles face the eye the way the game's do, since faces turned away are
// not drawn: a hole in the gradient is not black but the last frame left
// behind, which the translucent clouds then pile up on.
//
// Only when the waves are merged all round for the same frame, whose camera
// and hexagon these use. The structure itself is only read: the game reads
// parts of it back (the tent's peak's t, at least). WR64_SKY_KEEP=1 leaves the
// game's sky alone, and WR64_SKY_PARTS picks which parts are replaced, for
// comparison.

#include "recomp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace wr64 {

bool water_built_for(uint32_t segment3, float eye[3], float* centre_x, float* centre_z, float* radius);

// The lists drawing the sky centred on the camera, through segment 6, and
// the segment base they expect.
struct SkyLists { uint32_t base, floor, gradient, clouds; };

namespace {

// The game's two frame buffers, as in patches/water_all_round.cpp.
constexpr uint32_t kFrameBuffers[2] = { 0x11F8E8, 0x1388D0 };

// Each frame buffer's sky, vertices first and then the three lists, in
// physical memory after the merged waves' and before the audio task's copy
// at 0x7E0000.
constexpr uint32_t kSky = 0x7C1000;
constexpr uint32_t kSkyStride = 0x6000;
constexpr uint32_t kSkyVertices = 0x3000 / 16;
constexpr uint32_t kFloorList = 0x3000, kGradientList = 0x3200, kCloudList = 0x4200, kListsEnd = 0x6000;

// The clouds and the gradient go round the camera in kSegments directions;
// the clouds as far as the game's reach straight ahead, or kCloudFar.
constexpr int kSegments = 16;
constexpr float kCloudFar = 6000.0f;

constexpr float kPi = 3.14159265f;

struct Vertex {
    float x, y, z, s, t;
    uint32_t rgba;
};

Vertex read_vertex(const uint8_t* rdram, uint32_t address) {
    auto half = [&](uint32_t offset) {
        return float(*reinterpret_cast<const int16_t*>(rdram + ((address + offset) ^ 2)));
    };
    return { half(0), half(2), half(4), half(8), half(10),
             *reinterpret_cast<const uint32_t*>(rdram + address + 12) };
}

int16_t to_half(float value) {
    return int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
}

void write_vertex(uint8_t* rdram, uint32_t address, const Vertex& p) {
    auto put = [&](uint32_t offset, float value) {
        *reinterpret_cast<int16_t*>(rdram + ((address + offset) ^ 2)) = to_half(value);
    };
    put(0, p.x); put(2, p.y); put(4, p.z); put(6, 0.0f); put(8, p.s); put(10, p.t);
    *reinterpret_cast<uint32_t*>(rdram + address + 12) = p.rgba;
}

uint32_t alpha_of(uint32_t rgba) { return rgba & 0xFF; }

// A texture coordinate as a linear function of position on the water's
// plane: value = a + b * x + c * z.
struct Plane {
    double a = 0, b = 0, c = 0;
    float at(float x, float z) const { return float(a + b * x + c * z); }
};

// The least-squares plane through seven vertices' s (which = 0) or t (1).
// False when the points do not span the plane.
bool fit(const std::array<Vertex, 7>& v, int which, Plane& out) {
    double mx = 0, mz = 0;
    for (const Vertex& p : v) { mx += p.x; mz += p.z; }
    mx /= 7.0; mz /= 7.0;
    double sxx = 0, sxz = 0, szz = 0, sv = 0, sxv = 0, szv = 0;
    for (const Vertex& p : v) {
        const double x = p.x - mx, z = p.z - mz, value = which == 0 ? p.s : p.t;
        sxx += x * x; sxz += x * z; szz += z * z;
        sv += value; sxv += x * value; szv += z * value;
    }
    const double det = sxx * szz - sxz * sxz;
    if (std::fabs(det) < 1e-6 * (sxx * szz + 1.0)) return false;
    out.b = (sxv * szz - szv * sxz) / det;
    out.c = (szv * sxx - sxv * sxz) / det;
    out.a = sv / 7.0 - out.b * mx - out.c * mz;
    return true;
}

// The plane moved by a whole number of texture repeats so that it is near
// zero at (x, z), which keeps coordinates far from it inside a half. Textures
// repeat every power of two texels and a texel is 32 units, so 4096 is a
// repeat of anything up to 128 texels across.
void recentre(Plane& plane, float x, float z) {
    plane.a -= std::round(plane.at(x, z) / 4096.0) * 4096.0;
}

// Which way a triangle faces the eye: positive one way round as the eye sees
// it, negative the other.
float facing(const Vertex& a, const Vertex& b, const Vertex& c, const float eye[3]) {
    const float e1[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    const float e2[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
    const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    return n[0] * (eye[0] - a.x) + n[1] * (eye[1] - a.y) + n[2] * (eye[2] - a.z);
}

// Where a ray from `o` along `d` meets triangle (a, b, c): its distance, and
// the weights of b and c there. False when it misses.
bool hit(const float o[3], const float d[3], const Vertex& a, const Vertex& b, const Vertex& c,
         float& distance, float& wb, float& wc) {
    const float e1[3] = { b.x - a.x, b.y - a.y, b.z - a.z };
    const float e2[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
    const float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-6f) return false;
    const float inv = 1.0f / det;
    const float s[3] = { o[0] - a.x, o[1] - a.y, o[2] - a.z };
    wb = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (wb < 0.0f || wb > 1.0f) return false;
    const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
    wc = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (wc < 0.0f || wb + wc > 1.0f) return false;
    distance = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return distance > 0.0f;
}

// Writes vertices and a list drawing triangles of them, in batches of at most
// the forty-eight vertices the microcode can name (see water_all_round.cpp),
// each loaded, through segment 6, before its triangles are drawn; then the
// end. Each triangle is turned to face the eye the way `reference` -- the
// game's own -- does.
class Writer {
public:
    Writer(uint8_t* rdram, uint32_t base, const float eye[3]) : rdram_(rdram), base_(base), eye_(eye) {}

    bool list(uint32_t list_offset, uint32_t list_end, const std::vector<Vertex>& vertices,
              std::vector<std::array<uint32_t, 3>> triangles, float reference) {
        uint32_t* out = reinterpret_cast<uint32_t*>(rdram_ + base_ + list_offset);
        const uint32_t capacity = (list_end - list_offset) / 8;
        uint32_t commands = 0;
        auto command = [&](uint32_t w0, uint32_t w1) {
            if (commands >= capacity) return false;
            out[commands * 2] = w0;
            out[commands * 2 + 1] = w1;
            ++commands;
            return true;
        };
        std::vector<uint32_t> batch;
        std::vector<std::array<uint32_t, 3>> drawn;
        auto flush = [&]() {
            if (batch.empty()) return true;
            const uint32_t n = uint32_t(batch.size());
            if (next_ + n > kSkyVertices) return false;
            const uint32_t address = 0x06000000u | (next_ * 16);
            for (uint32_t i : batch) write_vertex(rdram_, base_ + next_++ * 16, vertices[i]);
            if (!command((0x04u << 24) | (n << 9) | (n * 16 - 1), address)) return false;
            for (const auto& tri : drawn) {
                if (!command(0xBFu << 24, ((tri[0] * 5) << 16) | ((tri[1] * 5) << 8) | (tri[2] * 5))) return false;
            }
            batch.clear();
            drawn.clear();
            return true;
        };
        for (auto& tri : triangles) {
            if (facing(vertices[tri[0]], vertices[tri[1]], vertices[tri[2]], eye_) * reference < 0.0f) {
                std::swap(tri[1], tri[2]);
            }
            uint32_t needed = 0;
            for (uint32_t i : tri) {
                if (std::find(batch.begin(), batch.end(), i) == batch.end()) ++needed;
            }
            if (batch.size() + needed > 48 && !flush()) return false;
            std::array<uint32_t, 3> entries{};
            for (int j = 0; j < 3; ++j) {
                auto it = std::find(batch.begin(), batch.end(), tri[j]);
                if (it == batch.end()) {
                    batch.push_back(tri[j]);
                    it = batch.end() - 1;
                }
                entries[j] = uint32_t(it - batch.begin());
            }
            drawn.push_back(entries);
        }
        return flush() && command(0xB8u << 24, 0);
    }

private:
    uint8_t* rdram_;
    uint32_t base_;
    const float* eye_;
    uint32_t next_ = 0;
};

// The gradient's profile straight ahead: where the game's tent meets the
// water ahead, then, for each of a run of elevations from the eye, how far
// away the tent is in that direction and the texture coordinate t it has
// there, all cast towards the tent's centre. Empty when the tent cannot be
// read that way.
struct ProfilePoint { float horizontal, height, t; };

std::vector<ProfilePoint> tent_profile(const std::array<Vertex, 7>& tent, const float eye[3]) {
    std::vector<ProfilePoint> profile;
    float ahead_x = 0.0f, ahead_z = 0.0f;
    for (int i = 0; i < 6; ++i) { ahead_x += tent[i].x / 6.0f; ahead_z += tent[i].z / 6.0f; }
    ahead_x -= eye[0];
    ahead_z -= eye[2];
    const float ahead = std::sqrt(ahead_x * ahead_x + ahead_z * ahead_z);
    if (ahead < 1.0f || tent[6].t == tent[0].t) return profile;
    ahead_x /= ahead;
    ahead_z /= ahead;

    // Where the ray along the water ahead leaves the tent's base.
    float base = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const Vertex &p = tent[i], &q = tent[(i + 1) % 6];
        const float ex = q.x - p.x, ez = q.z - p.z;
        const float det = ex * ahead_z - ez * ahead_x;
        if (std::fabs(det) < 1e-6f) continue;
        const float rx = p.x - eye[0], rz = p.z - eye[2];
        const float u = (ex * rz - ez * rx) / det, w = (ahead_x * rz - ahead_z * rx) / det;
        if (w >= 0.0f && w <= 1.0f && u > base) base = u;
    }
    if (base < 100.0f) return profile;
    profile.push_back({ base, 0.0f, tent[0].t });

    static constexpr float kElevations[] = { 0, 2, 5, 10, 15, 20, 30, 45, 60, 75 };
    for (const float degrees : kElevations) {
        const float e = degrees * kPi / 180.0f;
        const float d[3] = { std::cos(e) * ahead_x, std::sin(e), std::cos(e) * ahead_z };
        float best = 1e30f, t = 0.0f;
        for (int i = 0; i < 6; ++i) {
            const Vertex &p = tent[i], &q = tent[(i + 1) % 6], &peak = tent[6];
            float distance, wq, wpeak;
            if (hit(eye, d, p, q, peak, distance, wq, wpeak) && distance < best) {
                best = distance;
                t = p.t * (1.0f - wq - wpeak) + q.t * wq + peak.t * wpeak;
            }
        }
        if (best > 1e29f) break;
        profile.push_back({ best * std::cos(e), eye[1] + best * std::sin(e), t });
    }
    if (profile.size() < 3) profile.clear();
    return profile;
}

// The clouds' fade straight ahead. The game gives each of its seven vertices
// an alpha and its triangles spread it between them, so the fade is the
// fan's own: it is read along the ray from the eye towards the hexagon's
// centre, at each of kCloudRings distances out to where the ray leaves the
// hexagon (or kCloudFar), and swept round the camera like the gradient. The
// first ring is the eye itself. Empty when the hexagon cannot be read so.
struct CloudRing { float distance; uint32_t alpha; };

std::vector<CloudRing> cloud_profile(const std::array<Vertex, 7>& clouds, const float eye[3]) {
    std::vector<CloudRing> rings;
    float cx = 0.0f, cz = 0.0f;
    for (int i = 1; i < 7; ++i) { cx += clouds[i].x / 6.0f; cz += clouds[i].z / 6.0f; }
    float dx = cx - eye[0], dz = cz - eye[2];
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length < 1.0f) return rings;
    dx /= length;
    dz /= length;

    // Where the ray leaves the hexagon of vertices 1 to 6.
    float reach = 0.0f;
    for (int i = 1; i < 7; ++i) {
        const Vertex &p = clouds[i], &q = clouds[i == 6 ? 1 : i + 1];
        // eye + u * d = p + w * (q - p)
        const float ex = q.x - p.x, ez = q.z - p.z;
        const float det = ex * dz - ez * dx;
        if (std::fabs(det) < 1e-6f) continue;
        const float rx = p.x - eye[0], rz = p.z - eye[2];
        const float u = (ex * rz - ez * rx) / det, w = (dx * rz - dz * rx) / det;
        if (w >= 0.0f && w <= 1.0f && u > reach) reach = u;
    }
    if (reach < 1000.0f) return rings;
    reach = std::min(reach, kCloudFar);

    // The fan's alpha at a distance along the ray, from the triangle of it
    // the point lies in; unchanged where it lies in none.
    auto alpha_at = [&](float d, uint32_t& alpha) {
        const float x = eye[0] + dx * d, z = eye[2] + dz * d;
        const Vertex& a = clouds[0];
        for (int i = 1; i < 7; ++i) {
            const Vertex &b = clouds[i], &c = clouds[i == 6 ? 1 : i + 1];
            const float det = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
            if (std::fabs(det) < 1e-6f) continue;
            const float wb = ((x - a.x) * (c.z - a.z) - (z - a.z) * (c.x - a.x)) / det;
            const float wc = ((b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x)) / det;
            if (wb < -1e-4f || wc < -1e-4f || wb + wc > 1.0001f) continue;
            const float value = float(alpha_of(a.rgba)) * (1.0f - wb - wc) + float(alpha_of(b.rgba)) * wb +
                                float(alpha_of(c.rgba)) * wc;
            alpha = uint32_t(std::clamp(value, 0.0f, 255.0f) + 0.5f);
            return;
        }
    };
    static constexpr float kCloudRings[] = { 0, 400, 800, 1200, 1600, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 5500 };
    uint32_t alpha = 255;
    for (const float d : kCloudRings) {
        if (d > reach - 200.0f) break;
        alpha_at(d, alpha);
        rings.push_back({ d, alpha });
    }
    alpha_at(reach - 1.0f, alpha);
    rings.push_back({ reach, alpha });
    return rings;
}

// Triangles joining rings of kSegments vertices, ring r's i-th at
// first + r * kSegments + i, each to the next.
void join_rings(std::vector<std::array<uint32_t, 3>>& tris, uint32_t first, uint32_t rings) {
    for (uint32_t r = 0; r + 1 < rings; ++r) {
        for (uint32_t i = 0; i < uint32_t(kSegments); ++i) {
            const uint32_t j = (i + 1) % kSegments;
            const uint32_t a = first + r * kSegments + i, b = first + r * kSegments + j;
            const uint32_t c = first + (r + 1) * kSegments + i, d = first + (r + 1) * kSegments + j;
            tris.push_back({ a, b, d });
            tris.push_back({ a, d, c });
        }
    }
}

// A fan from vertex `centre` to the ring of kSegments vertices from `first`.
void fan(std::vector<std::array<uint32_t, 3>>& tris, uint32_t centre, uint32_t first) {
    for (uint32_t i = 0; i < uint32_t(kSegments); ++i) {
        tris.push_back({ centre, first + i, first + (i + 1) % kSegments });
    }
}

}  // namespace

bool sky_all_round(uint8_t* rdram, uint32_t segment3, uint32_t segment6, SkyLists* lists) {
    static const bool keep = std::getenv("WR64_SKY_KEEP") != nullptr;
    if (keep || segment6 == 0) return false;
    int b = -1;
    for (int i = 0; i < 2; ++i) {
        if (segment3 == kFrameBuffers[i]) b = i;
    }
    float eye[3], centre_x, centre_z, radius;
    if (b < 0 || !water_built_for(segment3, eye, &centre_x, &centre_z, &radius)) return false;

    const uint32_t structure = segment6 & 0x00FFFFFFu;
    std::array<Vertex, 7> floor, tent, clouds;
    for (uint32_t i = 0; i < 7; ++i) {
        floor[i] = read_vertex(rdram, structure + 0x000 + i * 16);
        tent[i] = read_vertex(rdram, structure + 0x0E0 + i * 16);
        clouds[i] = read_vertex(rdram, structure + 0x1C0 + i * 16);
    }
    // What was measured: the floor and the tent's base at the water level,
    // the tent's peak and the clouds above the eye, the clouds level.
    // Anything else is not the sky this was written for.
    const float cloud_height = clouds[0].y;
    for (int i = 0; i < 7; ++i) {
        if (floor[i].y != 0.0f || clouds[i].y != cloud_height || (i < 6 && tent[i].y != 0.0f)) return false;
    }
    if (tent[6].y <= eye[1] || cloud_height <= eye[1]) return false;

    Plane floor_s, floor_t, cloud_s, cloud_t;
    if (!fit(floor, 0, floor_s) || !fit(floor, 1, floor_t) || !fit(clouds, 0, cloud_s) || !fit(clouds, 1, cloud_t)) {
        return false;
    }
    const std::vector<ProfilePoint> profile = tent_profile(tent, eye);
    const std::vector<CloudRing> cloud_rings = cloud_profile(clouds, eye);
    if (profile.empty() || cloud_rings.size() < 2) return false;

    const uint32_t base = kSky + uint32_t(b) * kSkyStride;
    Writer writer(rdram, base, eye);
    std::vector<Vertex> v;
    std::vector<std::array<uint32_t, 3>> tris;

    // The floor: a hexagon over the merged waves' own, fanned from its centre.
    recentre(floor_s, centre_x, centre_z);
    recentre(floor_t, centre_x, centre_z);
    v.push_back({ centre_x, 0.0f, centre_z, floor_s.at(centre_x, centre_z), floor_t.at(centre_x, centre_z), floor[1].rgba });
    for (int c = 0; c < 6; ++c) {
        const float x = centre_x + radius * std::cos(float(c) * kPi / 3.0f);
        const float z = centre_z + radius * std::sin(float(c) * kPi / 3.0f);
        v.push_back({ x, 0.0f, z, floor_s.at(x, z), floor_t.at(x, z), floor[1].rgba });
    }
    for (uint32_t c = 0; c < 6; ++c) tris.push_back({ 0, 1 + c, 1 + (c + 1) % 6 });
    if (!writer.list(kFloorList, kGradientList, v, tris, facing(floor[0], floor[1], floor[2], eye))) return false;

    // The gradient: the game's tent as it is straight ahead, swept round the
    // camera into rings, and closed over the top at the peak's t.
    v.clear();
    tris.clear();
    for (const ProfilePoint& ring : profile) {
        for (int i = 0; i < kSegments; ++i) {
            const float angle = 2.0f * kPi * float(i) / float(kSegments);
            v.push_back({ eye[0] + ring.horizontal * std::cos(angle), ring.height,
                          eye[2] + ring.horizontal * std::sin(angle), tent[0].s, ring.t, tent[0].rgba });
        }
    }
    const ProfilePoint& top = profile.back();
    v.push_back({ eye[0], top.height + top.horizontal * 0.3f, eye[2], tent[6].s, tent[6].t, tent[6].rgba });
    const uint32_t rings = uint32_t(profile.size());
    join_rings(tris, 0, rings);
    fan(tris, uint32_t(v.size() - 1), (rings - 1) * kSegments);
    if (!writer.list(kGradientList, kCloudList, v, tris, facing(tent[0], tent[1], tent[6], eye))) return false;

    // The clouds: a fan from the eye and bands out to the distance, with the
    // game's fade (cloud_profile) and texture coordinates.
    recentre(cloud_s, eye[0], eye[2]);
    recentre(cloud_t, eye[0], eye[2]);
    const uint32_t rgb = clouds[0].rgba & 0xFFFFFF00u;
    v.clear();
    tris.clear();
    v.push_back({ eye[0], cloud_height, eye[2], cloud_s.at(eye[0], eye[2]), cloud_t.at(eye[0], eye[2]),
                  rgb | cloud_rings[0].alpha });
    for (size_t r = 1; r < cloud_rings.size(); ++r) {
        for (int i = 0; i < kSegments; ++i) {
            const float angle = 2.0f * kPi * float(i) / float(kSegments);
            const float x = eye[0] + cloud_rings[r].distance * std::cos(angle);
            const float z = eye[2] + cloud_rings[r].distance * std::sin(angle);
            v.push_back({ x, cloud_height, z, cloud_s.at(x, z), cloud_t.at(x, z), rgb | cloud_rings[r].alpha });
        }
    }
    fan(tris, 0, 1);
    join_rings(tris, 1, uint32_t(cloud_rings.size() - 1));
    if (!writer.list(kCloudList, kListsEnd, v, tris, facing(clouds[0], clouds[1], clouds[2], eye))) return false;

    // WR64_SKY_PARTS: 1 the floor, 2 the gradient, 4 the clouds; all three
    // unless set.
    static const char* parts_env = std::getenv("WR64_SKY_PARTS");
    static const long parts = parts_env != nullptr ? std::strtol(parts_env, nullptr, 0) : 7;
    lists->base = base;
    lists->floor = (parts & 1) ? 0x06000000u | kFloorList : 0;
    lists->gradient = (parts & 2) ? 0x06000000u | kGradientList : 0;
    lists->clouds = (parts & 4) ? 0x06000000u | kCloudList : 0;

    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr, "[wr64-sky] the sky is drawn round the camera: floor %.0f units to a corner;"
                             " gradient in %u rings from %.0f units; clouds at %.0f out to %.0f, alpha %u there\n",
                     radius, rings, profile.front().horizontal, cloud_height,
                     cloud_rings.back().distance, cloud_rings.back().alpha);
        std::fflush(stderr);
    }
    return true;
}

}  // namespace wr64

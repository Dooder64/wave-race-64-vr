// The back halves of courses' land.
//
// The game models its land only as far as a race shows it: Sunny Beach's
// mainland is a beach rising to a hill whose model stops at the hilltop, and
// its triangles face the course alone, so from behind -- reachable in Free
// Roam -- the hill is not there at all, and the sandbar shows through it.
//
// Here each such hilltop (a "rim": the open edges of the course's land,
// those used by one triangle only, high enough to be a hill's top) carries
// on into a back slope: rows stepped out from the rim, away from the land
// (the way out at each point, kept with it), the ground falling smoothly
// from the rim's height to below the water over a distance by its height,
// level at the top and at the bottom. It wears the hill's own texture as
// the hill does -- the texture's top at the rim, its foot at the waterline,
// and repeating along the rim at the hill's rate -- and is drawn with the
// course's water, opaque, unlit, shaded by its slope -- before the water, as
// the seafloor is not: the sky is drawn after the seafloor, over anything
// standing above the horizon.
//
// The rims come from the course's triangles read out of a frame's memory
// (scratchpad tools dl_scan.py, rim_chains.py).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace wr64 {

namespace {

// A point of a rim: where, and the way out from the land there.
struct RimPoint { float x, y, z, out_x, out_z; };

// Sunny Beach (course 1): the mainland's hilltop, from the far west round
// the south of the course to the north-east, 28 points, 37000 units; and at
// either end, where the mainland is cut off, down the cut to the beach (both
// ends are reached following the shore), the slope going on past the end.
constexpr RimPoint kSunnyRim[] = {
    { -15200, -50, 3400, -0.581f, 0.814f }, { -14549, 50, 3865, -0.581f, 0.814f },
    { -14097, 650, 4188, 0.854f, 0.520f },  { -13319, 650, 2910, 0.722f, 0.692f },  { -12524, 563, 2379, 0.404f, 0.915f },
    { -10522, 485, 1885, 0.184f, 0.983f },  { -9132, 740, 1706, 0.126f, 0.992f },   { -8505, 1043, 1627, 0.280f, 0.960f },
    { -8315, 1073, 1537, 0.010f, 1.000f },  { -8208, 1073, 1585, -0.484f, 0.875f }, { -8136, 1048, 1633, -0.772f, 0.636f },
    { -7831, 997, 2364, -0.713f, 0.701f },  { -6235, 1090, 3064, -0.339f, 0.941f }, { -4418, 826, 3584, 0.022f, 1.000f },
    { -2654, 873, 2993, 0.099f, 0.995f },   { -1061, 809, 3194, -0.276f, 0.961f },  { 628, 443, 3975, -0.267f, 0.964f },
    { 2616, 516, 4191, 0.001f, 1.000f },    { 4590, 650, 3972, 0.222f, 0.975f },    { 6424, 901, 3331, 0.471f, 0.882f },
    { 8005, 1118, 2144, 0.803f, 0.596f },   { 8550, 1077, 652, 0.990f, 0.139f },    { 8426, 682, -1086, 0.976f, -0.219f },
    { 8077, 934, -1986, 0.932f, 0.364f },   { 8474, 955, -2181, 0.321f, 0.947f },   { 9332, 902, -2352, 0.078f, 0.997f },
    { 10718, 650, -2297, -0.338f, 0.941f }, { 11783, 463, -1488, -0.517f, 0.856f }, { 13565, 490, -656, -0.261f, 0.965f },
    { 15600, 650, -468, -0.092f, 0.996f },
    { 15600, 50, -1000, 1.0f, 0.0f },       { 15600, -50, -1800, 1.0f, 0.0f },
};

// Glacier Coast (course 8): the cliffs round the bay, a thin wall seen from
// both sides with the sea behind; and the long island the course goes round,
// its two halves' rims along the canyon between their walls.
constexpr RimPoint kGlacierCliffs[] = {
    // (Each arm's far end, reached following the shore, tapered away below
    // the water past the end of its wall.)
    { -9523, -60, -12817, 0.866f, -0.500f },
    { -9223, 240, -12297, 0.866f, -0.500f }, { -8998, 240, -11907, 0.866f, -0.500f }, { -8773, 240, -11517, 0.824f, -0.567f },
    { -8486, 240, -11163, 0.824f, -0.567f }, { -8261, 240, -10773, 0.866f, -0.500f }, { -8036, 200, -10384, 0.876f, -0.482f },
    { -7804, 200, -9940, 0.928f, -0.372f }, { -7671, 240, -9480, 0.963f, -0.268f }, { -7555, 240, -9045, 0.983f, -0.182f },
    { -7507, 240, -8593, 0.966f, -0.259f }, { -7329, 200, -8193, 0.935f, -0.354f }, { -7140, 220, -7594, 0.988f, -0.152f },
    { -7140, 220, -7144, 0.997f, -0.071f }, { -7090, 240, -6794, 0.877f, -0.480f }, { -6737, 240, -6490, 0.707f, -0.707f },
    { -6433, 220, -6137, 0.733f, -0.680f }, { -6079, 220, -5783, 0.541f, -0.841f }, { -5690, 240, -5638, 0.228f, -0.974f },
    { -5250, 190, -5592, 0.189f, -0.982f }, { -4810, 240, -5467, 0.210f, -0.978f }, { -4370, 190, -5402, 0.230f, -0.973f },
    { -3930, 240, -5257, 0.355f, -0.935f }, { -3517, 240, -5079, 0.394f, -0.919f }, { -3103, 240, -4902, 0.345f, -0.939f },
    { -2690, 190, -4774, 0.320f, -0.947f }, { -2254, 240, -4614, 0.294f, -0.956f }, { -1963, 240, -4541, 0.175f, -0.985f },
    { -1513, 230, -4493, 0.106f, -0.994f }, { -1063, 240, -4445, -0.004f, -1.000f }, { -613, 230, -4497, -0.018f, -1.000f },
    { -13, 240, -4450, -0.272f, -0.962f }, { 351, 240, -4715, -0.621f, -0.783f }, { 715, 190, -5029, -0.621f, -0.783f },
    { 1079, 190, -5294, -0.555f, -0.832f }, { 1500, 240, -5550, -0.270f, -0.963f }, { 1950, 240, -5550, -0.055f, -0.998f },
    { 2400, 190, -5600, 0.000f, -1.000f }, { 2850, 240, -5550, 0.055f, -0.998f }, { 3300, 240, -5550, -0.055f, -0.998f },
    { 3750, 190, -5600, 0.000f, -1.000f }, { 4200, 240, -5550, 0.000f, -1.000f }, { 4650, 191, -5600, 0.000f, -1.000f },
    { 5100, 240, -5550, 0.036f, -0.999f }, { 5550, 220, -5567, -0.141f, -0.990f }, { 5982, 240, -5675, -0.311f, -0.951f },
    { 6414, 190, -5851, -0.276f, -0.961f }, { 6846, 240, -5926, -0.466f, -0.885f }, { 7136, 240, -6223, -0.743f, -0.670f },
    { 7425, 220, -6570, -0.726f, -0.688f }, { 7734, 220, -6857, -0.900f, -0.437f }, { 7771, 200, -7307, -0.965f, -0.262f },
    { 7987, 220, -7756, -0.954f, -0.300f }, { 8044, 220, -8106, -0.967f, -0.256f }, { 8241, 220, -8634, -0.938f, -0.347f },
    { 8391, 240, -9044, -0.948f, -0.318f }, { 8514, 240, -9448, -0.957f, -0.290f }, { 8664, 220, -9945, -0.899f, -0.438f },
    { 8916, 220, -10302, -0.817f, -0.577f }, { 9168, 240, -10659, -0.844f, -0.536f }, { 9404, 220, -11074, -0.844f, -0.536f },
    { 9656, 220, -11431, -0.817f, -0.577f }, { 9908, 240, -11788, -0.827f, -0.562f }, { 10154, 240, -12164, -0.837f, -0.548f },
    { 10401, 240, -12541, -0.836f, -0.549f }, { 10648, 240, -12917, -0.836f, -0.548f }, { 10894, 240, -13293, -0.837f, -0.547f },
    { 11223, -60, -13795, -0.837f, -0.547f },
};
constexpr RimPoint kGlacierIslandWest[] = {
    { -2935, 77, 5013, 0.455f, 0.890f }, { -2890, 170, 4990, 0.711f, 0.703f }, { -2396, 267, 4440, 0.888f, 0.459f },
    { -2323, 328, 4118, 0.949f, 0.314f }, { -2165, 284, 3761, 0.948f, 0.319f }, { -2043, 270, 3248, 0.997f, 0.076f },
    { -2081, 290, 2785, 0.998f, -0.068f }, { -2102, 243, 2404, 0.962f, -0.272f }, { -2309, 220, 2021, 0.921f, -0.389f },
    { -2455, 277, 1554, 0.942f, -0.335f }, { -2681, 222, 988, 0.952f, -0.305f }, { -2530, 120, 1604, 0.964f, -0.266f },
    { -2392, 90, 2053, 0.923f, -0.385f }, { -2205, 90, 2401, 0.962f, -0.274f }, { -2180, 140, 2800, 0.997f, -0.072f },
    { -2143, 140, 3248, 0.997f, 0.081f }, { -2255, 140, 3697, 0.955f, 0.297f }, { -2405, 90, 4097, 0.955f, 0.298f },
    { -2480, 140, 4396, 0.911f, 0.412f }, { -2890, 190, 4990, 0.711f, 0.703f }, { -2400, 160, 4396, 0.893f, 0.451f },
    { -2325, 110, 4097, 0.955f, 0.298f }, { -2175, 160, 3697, 0.955f, 0.297f }, { -2063, 160, 3248, 0.997f, 0.081f },
    { -2100, 160, 2800, 0.997f, -0.072f }, { -2125, 110, 2401, 0.962f, -0.274f }, { -2312, 110, 2053, 0.923f, -0.385f },
    { -2450, 160, 1604, 0.964f, -0.266f }, { -2601, 262, 988, 0.968f, -0.251f }, { -2720, 262, 554, 0.988f, -0.155f },
    { -2739, 152, 120, 0.968f, -0.253f }, { -2958, 262, -314, 0.967f, -0.254f }, { -2978, 172, -748, 0.985f, 0.174f },
    { -2857, 242, -1037, 0.991f, -0.131f }, { -3206, 282, -1487, 1.000f, 0.001f }, { -2855, 222, -1937, 1.000f, 0.022f },
    { -3204, 452, -2427, 0.996f, -0.091f }, { -3013, 202, -2837, 0.999f, -0.051f }, { -3282, 242, -3287, 0.865f, -0.502f },
    { -3500, 220, -3675, 0.994f, -0.109f }, { -3437, 240, -3884, 0.858f, 0.513f }, { -3337, 240, -3984, 0.533f, 0.846f },
    { -3150, 240, -4050, 0.178f, 0.984f }, { -2600, 220, -4060, 0.004f, 1.000f }, { -2150, 150, -4055, 0.033f, 0.999f },
    { -1700, 220, -4090, 0.006f, 1.000f }, { -1250, 150, -4060, 0.011f, 1.000f }, { -800, 190, -4100, 0.011f, 1.000f },
    { -350, 180, -4070, -0.033f, 0.999f }, { 100, 240, -4070, -0.066f, 0.998f }, { 550, 150, -4010, -0.033f, 0.999f },
    { 1000, 240, -4040, 0.000f, 1.000f }, { 1450, 165, -4010, 0.011f, 1.000f }, { 1900, 205, -4050, 0.003f, 1.000f },
    { 2500, 225, -4000, -0.338f, 0.941f }, { 2758, 195, -3821, -0.676f, 0.737f }, { 2900, 155, -3650, -0.903f, 0.431f },
    { 2950, 155, -3400, -0.833f, -0.553f }, { 2291, 175, -3266, -0.340f, -0.940f }, { 1812, 135, -3008, -0.687f, -0.726f },
    { 1571, 145, -2614, -0.853f, -0.522f },
};
constexpr RimPoint kGlacierIslandEast[] = {
    { -2890, 190, 4990, 0.962f, -0.273f }, { -2815, 190, 5254, 0.962f, -0.274f }, { -2739, 190, 5521, 0.881f, -0.474f },
    { -2448, 140, 5860, 0.574f, -0.819f }, { -2187, 140, 5959, 0.059f, -0.998f }, { -1750, 140, 5850, -0.393f, -0.920f },
    { -1428, 190, 5647, -0.611f, -0.792f }, { -1105, 140, 5345, -0.565f, -0.825f }, { -683, 190, 5142, -0.499f, -0.866f },
    { -311, 190, 4889, -0.625f, -0.781f }, { 12, 240, 4587, -0.565f, -0.825f }, { 434, 340, 4384, -0.496f, -0.868f },
    { 863, 360, 4097, -0.854f, -0.520f }, { 889, 360, 3869, -0.951f, 0.311f }, { 555, 300, 3507, -0.770f, 0.639f },
    { 275, 340, 3131, -0.785f, 0.619f }, { -5, 220, 2795, -0.942f, 0.335f }, { 0, 240, 2350, -0.990f, 0.140f },
    { -150, 175, 1850, -0.943f, 0.332f }, { -405, 272, 1222, -0.921f, 0.388f }, { -635, 221, 696, -0.946f, 0.326f },
    { -778, 221, 138, -0.959f, 0.284f }, { -898, 172, -219, -0.997f, -0.072f }, { -640, 241, -728, -0.834f, -0.551f },
    { -356, 177, -1066, -0.761f, -0.649f }, { -86, 199, -1378, -0.680f, -0.733f }, { 273, 181, -1644, -0.554f, -0.833f },
    { 1022, 235, -2089, -0.730f, -0.683f }, { 1203, 186, -2446, -0.764f, -0.645f }, { 1562, 239, -2712, -0.636f, -0.772f },
    { 1843, 291, -2969, -0.571f, -0.821f }, { 2381, 225, -3245, -0.362f, -0.932f }, { 2950, 155, -3400, -0.372f, -0.928f },
    { 2481, 65, -3146, -0.467f, -0.884f }, { 1944, 111, -2870, -0.571f, -0.821f }, { 1663, 69, -2613, -0.675f, -0.738f },
};

// A texture (RGBA16, by its segment and offset, width x height, how many
// texels along a unit of rim).
struct Texture {
    int segment;
    uint32_t offset;
    int width, height;
    float texels_per_unit;
};
// A rim, and the texture it wears as the wall or hill below it does; and,
// if it has one, the shore's, worn from kShoreTop down, as the land's own
// edge wears it: sand, wet sand and the shallows at the waterline (the hill's
// texture then ends at its own strip of sand where the shore's begins). A
// course's rims.
struct Rim {
    const RimPoint* points;
    int count;
    Texture hill;
    Texture shore;
};
#define RIM(points) points, int(sizeof(points) / sizeof(points[0]))
struct Backs {
    uint32_t course;
    Rim rims[4];
    int rim_count;
};
constexpr Backs kBacks[] = {
    // Sunny Beach: the hill's foliage (0E013000), the beach's sand into the
    // shallows (0E010800, as on the mainland's ends).
    { 1, { { RIM(kSunnyRim), { 14, 0x013000, 64, 32, 0.0537f }, { 14, 0x010800, 32, 64, 0.0537f } } }, 1 },
    { 8, { { RIM(kGlacierCliffs), { 14, 0x003800, 32, 64, 0.107f }, {} },
           { RIM(kGlacierIslandWest), { 14, 0x004800, 32, 64, 0.107f }, {} },
           { RIM(kGlacierIslandEast), { 14, 0x004800, 32, 64, 0.107f }, {} } }, 3 },
};
#undef RIM

// Rows stepped out from a rim, and how far down below the water the slope
// ends (it meets the seafloor there, or goes under it).
constexpr int kRows = 8;
constexpr float kFoot = -80.0f;
// Where a shore begins, its rows, and the row of its texture at the
// waterline (the shallows' colour below it, the texture's last rows).
constexpr float kShoreTop = 220.0f;
constexpr int kShoreRows = 4;
constexpr float kShoreWaterline = 54.0f;

// Memory, unused by the game in any race (checked in dumps of several
// courses): a marker; the list drawing them; their vertices, elsewhere.
// The identity matrix they are drawn under sits after the marker.
constexpr uint32_t kBase = 0x760000, kList = kBase + 0x100, kListEnd = 0x770000;
constexpr uint32_t kVertices = 0x7A0000, kVerticesEnd = 0x7B0000;
constexpr uint32_t kMarker = 0x4C414E44u;       // "LAND"

void put_half(uint8_t* rdram, uint32_t address, float value) {
    *reinterpret_cast<int16_t*>(rdram + (address ^ 2)) = int16_t(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
}

struct Builder {
    uint8_t* rdram;
    uint32_t at, end;
    bool full = false;
    void command(uint32_t w0, uint32_t w1) {
        if (at + 8 > end) { full = true; return; }
        reinterpret_cast<uint32_t*>(rdram + at)[0] = w0;
        reinterpret_cast<uint32_t*>(rdram + at)[1] = w1;
        at += 8;
    }
};

// How far out from a rim of height `top` its slope reaches the foot: the
// higher the farther, steep for a low rim, gentler for a high one.
float reach(float top) { return std::clamp(2.2f * (top - kFoot), 1200.0f, 3000.0f); }

// The slope's height a share `t` of the way out from a rim of height `top`.
float slope_height(float top, float t) {
    const float s = t * t * (3.0f - 2.0f * t);
    return kFoot + (top - kFoot) * (1.0f - s);
}

// The share of the way out where a rim of height `top`'s slope comes down to
// `height` (0 if the rim is no higher).
float slope_share(float top, float height) {
    if (top <= height) return 0.0f;
    float lo = 0.0f, hi = 1.0f;
    for (int k = 0; k < 20; ++k) {
        const float mid = (lo + hi) / 2.0f;
        (slope_height(top, mid) > height ? lo : hi) = mid;
    }
    return hi;
}

// A rim's points, with each sharp outward bend (the walls either side
// turning away from each other by more than kBend) fanned out: the point
// repeated, its way out turned in steps from the wall before's to the wall
// after's, so the slope wraps round the corner. One way out at the corner,
// the mean of the two, left a wedge open between the walls' slopes.
constexpr float kBend = 0.6f;  // radians
std::vector<RimPoint> cornered(const Rim& rim) {
    std::vector<RimPoint> points;
    // A stretch's way out: square to it, on the side its points' ways out lie.
    const auto normal = [](const RimPoint& a, const RimPoint& b, float* nx, float* nz) {
        const float ex = b.x - a.x, ez = b.z - a.z, n = std::max(std::hypot(ex, ez), 1e-3f);
        *nx = ez / n;
        *nz = -ex / n;
        if (*nx * (a.out_x + b.out_x) + *nz * (a.out_z + b.out_z) < 0.0f) {
            *nx = -*nx;
            *nz = -*nz;
        }
    };
    for (int i = 0; i < rim.count; ++i) {
        const RimPoint& p = rim.points[i];
        if (i == 0 || i + 1 == rim.count) {
            points.push_back(p);
            continue;
        }
        const RimPoint& a = rim.points[i - 1];
        const RimPoint& b = rim.points[i + 1];
        float ax, az, bx, bz;
        normal(a, p, &ax, &az);
        normal(p, b, &bx, &bz);
        const float turn = std::atan2(ax * bz - az * bx, ax * bx + az * bz);
        // Outward: the next point back on the land's side of the wall before.
        const bool outward = (b.x - p.x) * ax + (b.z - p.z) * az < 0.0f;
        if (!outward || std::fabs(turn) < kBend || std::hypot(b.x - p.x, b.z - p.z) < 1.0f ||
            std::hypot(p.x - a.x, p.z - a.z) < 1.0f) {
            points.push_back(p);
            continue;
        }
        const int steps = std::min(8, int(std::ceil(std::fabs(turn) / 0.25f)));
        const float from = std::atan2(az, ax);
        for (int k = 0; k <= steps; ++k) {
            const float angle = from + turn * float(k) / float(steps);
            points.push_back({ p.x, p.y, p.z, std::cos(angle), std::sin(angle) });
        }
    }
    return points;
}

// Builds course `backs`'s vertices and list, the frame's segments locating
// its textures; whether it all fitted.
bool build(uint8_t* rdram, const Backs& backs, const uint32_t segments[16]) {
    uint32_t* marker = reinterpret_cast<uint32_t*>(rdram + kBase);
    if (*marker != kMarker) {
        for (const auto& [from, to] : { std::pair{ kBase, kListEnd }, std::pair{ kVertices, kVerticesEnd } }) {
            for (uint32_t a = from; a < to; a += 4) {
                if (*reinterpret_cast<const uint32_t*>(rdram + a) != 0) {
                    std::fprintf(stderr, "[land] memory at 0x%06X is in use; no backs\n", a);
                    return false;
                }
            }
        }
        *marker = kMarker;
    }
    for (int i = 0; i < 16; ++i) {
        put_half(rdram, kBase + 0x40 + uint32_t(i) * 2, (i % 5) == 0 ? 1.0f : 0.0f);
        put_half(rdram, kBase + 0x60 + uint32_t(i) * 2, 0.0f);
    }
    Builder list{ rdram, kList, kListEnd };
    // The state, as the seafloor's: one cycle, filtered, in perspective,
    // opaque, tested against depth and writing it, texture times shade,
    // unlit, unfogged and not culled.
    list.command(0xE7000000u, 0);
    list.command(0xBA001402u, 0x00000000u);
    list.command(0xBA000C02u, 0x00002000u);
    list.command(0xBA001301u, 0x00080000u);
    list.command(0xBA000903u, 0x00000C00u);
    list.command(0xBA001001u, 0x00000000u);
    list.command(0xBA000E02u, 0x00000000u);
    list.command(0xBA001102u, 0x00000000u);
    list.command(0xB9000002u, 0x00000000u);
    list.command(0xB9000201u, 0x00000000u);
    list.command(0xB900031Du, 0x00552078u);
    list.command(0xFC121824u, 0xFF33FFFFu);
    list.command(0xB6000000u, 0x000F3001u);
    list.command(0xB7000000u, 0x00000205u);
    list.command(0xBB000001u, 0xFFFFFFFFu);
    // Each stretch of rim between two points, for each texture it wears: their
    // two columns of rows over the share of the way out that texture covers,
    // loaded together, and the quads between; the texture first, as the
    // seafloor's are loaded: an RGBA16 block, the render tile repeating it
    // (clamped down the slope for a shore, whose texture is laid once).
    const auto log2 = [](int v) { uint32_t n = 0; while ((1 << (n + 1)) <= v) ++n; return n; };
    uint32_t vertex = kVertices;
    const auto band = [&](const Rim& rim, const Texture& tex, bool shore, bool hill_over_shore) {
        const uint32_t texture = (segments[tex.segment] + tex.offset) & 0x7FFFFF;
        const uint32_t texels = uint32_t(tex.width * tex.height), words = uint32_t(tex.width) / 4;
        list.command(0xE7000000u, 0);
        list.command(0xFD100000u, 0x80000000u | texture);
        list.command(0xF5100000u, 0x07000000u);
        list.command(0xE6000000u, 0);
        list.command(0xF3000000u, ((texels - 1) << 12) | ((2048u + words - 1) / words) | 0x07000000u);
        list.command(0xE7000000u, 0);
        list.command(0xF5100000u | (words << 9),
                     (shore ? (2u << 18) : (log2(tex.height) << 14)) | (log2(tex.width) << 4));
        list.command(0xF2000000u, (uint32_t(tex.width - 1) << 14) | (uint32_t(tex.height - 1) << 2));
        const int rows = shore ? kShoreRows : kRows;
        const std::vector<RimPoint> points = cornered(rim);
        float along = 0.0f;
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const RimPoint* ends[2] = { &points[i], &points[i + 1] };
            const float length = std::hypot(ends[1]->x - ends[0]->x, ends[1]->z - ends[0]->z);
            if (vertex + 2 * uint32_t(rows + 1) * 16 > kVerticesEnd) return false;
            // Whole repeats of the texture taken off the stretch's texels
            // along, to keep them within the coordinates' range.
            const float repeats = std::floor(along * tex.texels_per_unit / float(tex.width)) * float(tex.width);
            for (int e = 0; e < 2; ++e) {
                const RimPoint& p = *ends[e];
                const float out = reach(p.y);
                const float s = (along + (e == 1 ? length : 0.0f)) * tex.texels_per_unit;
                // The share of the way out where the shore begins.
                const float split = hill_over_shore || shore ? slope_share(p.y, kShoreTop) : 1.0f;
                const float from = shore ? split : 0.0f, to = shore ? 1.0f : split;
                for (int k = 0; k <= rows; ++k) {
                    const float t = from + (to - from) * float(k) / float(rows);
                    const float y = slope_height(p.y, t);
                    // Shaded by how steep it is there: the steepest darkest.
                    const float rise = (p.y - kFoot) * 6.0f * t * (1.0f - t) / out;
                    const float shade = std::clamp(0.92f - 0.45f * (1.0f - 1.0f / std::sqrt(1.0f + rise * rise)), 0.0f, 1.0f);
                    const uint32_t c = uint32_t(shade * 255.0f);
                    // Down the texture: a hill's once over its share (all the
                    // way, if no shore); a shore's by height, its waterline row
                    // at the water.
                    const float row = shore ? std::clamp(kShoreWaterline * (kShoreTop - y) / kShoreTop, 0.0f, float(tex.height) - 0.5f)
                                            : float(tex.height) * float(k) / float(rows);
                    const uint32_t v = vertex + uint32_t(e * (rows + 1) + k) * 16;
                    put_half(rdram, v + 0, p.x + p.out_x * out * t);
                    put_half(rdram, v + 2, y);
                    put_half(rdram, v + 4, p.z + p.out_z * out * t);
                    put_half(rdram, v + 6, 0.0f);
                    put_half(rdram, v + 8, (s - repeats) * 32.0f);
                    put_half(rdram, v + 10, row * 32.0f);
                    *reinterpret_cast<uint32_t*>(rdram + v + 12) = (c << 24) | (c << 16) | (c << 8) | 0xFFu;
                }
            }
            const uint32_t n = 2 * uint32_t(rows + 1);
            list.command((0x04u << 24) | (n << 9) | (n * 16 - 1), 0x80000000u | vertex);
            for (uint32_t k = 0; k < uint32_t(rows); ++k) {
                const uint32_t a = k, b = k + 1, c = uint32_t(rows + 1) + k, d = c + 1;
                list.command(0xBFu << 24, ((a * 5) << 16) | ((c * 5) << 8) | (d * 5));
                list.command(0xBFu << 24, ((a * 5) << 16) | ((d * 5) << 8) | (b * 5));
            }
            vertex += n * 16;
            along += length;
        }
        return true;
    };
    for (int r = 0; r < backs.rim_count; ++r) {
        const Rim& rim = backs.rims[r];
        const bool has_shore = rim.shore.width != 0;
        if (!band(rim, rim.hill, false, has_shore)) return false;
        if (has_shore && !band(rim, rim.shore, true, true)) return false;
    }
    list.command(0xB8000000u, 0);
    if (list.full) return false;
    std::fprintf(stderr, "[land] course %u's backs built: %u vertices\n", backs.course, (vertex - kVertices) / 16);
    return true;
}

}  // namespace

// Whether (x, z) on course `course` is on one of its land's backs, above
// the water (higher than `above`); if so, the nearest place off it, out from
// the land (`out_x`, `out_z`). Within a slope's reach, its height there
// (`height`) and the way out (`way_x`, `way_z`) either way. The game knows
// nothing of the backs, so a jet ski would ride into them.
namespace {

// Course `course`'s rims' nearest point to (x, z), its height there and the
// way out; the distance squared, or 1e30 if the course has no backs. Where
// slopes overlap (Glacier Coast's narrow canyon, its two sides' slopes
// meeting), the rim whose slope is highest there instead; in *slope, the
// slope's height there (-1e30 if none). Each rim answers
// by its nearest point only: a farther stretch of it, seen past its end at a
// slant, would put a slope where the nearer wall has the course's water
// (Glacier Coast's channels, ridden into invisible slopes).
float nearest_rim(uint32_t course, float x, float z, float* px, float* pz, float* top, float* nx, float* nz, float* slope) {
    float best = 1e30f, highest = -1e30f;
    *slope = -1e30f;
    for (const Backs& backs : kBacks) {
        if (backs.course != course) continue;
        for (int r = 0; r < backs.rim_count; ++r) {
            const Rim& rim = backs.rims[r];
            float near = 1e30f, qx = 0, qz = 0, y = 0, ox = 0, oz = 0;
            bool past_end = false;
            for (int i = 0; i + 1 < rim.count; ++i) {
                const RimPoint& a = rim.points[i];
                const RimPoint& b = rim.points[i + 1];
                const float ex = b.x - a.x, ez = b.z - a.z;
                const float u = std::clamp(((x - a.x) * ex + (z - a.z) * ez) / std::max(ex * ex + ez * ez, 1.0f), 0.0f, 1.0f);
                const float sx = a.x + ex * u, sz = a.z + ez * u;
                const float d = (x - sx) * (x - sx) + (z - sz) * (z - sz);
                if (d >= near) continue;
                near = d;
                past_end = (i == 0 && u <= 0.0f) || (i + 2 == rim.count && u >= 1.0f);
                qx = sx;
                qz = sz;
                y = a.y + (b.y - a.y) * u;
                ox = a.out_x + (b.out_x - a.out_x) * u;
                oz = a.out_z + (b.out_z - a.out_z) * u;
            }
            if (near >= 1e30f) continue;
            const float far = reach(y), out = std::sqrt(near);
            // (Nor past either end of the rim, where none is drawn.)
            const bool behind = (x - qx) * ox + (z - qz) * oz > 0.0f && !past_end;
            const float height = behind && out < far ? slope_height(y, out / far) : -1e30f;
            if (height > highest) {
                highest = height;
            } else if (height < highest || near >= best) {
                continue;
            }
            best = near;
            *px = qx;
            *pz = qz;
            *top = y;
            *nx = ox;
            *nz = oz;
            *slope = height;
        }
    }
    return best;
}

}  // namespace

// How far (x, z) on course `course` is from the land's backs: from their
// foot, out past them; from the rim, on the land's side; 1e9 if the course
// has none. The land's backs count as land for Free Roam's current, so the
// way round behind them stays open.
float land_backs_distance(uint32_t course, float x, float z) {
    float px = 0, pz = 0, top = 0, nx = 0, nz = 0, slope = 0;
    const float best = nearest_rim(course, x, z, &px, &pz, &top, &nx, &nz, &slope);
    if (best >= 1e30f) return 1e9f;
    const float d = std::sqrt(best);
    const bool outside = (x - px) * nx + (z - pz) * nz > 0.0f;
    return outside ? std::max(0.0f, d - reach(top)) : d;
}

// The height of the land's backs at (x, z) on course `course`, in *height;
// false where there are none (on the land's side of a rim, or past a slope's
// foot). A thrown rider stands, walks and swims on them as on the land.
bool land_backs_ground(uint32_t course, float x, float z, float* height) {
    float px = 0, pz = 0, top = 0, nx = 0, nz = 0, slope = 0;
    nearest_rim(course, x, z, &px, &pz, &top, &nx, &nz, &slope);
    if (slope <= -1e29f) return false;
    *height = slope;
    return true;
}

bool land_backs_hold(uint32_t course, float x, float z, float above, float* out_x, float* out_z, float* way_x,
                     float* way_z, float* height) {
    *height = -1e9f;
    // The rim's nearest point, its height there, and the way out.
    float px = 0, pz = 0, top = 0, nx = 0, nz = 0, slope = 0;
    const float best = nearest_rim(course, x, z, &px, &pz, &top, &nx, &nz, &slope);
    const float n = std::hypot(nx, nz);
    // Only where nearest_rim finds a slope (not past a rim's end).
    if (best >= 1e30f || n < 1e-3f || slope <= -1e29f) return false;
    nx /= n;
    nz /= n;
    // On the land's outer side only, and within the slope's reach.
    const float vx = x - px, vz = z - pz;
    if (vx * nx + vz * nz <= 0.0f) return false;
    const float out = std::sqrt(best), far = reach(top);
    if (out >= far) return false;
    // Out along the way from the rim's nearest point to (x, z).
    const float wx = vx / out, wz = vz / out;
    *way_x = wx;
    *way_z = wz;
    *height = slope_height(top, out / far);
    if (*height <= above) return false;
    // Where the slope comes down to `above`: stepped out to it.
    float lo = out / far, hi = 1.0f;
    for (int k = 0; k < 20; ++k) {
        const float mid = (lo + hi) / 2.0f;
        (slope_height(top, mid) > above ? lo : hi) = mid;
    }
    *out_x = px + wx * far * hi;
    *out_z = pz + wz * far * hi;
    return true;
}

// For the display-list rewriter, about to draw course `course`'s water: the
// list drawing its land's backs, or 0, and the identity matrix to draw it
// under. `segments` are the frame's.
uint32_t land_backs_list(uint8_t* rdram, uint32_t course, const uint32_t segments[16], uint32_t* identity) {
    *identity = 0x80000000u | (kBase + 0x40);
    const Backs* backs = nullptr;
    for (const Backs& b : kBacks) {
        if (b.course == course) backs = &b;
    }
    if (backs == nullptr || segments[backs->rims[0].hill.segment] == 0) return 0;
    // Built again whenever the course or where its textures are changes.
    const uint32_t textures = segments[backs->rims[0].hill.segment];
    static uint32_t built_course = ~0u, built_textures = 0;
    static bool ok = false;
    if (built_course != course || built_textures != textures) {
        built_course = course;
        built_textures = textures;
        ok = build(rdram, *backs, segments);
    }
    return ok ? 0x80000000u | kList : 0;
}

}  // namespace wr64

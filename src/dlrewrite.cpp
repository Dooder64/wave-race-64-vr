// The display-list rewriter. See include/wr64/dlrewrite.h for what it is for.
//
// What it does (phase 07, steps A1 to A3):
//
// 1. The drawn region. The scissor the game sets is read as it goes by, and
//    the presentation's crop follows it (wr64::display::set_content): with
//    two players the game draws into a taller region than with one.
//
// 2. 3D inside a menu's 2D layout. In a menu state, a perspective projection
//    drawn after the 2D layout has started -- the models on the watercraft
//    and rider select screens -- is given a viewport whose origin is the
//    centre of the frame. RT64 keeps a full-width projection "adjusted" to
//    the widened frame by default: a wider field of view for a perspective
//    projection, 2D kept at 4:3 in the middle for an orthographic one. A
//    viewport with an origin switches that off for its projection, so the
//    game's frustum is rendered into the centred 4:3 region, where the 2D is.
//    3D drawn before any 2D is a backdrop (the title, the course overview)
//    and stays as wide as the frame.
//
// 3. The 2D layer. Each 2D draw -- a vertex load, or a call to a static list,
//    under an orthographic projection -- is classified by where it sits on
//    the 320-wide screen: an element in the left third is anchored to the
//    left edge, one in the right third to the right edge, one that covers the
//    frame is stretched across it, and the rest stay in the middle. A tag
//    table in the settings folder (hud.json) overrides the class per texture
//    or per static list. Anchors are applied only when the HUD Placement
//    setting asks for them; stretching is applied always, since a sun glare
//    that stops at 4:3 is a defect in any mode. Stretching is for the 2D
//    layer alone, though: a full-frame rectangle drawn under a perspective
//    projection belongs to the 3D pass, which is already at the frame's
//    width, and stretching it magnifies the picture. See classify().
//
//    RT64 anchors 2D geometry by the origin carried on its viewport, and
//    stretches by a flag on its projection group, both per projection rather
//    than per triangle, so each change of class reissues the game's own
//    viewport or projection command after the new alignment or group. An
//    anchored element is also inset by how far the visible picture's edge
//    lies inside RT64's widened frame (wr64::display::anchor_inset), so that
//    RT64's edge means the window's edge. The 2D projection groups carry no
//    interpolation: the same projection is reissued several times per frame
//    with different flags, and RT64 must never blend one with another.
//
// 4. The sky and the water (phase 07, step B). The game rebuilds both from
//    vertices every frame, under an unchanging identity matrix, so RT64 pairs
//    that matrix perfectly, finds no motion, and holds them still between the
//    game's frames while the world moves. Each is given a matrix group asking
//    for the vertices and their texture coordinates to be interpolated as
//    well, and an explicit transform id so the pairing they depend on cannot
//    be lost to a tie. See the sky and water sections below.
//
// How the list is read. The game builds one top-level list per frame with
// every matrix load inline, and calls its model and HUD lists from it. This
// walks the top-level list, copying each command, following branches in
// place and leaving calls as calls: the called lists live where they live,
// and RT64 runs them from there. A called list is scanned, read-only, to
// learn its extent and texture, so it can be classified like an inline draw.
// Segment table writes, the projection matrix and a modelview stack are
// tracked so that matrices and vertices can be read from RDRAM; the
// perspective test is RT64's own, element [3][3] of the projection.
//
// Memory. RDRAM in the runtime stores 32-bit words natively, so a command is
// two host words at its physical offset, and RT64 reads it the same way. The
// 16-bit halves of a matrix or a vertex are the exception: they sit at their
// offset XOR 2, which is also how RT64 reads them. The scratch list lives at
// physical 0x700000, in the top of the 8 MB the runtime reports; the
// cartridge is a 4 MB one that never reads osMemSize, so nothing of the
// game's is there, and the audio task's private copy of its command list is
// above it at 0x7E0000.
//
// WR64_HUD_TRACE=1 prints every 2D draw of the first two drawing lists after
// each change of game state, with its identity, its extent and the class it
// was given; that is how a tag for hud.json is found. It also reports any
// element that is given one class in one frame and another in the next,
// which flickers between the two however defensible each classification is. WR64_HUD_OFF=1 leaves
// the 2D layer alone, WR64_NO_REWRITE=1 switches the rewriter off entirely,
// WR64_NO_SKY_INTERP=1 leaves the sky at the game's rate and
// WR64_NO_WATER_INTERP=1 leaves the water's waves at it. WR64_PAIRING=1, read
// in patches/framerate.cpp rather than here, reports how much of the frame
// RT64 is managing to interpolate at all.
//
// WR64_3D_TRACE names a file and writes the whole of a few race frames into
// it -- every matrix load, vertex load and call, with each block's hash and
// its clip-space extent -- which is how the sky was found: two consecutive
// frames diffed against each other say which geometry the game rebuilds
// rather than moves. WR64_3D_TRACE_FRAMES sets how many frames (two by
// default) and WR64_3D_TRACE_STATE takes a game state in hexadecimal for the
// screens that are not races, or "all" for one frame of every state.
//
// WR64_CAMERA_LOG=1 prints the race camera's position, direction and lens
// about once a second; see log_camera. WR64_CULL_SURVEY=1 reports in which
// directions from the camera, and how far, a race draws anything; see the
// cull survey.
//
// 5. VR mode (WR64_VR=1) opens the world's scissor. The game draws its 3D
// inside (8, 20)-(311, 219) and leaves the rest of its framebuffer black for a
// CRT's overscan to hide; through a headset's lens that border would sit in
// the middle of the view, so the scissor that names that region is widened
// to the whole framebuffer. Everything else this walk does still sees the
// game's own scissor. WR64_VR_KEEP_SCISSOR=1 leaves it alone, for comparison.
//
// 6. In VR mode RT64 draws a frame's scene through the headset's lens and
// everything flat on a panel in front of the eyes (tools/patch_rt64_vr.py).
// 3D that is not the scene belongs on the panel too, so a perspective
// projection other than the game's camera's -- the one it builds each frame
// at 0xE008 in segment 3, the only one the title loads -- is given a group of
// its own: in a
// race (the direction arrow over the HUD) G_EX_ASPECT_STRETCH, which at the
// headset's 4:3 changes nothing but keeps it off the lens; anywhere else (the
// jet skis of the menus) G_EX_ASPECT_ADJUST, which RT64 draws in front of the
// scene behind the panel, in its box -- the course card's and course
// select's track model too. The title's lens flare and the sun's glints and
// glare, rectangles that would hang on the panel in front of the player's
// face, are left out. So is the screen-wide rectangle the results and course
// select screens paint behind their water, the first rectangle after a call
// of 0x0106F8A0 (kBackdropSetupList): on the panel, where the water is not, it
// hid the view behind a sheet of colour.

#include "wr64/dlrewrite.h"
#include "wr64/display.h"
#include "wr64/testdrive.h"
#include "wr64/vr.h"
#include "wr64/water.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <librecomp/game.hpp>
#include <ultramodern/config.hpp>
#include "json/json.hpp"

#include "rt64_extended_gbi.h"

namespace wr64 {
// patches/water_all_round.cpp: the merged wave mesh for the frame buffer
// segment 3 points at -- the segment base it expects and its list.
bool water_merged(uint32_t segment3, uint32_t* base, uint32_t* list);
// The same for the full grid of the results screen, ringed out to the distance.
bool water_full_grid_ringed(uint8_t* rdram, uint32_t segment3, uint32_t* base, uint32_t* list);
void water_copy_camera(float* eye_x, float* eye_z, float* heading);
// patches/sky_all_round.cpp: the sky's three draws rebuilt round the camera,
// as lists through segment 6 and the segment base they expect.
struct SkyLists { uint32_t base, floor, gradient, clouds; };
bool sky_all_round(uint8_t* rdram, uint32_t segment3, uint32_t segment6, SkyLists* lists);
// The same for the results screen: a gradient dome only.
bool sky_results_dome(uint8_t* rdram, uint32_t segment3, uint32_t segment6, SkyLists* lists);
// src/first_person.cpp: a view from the player's rider to multiply onto the
// projection in place of the game's, its head shrunk away; or 0.
uint32_t first_person_view(uint8_t* rdram, uint32_t segment3, uint32_t* haze, uint32_t* identity, uint32_t* underside);
// src/body_splash.cpp: the game's bow splash round the thrown rider's
// skidding body, to call after the game's particles; or 0.
uint32_t body_splash_list(uint8_t* rdram, uint32_t list, uint32_t segment5, uint32_t segment3);
// The same round the fellow rider's jet ski on Dolphin Park; or 0.
uint32_t body_splash_companion_list(uint8_t* rdram, uint32_t segment3);
// src/fish.cpp: Dolphin Park's fish, each drawn twice and varied.
bool fish_rewrite(uint8_t* rdram, const uint32_t segments[16], uint32_t course, uint32_t at, uint32_t* next,
                  std::vector<std::pair<uint32_t, uint32_t>>& out);
void fish_field(uint8_t* rdram, const uint32_t segments[16], uint32_t course,
                std::vector<std::pair<uint32_t, uint32_t>>& out);
// src/companions.cpp: the fellow riders on the water, after the player's.
void companions_draw(uint8_t* rdram, const uint32_t segments[16], uint32_t course,
                     std::vector<std::pair<uint32_t, uint32_t>>& out);
// patches/swim_back.cpp: once a frame.
void swim_frame(uint8_t* rdram);
// patches/unlock_all.cpp: once a frame.
void unlock_frame(uint8_t* rdram);
// src/seafloor.cpp: the list drawing the course's seafloor, and an identity
// matrix to draw it under; or 0.
uint32_t seafloor_list(uint8_t* rdram, uint32_t course, const uint32_t segments[16], uint32_t* identity);
bool seafloor_only_below(uint32_t course);
}

namespace {

constexpr uint32_t kScratch = 0x80700000u;
constexpr uint32_t kScratchSize = 0x40000u;      // 256 KB: 32768 commands
constexpr uint32_t kMaxCommands = kScratchSize / 8;

// Fast3D opcodes this walk has to understand. Everything else is copied.
constexpr uint8_t kOpMtx = 0x01;
constexpr uint8_t kOpMoveMem = 0x03;
constexpr uint8_t kOpVtx = 0x04;
constexpr uint8_t kOpDisplayList = 0x06;
constexpr uint8_t kOpMoveWord = 0xBC;
constexpr uint8_t kOpPopMtx = 0xBD;
constexpr uint8_t kOpEndDisplayList = 0xB8;
constexpr uint8_t kOpSetScissor = 0xED;     // RDP, 10.2 fixed point
constexpr uint8_t kOpSetTextureImage = 0xFD;

// The three commands that draw a rectangle: a textured one, a textured one
// flipped, and a filled one. All three carry their corners in the same fields,
// and RT64 applies the same alignment and aspect state to all three.
//
// The filled one was missing here, and that is what left the pause screen's
// dim at 4:3 with a bright band down its left: the dim is a fill rectangle,
// so it was never classified and never stretched, however the test that
// decides what to stretch was written.
constexpr uint8_t kOpTexRect = 0xE4;
constexpr uint8_t kOpTexRectFlip = 0xE5;
constexpr uint8_t kOpFillRect = 0xF6;

bool is_rect(uint8_t op) {
    return op == kOpTexRect || op == kOpTexRectFlip || op == kOpFillRect;
}

// The sun's lens flare (circles of one texture at many sizes along a line
// through the frame), its glints, and the glare that whitens the whole frame
// while the sun is in view, drawn as rectangles under the scene's
// perspective projection on the title; left out in VR (see 6. at the top).
constexpr uint32_t kLensFlareTexture = 0x0100FAB0;
constexpr uint32_t kSunGlintTexture = 0x08004400;
constexpr uint32_t kSunGlareTexture = 0x01005748;

// The list the results and course select screens call before painting the
// whole picture behind their water; that rectangle is left out in VR (see 6.
// at the top).
constexpr uint32_t kBackdropSetupList = 0x0106F8A0;
// The full grid of waves the results-like screens look down on.
constexpr uint32_t kFullGridList = 0x010082F0;

// The game's camera projection, which it builds each frame in its frame
// buffer, reached through segment 3: in VR the scene seen through the lens
// (see 6. at the top).
constexpr uint32_t kSceneProjection = 0x0300E008;

// The game's camera, multiplied onto that projection: in first person, a view
// from the player's rider instead (src/first_person.cpp).
constexpr uint32_t kSceneView = 0x0300E088;

constexpr uint32_t kMtxProjection = 0x01;
constexpr uint32_t kMtxLoad = 0x02;
constexpr uint32_t kMtxPush = 0x04;

// The course being raced (gCourseID).
constexpr uint32_t kCourseId = 0x800D8170;
constexpr uint8_t kMoveWordSegment = 0x06;
constexpr uint8_t kMoveMemViewport = 0x80;

// RT64 displaces a viewport by origin / 1024 of the framebuffer's width, in
// quarter pixels; these offsets undo it, so the game's viewport data is used
// as it is and only the anchor moves.
constexpr int kFramebufferWidth = 320;
constexpr int kFramebufferHeight = 240;
int origin_cancel(uint32_t origin) {
    return -static_cast<int>((origin * kFramebufferWidth * 4) / G_EX_ORIGIN_RIGHT);
}

// A 2D draw covers the frame when its vertices reach this close to every edge
// of clip space; the margin only forgives rounding. Anchoring is by the
// element's centre: past a third of the way to an edge, it belongs to it.
constexpr float kFrameEdge = 0.97f;
constexpr float kAnchorThird = 1.0f / 3.0f;

// The states in which the game is racing: the attract demo, and the race
// itself, whichever mode it is in.
bool racing(uint32_t state) {
    return state == 0x07 || state == 0x28;
}

// Whether the frame is a race, read from the frame itself rather than from
// the game's state variable.
//
// The state variable was the wrong thing to key on: the game keeps a dozen
// states around a race -- 0x28 through 0x2D at least, by the decompilation's
// enumeration -- and a race frame in a state this port did not list was
// treated as a menu and squeezed to 4:3.
//
// The frame says it instead. A race draws its world first, under a
// perspective projection, and sets an inset scissor to do it: (8, 20)-(311,
// 219) with one player, (8, 12)-(311, 229) with two. Menus with a 3D backdrop
// -- the title, the main menu, the select screens -- do both of those as well,
// so neither is enough on its own. What separates them is where the 2D goes:
// a race switches to an orthographic projection to draw its HUD, while a menu
// lays its frames and labels out with the backdrop's perspective projection
// still loaded. Measured across the title, the main menu, the select screens
// and a race, every race HUD rectangle was drawn under an orthographic
// projection and every menu rectangle under a perspective one, with no
// overlap. The projection type is per draw, so it is tested there; these two
// are the frame-wide half of the same question.
struct RaceTest {
    bool first_projection_seen = false;
    bool world_first = false;
    bool inset_scissor = false;

    bool race() const { return world_first && inset_scissor; }
};

enum class Class { Auto, Left, Right, Stretch };

const char* class_name(Class c) {
    switch (c) {
        case Class::Left:    return "left";
        case Class::Right:   return "right";
        case Class::Stretch: return "stretch";
        default:             return "center";
    }
}

// The tag table: hud.json in the settings folder. Four lists of identities,
// "tex:0x01004A20" for a texture and "dl:0x0106F8A0" for a static list, as
// the trace prints them, under "left", "right", "center" and "stretch".
struct Tags {
    std::unordered_map<std::string, Class> by_identity;
    bool loaded = false;

    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    void load() {
        loaded = true;
        const std::filesystem::path path = recomp::get_config_path() / "hud.json";
        std::ifstream in(path);
        if (!in) {
            // Leave a template behind so the format does not have to be guessed.
            std::ofstream out(path);
            if (out) {
                out << "{\n"
                       "    \"help\": \"Run with WR64_HUD_TRACE=1 to list 2D draws with their identity"
                       " (tex:... or dl:...) and the class they were given; put an identity in one"
                       " of the lists below to override it.\",\n"
                       "    \"left\": [],\n"
                       "    \"right\": [],\n"
                       "    \"center\": [],\n"
                       "    \"stretch\": []\n"
                       "}\n";
            }
            return;
        }
        nlohmann::json doc;
        try {
            in >> doc;
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[wr64] hud.json could not be read: %s\n", e.what());
            return;
        }
        const std::pair<const char*, Class> lists[] = {
            { "left", Class::Left }, { "right", Class::Right },
            { "center", Class::Auto }, { "stretch", Class::Stretch },
        };
        int count = 0;
        for (const auto& [key, cls] : lists) {
            if (!doc.contains(key) || !doc[key].is_array()) continue;
            for (const auto& item : doc[key]) {
                if (item.is_string()) {
                    by_identity[lower(item.get<std::string>())] = cls;
                    ++count;
                }
            }
        }
        if (count > 0) {
            std::fprintf(stderr, "[wr64] hud.json: %d tagged elements\n", count);
        }
    }

    bool lookup(const std::string& identity, Class& out) const {
        const auto it = by_identity.find(lower(identity));
        if (it == by_identity.end()) return false;
        out = it->second;
        return true;
    }
};

struct Mat4 {
    float m[4][4];

    static Mat4 identity() {
        Mat4 r{};
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }

    // Row-vector convention, as the N64's matrices are: v' = v * M.
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }
};

// The clip-space extent of a set of vertices.
struct Extent {
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    bool empty() const { return max_x < min_x; }
    // The draw's horizontal extent in the game's own screen pixels.
    float left_px() const { return (min_x + 1.0f) * (kFramebufferWidth / 2.0f); }
    float right_px() const { return (max_x + 1.0f) * (kFramebufferWidth / 2.0f); }
    float center_x() const { return (min_x + max_x) / 2.0f; }
    void add(const Extent& o) {
        min_x = std::min(min_x, o.min_x); max_x = std::max(max_x, o.max_x);
        min_y = std::min(min_y, o.min_y); max_y = std::max(max_y, o.max_y);
    }
};

struct Walker {
    uint8_t* rdram;
    uint32_t segments[16] = {};
    uint32_t* out;
    uint32_t written = 0;       // commands
    bool overflow = false;
    uint32_t state = 0;
    bool menu = true;
    bool anchors = false;       // HUD Placement asks for edges
    bool hud = true;            // the 2D layer is handled at all
    bool noemit = false;        // classify and trace, but change nothing (WR64_HUD_NOEMIT)
    float inset = 0.0f;         // wr64::display::anchor_inset()
    const Tags* tags = nullptr;
    std::string* trace = nullptr;

    // The game's most recent viewport and projection commands, reissued after
    // a change of alignment or group so that the change takes effect.
    bool have_viewport = false;
    uint32_t viewport_w0 = 0, viewport_w1 = 0;
    bool have_projection = false;
    uint32_t projection_w0 = 0, projection_w1 = 0;
    bool have_scissor_cmd = false;
    uint32_t scissor_w0 = 0, scissor_w1 = 0;
    bool scissor_widened = false;

    // The matrices in force, as floats, and the texture last set.
    Mat4 projection = Mat4::identity();         // everything on the projection stack
    Mat4 projection_only = Mat4::identity();    // the projection without the camera
    bool projection_is_perspective = false;
    Mat4 modelview[16];
    int modelview_depth = 0;
    uint32_t texture = 0;

    // The frame's first camera: the affine matrix the game multiplies onto a
    // perspective projection, and that projection as it stood. Read by the
    // camera log (WR64_CAMERA_LOG) and nothing else.
    bool have_camera = false;
    Mat4 camera = Mat4::identity();
    Mat4 camera_lens = Mat4::identity();

    // Section state: A1's centred perspective sections, and the 2D class in
    // force.
    bool centred = false;
    bool seen_ortho = false;
    RaceTest race_test;

    // VR (see 6. at the top): whether a projection was given a group of its
    // own that the next must undo.
    bool own_group = false;
    // VR: whether kBackdropSetupList has been called, on the results or
    // course select screen, and the next rectangle not yet seen.
    bool backdrop_next = false;
    // First person: the rider's view for this frame, worked out at the
    // game's first view multiply (which shrinks the head), or 0.
    bool first_person_tried = false;
    uint32_t first_person_view = 0;
    // With a first-person eye below the waves: the water's fog to draw round
    // it, the matrix for it, the waves' underside to draw in their place, and
    // whether the fog has been drawn this frame.
    uint32_t haze_list = 0, haze_identity = 0, underside_list = 0;
    bool haze_drawn = false;
    Class cls = Class::Auto;
    uint32_t centred_sections = 0;
    uint32_t class_changes = 0;
    uint32_t draws_2d = 0;      // how many 2D elements the list drew

    // The union of the scissors the game set for its drawing this frame. A
    // scissor covering the whole framebuffer is a clear, not drawing, and is
    // left out; what remains is the region the game draws into.
    int scissor_left = 0x7FFF, scissor_top = 0x7FFF, scissor_right = -1, scissor_bottom = -1;

    Walker() {
        modelview[0] = Mat4::identity();
    }

    // ---- memory ---------------------------------------------------------

    uint32_t physical(uint32_t segmented) const {
        const uint32_t base = segments[(segmented >> 24) & 0xF];
        return (base + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }

    const uint32_t* words(uint32_t physical_addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (physical_addr & 0x00FFFFFFu));
    }

    uint16_t half(uint32_t physical_addr) const {
        return *reinterpret_cast<const uint16_t*>(rdram + ((physical_addr & 0x00FFFFFFu) ^ 2));
    }

    int16_t signed_half(uint32_t physical_addr) const {
        return static_cast<int16_t>(half(physical_addr));
    }

    // A fixed-point Mtx: sixteen integer halves then sixteen fraction halves,
    // row-major; the same conversion RT64 makes.
    Mat4 read_matrix(uint32_t physical_addr) const {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            const uint32_t full = (uint32_t(half(physical_addr + 2 * i)) << 16) |
                                  half(physical_addr + 32 + 2 * i);
            r.m[i / 4][i % 4] = static_cast<float>(static_cast<int32_t>(full)) / 65536.0f;
        }
        return r;
    }

    static bool perspective(const Mat4& p) {
        return p.m[3][3] == 0.0f && (p.m[1][1] > 1e-6f || p.m[1][1] < -1e-6f);
    }

    // VR: the first perspective projection a called list loads, or any it
    // calls in turn, other than the game's camera's (kSceneProjection,
    // declared below the walker); 0 if none. The walker writes called lists
    // out as they are, so a projection loaded inside one would otherwise be
    // seen through the lens like the scene (see 6. at the top).
    uint32_t nested_projection(uint32_t physical_addr, uint32_t scene, int depth = 0) const {
        for (uint32_t steps = 0; steps < 4096; ++steps, physical_addr += 8) {
            const uint32_t* c = words(physical_addr);
            const uint8_t op = static_cast<uint8_t>(c[0] >> 24);
            if (op == 0xB8) return 0;
            if (op == 0x01) {                               // a matrix
                const uint32_t params = (c[0] >> 16) & 0xFF;
                if ((params & 0x01) && (params & 0x02) && c[1] != scene && perspective(read_matrix(physical(c[1])))) {
                    return c[1];
                }
            }
            else if (op == 0x06 && depth < 4) {             // a call, or a branch
                if (((c[0] >> 16) & 0xFF) != 0) {
                    physical_addr = physical(c[1]) - 8;
                    continue;
                }
                if (const uint32_t found = nested_projection(physical(c[1]), scene, depth + 1)) return found;
            }
        }
        return 0;
    }

    Extent extent(uint32_t vtx_physical, uint32_t count, const Mat4& mv) const {
        const Mat4 mvp = mv * projection;
        Extent e;
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            const uint32_t v = vtx_physical + i * 16;
            const float x = static_cast<float>(signed_half(v + 0));
            const float y = static_cast<float>(signed_half(v + 2));
            const float z = static_cast<float>(signed_half(v + 4));
            const float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
            const float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
            const float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
            if (cw == 0.0f) continue;
            e.min_x = std::min(e.min_x, cx / cw);
            e.max_x = std::max(e.max_x, cx / cw);
            e.min_y = std::min(e.min_y, cy / cw);
            e.max_y = std::max(e.max_y, cy / cw);
        }
        return e;
    }

    // ---- output ---------------------------------------------------------

    void emit(uint32_t w0, uint32_t w1) {
        if (written >= kMaxCommands) {
            overflow = true;
            return;
        }
        out[written * 2] = w0;
        out[written * 2 + 1] = w1;
        ++written;
    }

    GfxCommand* reserve(uint32_t count) {
        if (written + count > kMaxCommands) {
            overflow = true;
            return nullptr;
        }
        GfxCommand* cmd = reinterpret_cast<GfxCommand*>(out + written * 2);
        written += count;
        return cmd;
    }

    void align_viewport(uint32_t origin, int offset_x) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXSetViewportAlign(cmd, origin, offset_x, 0);
        }
        if (have_viewport) {
            emit(viewport_w0, viewport_w1);
        }
    }

    // A projection group for a 2D section: the aspect mode given, and no
    // interpolation of anything.
    void projection_group(uint32_t aspect) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXMatrixGroup(cmd, G_EX_ID_AUTO, G_EX_INTERPOLATE_SIMPLE, G_EX_NOPUSH, 1,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP,
                           G_EX_COMPONENT_SKIP, G_EX_ORDER_AUTO, G_EX_EDIT_NONE, aspect,
                           G_EX_COMPONENT_SKIP, G_EX_COMPONENT_SKIP);
        }
    }

    void projection_aspect(uint32_t aspect) {
        projection_group(aspect);
        if (have_projection) {
            emit(projection_w0, projection_w1);
        }
    }

    // ---- 2D classes -----------------------------------------------------

    // Leaves the class in force, restoring RT64's defaults for what follows.
    // `before_projection_load` is set when the game's own projection load
    // comes next, in which case the group alone is enough for it.
    void leave_class(bool before_projection_load) {
        switch (cls) {
            case Class::Stretch:
                if (before_projection_load) projection_group(G_EX_ASPECT_AUTO);
                else projection_aspect(G_EX_ASPECT_AUTO);
                break;
            case Class::Left:
            case Class::Right:
                align_viewport(G_EX_ORIGIN_NONE, 0);
                break;
            default:
                break;
        }
        cls = Class::Auto;
    }

    void set_class(Class next) {
        if (next == cls) return;
        leave_class(false);
        const int inset_q = static_cast<int>(inset * 4.0f);
        switch (next) {
            case Class::Stretch:
                projection_aspect(G_EX_ASPECT_STRETCH);
                break;
            case Class::Left:
                align_viewport(G_EX_ORIGIN_LEFT, origin_cancel(G_EX_ORIGIN_LEFT) + inset_q);
                break;
            case Class::Right:
                align_viewport(G_EX_ORIGIN_RIGHT, origin_cancel(G_EX_ORIGIN_RIGHT) - inset_q);
                break;
            default:
                break;
        }
        cls = next;
        ++class_changes;
    }

    // ---- the sky --------------------------------------------------------
    //
    // The sky is the one thing in a race frame that RT64 cannot smooth on its
    // own, and the reason is worth stating: it does not move.
    //
    // Wave Race 64 puts the camera on the projection stack and draws the
    // world under an identity model matrix, so the world is smooth because
    // the camera's matrix is interpolated. The sky is drawn the same way --
    // three seven-vertex bands, the haze, the horizon and the clouds, under
    // that same identity matrix -- but it is not part of the world: the game
    // rebuilds its vertices every frame into a scratch buffer reached through
    // segment 6, following the camera and scrolling the clouds. So the
    // matrix RT64 interpolates is identical from one frame to the next, RT64
    // pairs it perfectly, computes no motion, and holds the sky still for
    // every frame it generates in between. The sky then jumps once per game
    // frame while everything around it moves smoothly, which is the shimmer
    // along the horizon at 60 frames per second and worse above it.
    //
    // The fix is to tell RT64 to interpolate the vertices themselves, which
    // it will do -- it takes the per-vertex difference from the previous
    // frame and carries it as a velocity -- but not by default: a transform
    // group's vertex and texture-coordinate components are G_EX_COMPONENT_SKIP
    // unless something asks for them, because most geometry that changes its
    // vertices between frames is geometry that has been replaced rather than
    // moved. The sky is the opposite case: the same seven points, in the same
    // order, a little further along.
    //
    // The section is opened at the frame's first perspective projection, and
    // closed at the first world matrix the top-level list loads for itself --
    // the sky's own two matrix loads are inside the game's static lists, so
    // the top level's first one is the course, and it must be created under
    // RT64's defaults. Segment 6 having a base at all is what says the sky is
    // about to be drawn. If any of that does not hold, the section simply
    // never opens and RT64 behaves as it did.
    //
    // WR64_NO_SKY_INTERP=1 switches it off, for comparison.

    static constexpr uint32_t kSkyId = 0x57A00001u;
    static constexpr uint32_t kWaterId = 0x57A00002u;

    enum class SkySection { Before, Open, Done };
    SkySection sky = SkySection::Before;
    bool sky_interp = true;

    // VR: the sky's three draws replaced by the same three round the camera
    // (patches/sky_all_round.cpp), decided at the first of them: the call of
    // 0x010103F0 (the water's floor), the gradient's own load of 0x060000E0
    // at the top level and its call of 0x010103B8, and the call of
    // 0x01010470 (the clouds). Each replacement is a call of its own list
    // with segment 6 moved to it for the call.
    static constexpr uint32_t kSkyFloorList = 0x010103F0;
    static constexpr uint32_t kSkyGradientVertices = 0x060000E0;
    static constexpr uint32_t kSkyGradientList = 0x010103B8;
    static constexpr uint32_t kSkyCloudList = 0x01010470;
    bool sky_tried = false;
    bool sky_replaced = false;
    wr64::SkyLists sky_lists{};

    // VR, the results and course-select screens: the full grid of waves it draws,
    // ringed out to the distance (patches/water_all_round.cpp), once a frame,
    // first needed by the sky, which is built over it, and then drawn by the
    // water's call; the segment base and list, or 0.
    static constexpr uint32_t kSkyFirstList = 0x010102D0;
    bool full_grid_tried = false;
    uint32_t full_grid_base = 0, full_grid_list = 0;
    bool ring_full_grid() {
        if (!full_grid_tried) {
            full_grid_tried = true;
            if (!wr64::vr::all_round() ||
                !wr64::water_full_grid_ringed(rdram, segments[3], &full_grid_base, &full_grid_list)) {
                full_grid_list = 0;
            }
        }
        return full_grid_list != 0;
    }

    // Whether a top-level call draws part of the sky, and so was replaced.
    // The results screen draws only the gradient, beginning with a call of
    // 0x010102D0 (a matrix load) rather than of the floor's list, and only
    // its gradient is replaced, since the floor's and the clouds' textures
    // are not set up for them there.
    bool replace_sky_call(uint32_t list) {
        const bool results = wr64::water::full_grid_screen(state);
        if (!sky_tried && (list == kSkyFloorList || (results && list == kSkyFirstList))) {
            sky_tried = true;
            sky_replaced = results ? ring_full_grid() && wr64::sky_results_dome(rdram, segments[3], segments[6], &sky_lists)
                                   : wr64::sky_all_round(rdram, segments[3], segments[6], &sky_lists);
        }
        // A course with a seafloor of its own (src/seafloor.cpp) draws it in
        // place of the game's flat floor at the water's level, which would
        // otherwise hide it, in a state of its own that RT64 puts back after.
        if (list == kSkyFloorList && racing(state)) {
            uint32_t identity = 0;
            const uint32_t course = *reinterpret_cast<const uint32_t*>(rdram + (kCourseId & 0x00FFFFFFu));
            // (A course whose floor shows only from below the waves: only
            // with the first-person eye there, haze_list set at the view.)
            const bool shown = !wr64::seafloor_only_below(course) || haze_list != 0;
            const uint32_t floor = shown ? wr64::seafloor_list(rdram, course, segments, &identity) : 0;
            if (floor != 0) {
                const uint32_t ex = uint32_t(RT64_EXTENDED_OPCODE) << 24;
                emit(ex | G_EX_PUSHGEOMETRYMODE_V1, 0);
                emit(ex | G_EX_PUSHOTHERMODE_V1, 0);
                emit(ex | G_EX_PUSHCOMBINE_V1, 0);
                // Out of the sky's group while they are drawn: RT64 pairs
                // that group's vertices from one frame to the next in order,
                // and the floor's chunks round the camera and the fish, more
                // or fewer each frame, put the sky's vertices out of step --
                // they flashed between the game's frames in the headset.
                const bool in_sky = sky == SkySection::Open;
                if (in_sky) vertex_interp_group(G_EX_ID_AUTO);
                emit((uint32_t(kOpMtx) << 24) | ((kMtxLoad | kMtxPush) << 16) | 0x40, identity);
                emit(uint32_t(kOpDisplayList) << 24, floor);
                // And the fish swimming about the camera, over it.
                static std::vector<std::pair<uint32_t, uint32_t>> field;
                field.clear();
                wr64::fish_field(rdram, segments, course, field);
                for (const auto& [a, b] : field) emit(a, b);
                emit(0xBD000000u, 0);                           // pop the modelview
                if (in_sky) vertex_interp_group(kSkyId);
                emit(ex | G_EX_POPCOMBINE_V1, 0);
                emit(ex | G_EX_POPOTHERMODE_V1, 0);
                emit(ex | G_EX_POPGEOMETRYMODE_V1, 0);
                return true;
            }
        }
        if (!sky_replaced) {
            // The game's own gradient, a tent placed over the water ahead,
            // is drawn from both sides: a camera skimming the water from
            // outside it -- the title's opening, the cameras after a race --
            // otherwise sees only faces turned away, and so no sky at all.
            if (list != kSkyGradientList) return false;
            const uint32_t ex = uint32_t(RT64_EXTENDED_OPCODE) << 24;
            emit(ex | G_EX_PUSHGEOMETRYMODE_V1, 0);
            emit(0xB6000000u, 0x00003000u);                 // clear culling of either side
            emit(uint32_t(kOpDisplayList) << 24, list);
            emit(ex | G_EX_POPGEOMETRYMODE_V1, 0);
            return true;
        }
        const uint32_t own = list == kSkyFloorList ? sky_lists.floor
                           : list == kSkyGradientList ? sky_lists.gradient
                           : list == kSkyCloudList ? sky_lists.clouds : 0;
        if (own == 0) return false;
        emit((uint32_t(kOpMoveWord) << 24) | ((6u * 4u) << 8) | kMoveWordSegment, sky_lists.base);
        emit(uint32_t(kOpDisplayList) << 24, own);
        emit((uint32_t(kOpMoveWord) << 24) | ((6u * 4u) << 8) | kMoveWordSegment, segments[6]);
        return true;
    }

    // The group both the sky and the water are drawn under, and the one that
    // puts RT64's defaults back afterwards. Only the vertex and
    // texture-coordinate components differ from those defaults; every other
    // field is passed as RT64 would have set it, so that closing a section
    // restores the renderer's own behaviour exactly.
    //
    // G_EX_COMPONENT_INTERPOLATE rather than G_EX_COMPONENT_AUTO for the
    // texture coordinates: automatic means "only when the positions did not
    // change", which is the pure texture scroll of a waterfall, and both of
    // these move their positions.
    //
    // Each section is given an explicit transform id rather than G_EX_ID_AUTO,
    // with linear ordering, and that is not decoration. Both the sky and the
    // water are drawn under the game's identity matrix, and identical matrices
    // are exactly what ties a matcher that pairs by position: RT64 was
    // measured leaving them unpaired in some frames. A transform that finds no
    // pair never reaches matchTransform, and it is matchTransform that
    // computes the per-vertex velocities both sections depend on, so a tie lost
    // meant the sky quietly falling back to the game's rate for a frame. An
    // explicit id is matched first and by identity, before any of that
    // (GameFrame::match), and several transforms sharing one id pair up in the
    // order they were submitted, which is what the sky's two want.
    //
    // The values only have to be distinct from each other and from RT64's
    // G_EX_ID_IGNORE (0) and G_EX_ID_AUTO (~0); nothing else in this port
    // gives a transform an id.
    void vertex_interp_group(uint32_t id) {
        const bool on = id != G_EX_ID_AUTO;
        const uint32_t v = on ? G_EX_COMPONENT_INTERPOLATE : G_EX_COMPONENT_SKIP;
        const uint32_t order = on ? G_EX_ORDER_LINEAR : G_EX_ORDER_AUTO;
        if (GfxCommand* cmd = reserve(2)) {
            gEXMatrixGroup(cmd, id, G_EX_INTERPOLATE_DECOMPOSE, G_EX_NOPUSH, 0,
                           G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO,
                           G_EX_COMPONENT_AUTO, G_EX_COMPONENT_AUTO, v,
                           G_EX_COMPONENT_AUTO, order, G_EX_EDIT_NONE,
                           G_EX_ASPECT_AUTO, v, G_EX_COMPONENT_AUTO);
        }
    }

    // Called for every matrix command the top-level list carries, after the
    // walk has taken account of it and before the command itself is written
    // out. A projection multiply -- the camera, which follows the projection
    // load -- is neither end of the section and is passed over.
    void sky_matrix(uint32_t params) {
        if (!sky_interp) return;
        if (params & kMtxProjection) {
            if ((params & kMtxLoad) && sky == SkySection::Before &&
                projection_is_perspective && segments[6] != 0) {
                vertex_interp_group(kSkyId);
                sky = SkySection::Open;
            }
            return;
        }
        sky_close();
    }

    void sky_close() {
        if (sky == SkySection::Open) {
            vertex_interp_group(G_EX_ID_AUTO);
            sky = SkySection::Done;
        }
    }

    // ---- the water ------------------------------------------------------
    //
    // The waves are the sky's problem again, in the place where it is most
    // worth solving. The water surface is a lattice of rows marching away
    // from the camera, fifty vertex blocks reached through segment 3 under
    // the same identity matrix the sky uses, and the game recomputes it every
    // frame: measured across four consecutive race frames, forty-one of the
    // fifty blocks carry different vertex data each time while the course and
    // the models (segments 1, 8 and 13) are byte-for-byte identical. So the
    // waves moved at the game's twenty or thirty frames a second under a
    // camera gliding at sixty.
    //
    // The lattice recenters around the camera in roughly 64-unit steps.
    // Stable vertex counts do not imply stable world positions: the same
    // array slot can refer to a different wave slope after a recenter. The
    // material-tagged renderer therefore samples the previous water surface
    // at each current XZ before interpolating heights and reflection UVs.
    // This keeps the waves moving smoothly at the presentation rate without
    // dragging the grid or changing the game's authoritative wave simulation.
    //
    // The whole surface is drawn from one of the game's static lists, so the
    // group can be put around the call rather than threaded through the list:
    // RT64 does not create a world transform when a matrix is loaded but when
    // the first vertex after it is, so a group set before the call is the one
    // the water's transform is created under, and a group restoring the
    // defaults after the call leaves everything else as it was.
    //
    // WR64_NO_WATER_INTERP=1 switches it off, for comparison.

    bool water_interp = true;

    // The segment a called list draws its first vertices from, or 0 for a
    // list that draws none. The game's drawing lists are static and are
    // called by the same segmented address every frame, so each is scanned
    // once and answered from the table afterwards.
    std::unordered_map<uint64_t, uint32_t> water_segment_cache;
    uint32_t first_vertex_segment(uint32_t segmented) {
        auto &cache = water_segment_cache;
        const uint64_t key = (uint64_t(segmented) << 32) | physical(segmented);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        const uint32_t found = scan_first_vertex_segment(physical(segmented), 0);
        cache.emplace(key, found);
        return found;
    }

    uint32_t scan_first_vertex_segment(uint32_t physical_addr, int depth) const {
        uint32_t cursor = physical_addr;
        for (uint32_t steps = 0; steps < 512; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            if (op == kOpEndDisplayList) return 0;
            if (op == kOpVtx) return (w1 >> 24) & 0xF;
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) { cursor = physical(w1); continue; }
                if (depth < 3) {
                    const uint32_t found = scan_first_vertex_segment(physical(w1), depth + 1);
                    if (found != 0) return found;
                }
            }
        }
        return 0;
    }

    // Whether a call about to be written out draws the water, and so should
    // be wrapped. Segment 3 is where the game builds the frame's own
    // geometry; in a race frame the whole of it is the water surface.
    static constexpr uint32_t kWaterSegment = 3;

    bool draws_water(uint32_t segmented_list) {
        return projection_is_perspective &&
               first_vertex_segment(segmented_list) == kWaterSegment;
    }

    bool known_water_material(uint32_t list) const {
        // USA Rev A Draw_WaterEffects explicitly selects these four lists for
        // the full grid, the opening/rider scene, and the two player views.
        // Other segment-3 geometry (including craft effects) keeps its material.
        return list==0x010082F0 || list==0x0100B590 || list==0x0100D258 || list==0x0100E680;
    }

    void water_material(bool enabled) {
        const uint32_t op = (RT64_EXTENDED_OPCODE << 24) | G_EX_WATER_MATERIAL_V1;
        const auto m = wr64::water::material(0);
        if (!enabled || m.identity.x == 0) { emit(op, 0); return; }
        if (GfxCommand *commands = reserve(1 + sizeof(m) / 8)) {
            auto *data = reinterpret_cast<uint32_t *>(commands);
            data[0] = op; data[1] = 2; // Water payload v2 appends effect preferences.
            std::memcpy(data + 2, &m, sizeof(m));
        }
    }

    // WR64_WATER_DUMP=<file>: one frame's water list and the vertices it loads,
    // for the game's mesh (and any copies given) or the merged one, to check them.
    void dump_water(uint32_t list, const uint32_t bases[], int copies, int depth, FILE* f) {
        uint32_t cursor = physical(list);
        for (uint32_t steps = 0; steps < 16384; ++steps) {
            const uint32_t w0 = words(cursor)[0], w1 = words(cursor)[1];
            cursor += 8;
            const uint8_t op = uint8_t(w0 >> 24);
            std::fprintf(f, "cmd %08X %08X\n", w0, w1);
            if (op == kOpEndDisplayList) return;
            if (op == kOpDisplayList) {
                if (((w0 >> 16) & 0xFF) != 0) { cursor = physical(w1); continue; }
                if (depth < 3) dump_water(w1, bases, copies, depth + 1, f);
                continue;
            }
            if (op != kOpVtx) continue;
            const uint32_t count = (w0 >> 9) & 0x7F;
            for (int m = 0; m <= copies; ++m) {
                const uint32_t base = m == 0 ? segments[(w1 >> 24) & 0xF] : bases[m - 1];
                const uint32_t addr = (base + (w1 & 0x00FFFFFFu)) & 0x00FFFFF8u;
                for (uint32_t i = 0; i < count && i < 64; ++i) {
                    const uint32_t v = addr + i * 16;
                    std::fprintf(f, "v %d %u %d %d %d %d %d %u %u %u %u\n", m, i,
                                 signed_half(v), signed_half(v + 2), signed_half(v + 4),
                                 signed_half(v + 8), signed_half(v + 10),
                                 rdram[(v + 12) ^ 3], rdram[(v + 13) ^ 3], rdram[(v + 14) ^ 3], rdram[(v + 15) ^ 3]);
                }
            }
        }
    }

    // ---- rectangles -----------------------------------------------------
    //
    // The race HUD is drawn as texture rectangles, which RT64 anchors through
    // a rect-alignment state that applies to every plain rectangle that
    // follows: no command has to be reissued, and nothing is interpolated.
    // The offsets undo RT64's own displacement for the origin, as for
    // viewports, and add the inset.

    Class rect_cls = Class::Auto;

    // A run: consecutive rectangles on one row, close together, which are one
    // element -- the digits of a time, the markers of a row -- and must be
    // classified as one, or a number is torn at the boundary of a zone. The
    // rectangles and the texture-setup commands between them are held back
    // until the run ends, then classified and written out together.
    std::vector<std::pair<uint32_t, uint32_t>> pending;
    std::vector<uint32_t> pending_textures;
    Extent pending_extent;
    bool pending_active = false;

    static bool run_command(uint8_t op) {
        switch (op) {
            case kOpTexRect: case kOpTexRectFlip: case kOpFillRect:
            case 0xFD: case 0xF5: case 0xF2: case 0xF3: // texture image, tile, tile size, load block
            case 0xF4: case 0xE6: case 0xE7: case 0xE8: // load tile, syncs
            case 0xE9: case 0xFA: case 0xFB: case 0xFC: // sync, colours, combine
            case 0xB3: case 0xB2: case 0xB4:            // RDP halves
            case 0xB9: case 0xBA:                       // other modes
            case 0xF7:                                  // fill colour
                return true;
            default:
                return false;
        }
    }

    static bool chains(const Extent& run, const Extent& r) {
        // Same row (the vertical extents overlap) and no more than 12 pixels apart.
        const float gap = 12.0f / 160.0f;
        const bool overlap_y = r.min_y <= run.max_y && r.max_y >= run.min_y;
        const bool near_x = r.min_x <= run.max_x + gap && r.max_x >= run.min_x - gap;
        return overlap_y && near_x;
    }

    void push_rect(uint32_t w0, uint32_t w1) {
        const Extent r = rect_extent(w0, w1);
        if (pending_active && !chains(pending_extent, r)) {
            flush_run();
        }
        if (!pending_active) {
            pending_active = true;
            pending_extent = r;
            pending_textures.clear();
            pending_textures.push_back(texture);
        }
        else {
            pending_extent.add(r);
            if (std::find(pending_textures.begin(), pending_textures.end(), texture) == pending_textures.end()) {
                pending_textures.push_back(texture);
            }
        }
        pending.emplace_back(w0, w1);
    }

    void flush_run() {
        if (pending_active) {
            classify_rect(pending_extent, 0, pending_textures);
        }
        for (const auto& [w0, w1] : pending) {
            emit(w0, w1);
        }
        pending.clear();
        pending_active = false;
    }

    // The game's scissor is the 4:3 frame, and RT64 keeps it there for the
    // 2D pass, so an anchored rectangle would be cut off at the frame's old
    // edge. While rectangles are anchored, the scissor's left edge is anchored
    // to the frame's left edge and its right edge to the right edge, and the
    // game's own scissor command is reissued so that it takes effect.
    void widen_scissor(bool widen) {
        if (widen == scissor_widened || !have_scissor_cmd) return;
        scissor_widened = widen;
        if (widen) {
            // A scissor spanning the whole widened frame, with the game's own
            // vertical bounds and mode: its left edge anchored to the frame's
            // left edge and its right edge to the right, so an anchored
            // element is not cut off at the 4:3 boundary.
            //
            // This is a scissor command, not an alignment. The first attempt
            // used gEXSetScissorAlign, which is persistent state that applies
            // to every plain scissor the game sets afterwards -- including the
            // ones its 3D pass sets on the next frame. That is what put black
            // bars around races and stopped the pause screen drawing.
            // The coordinates are given relative to the origin each edge is
            // anchored to, because RT64 displaces them by the origin when it
            // stores the scissor: the left edge by nothing, the right edge by
            // the framebuffer's width. Passing the game's own numbers less
            // that displacement leaves the stored rectangle exactly as the
            // game set it, so every test RT64 makes against it -- including
            // whether the 3D pass covers the frame, and so whether it is
            // widened at all -- sees no change. Only the origins differ, and
            // those are what spread the scissor across the widened frame when
            // it is drawn. Giving the widened rectangle directly instead cost
            // the 3D pass its widescreen and put black bars beside the race.
            const uint8_t mode = static_cast<uint8_t>((scissor_w1 >> 24) & 3);
            const int ulx = static_cast<int>((scissor_w0 >> 12) & 0xFFF) >> 2;
            const int uly = static_cast<int>(scissor_w0 & 0xFFF) >> 2;
            const int lrx = static_cast<int>((scissor_w1 >> 12) & 0xFFF) >> 2;
            const int lry = static_cast<int>(scissor_w1 & 0xFFF) >> 2;
            if (GfxCommand* cmd = reserve(2)) {
                gEXSetScissor(cmd, mode, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_RIGHT,
                              ulx, uly, lrx - kFramebufferWidth, lry);
            }
        }
        else {
            // Restored by the game's own command, which carries no origins.
            emit(scissor_w0, scissor_w1);
        }
    }

    void set_rect_class(Class next, const Extent& e = Extent{}) {
        if (next == rect_cls) return;
        (void)e;
        widen_scissor(next == Class::Left || next == Class::Right);
        if (rect_cls == Class::Stretch) {
            if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_AUTO);
        }
        const int inset_q = static_cast<int>(inset * 4.0f);
        switch (next) {
            case Class::Left:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_LEFT, G_EX_ORIGIN_LEFT, inset_q, 0, inset_q, 0);
                }
                break;
            case Class::Right: {
                const int off = origin_cancel(G_EX_ORIGIN_RIGHT) - inset_q;
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_RIGHT, G_EX_ORIGIN_RIGHT, off, 0, off, 0);
                }
                break;
            }
            case Class::Stretch:
                // Widen the element by however much the frame was widened,
                // about its centre. Pinning its own edges to the frame's
                // edges instead was tried and reverted: it made no difference
                // to the MAX POWER banner, whose last letter is clipped by
                // the game's own rectangle rather than by the frame, and it
                // undid the select screens' backgrounds, which this gets
                // right.
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
                }
                if (GfxCommand* cmd = reserve(1)) gEXSetRectAspect(cmd, G_EX_ASPECT_STRETCH);
                break;
            default:
                if (GfxCommand* cmd = reserve(2)) {
                    gEXSetRectAlign(cmd, G_EX_ORIGIN_NONE, G_EX_ORIGIN_NONE, 0, 0, 0, 0);
                }
                break;
        }
        rect_cls = next;
        ++class_changes;
    }

    // A rectangle's extent in the same clip units as a vertex draw's, so the
    // one classifier serves both. Coordinates arrive in 10.2 fixed point.
    static Extent rect_extent(uint32_t w0, uint32_t w1) {
        const float ulx = float((w1 >> 12) & 0xFFF) / 4.0f;
        const float uly = float(w1 & 0xFFF) / 4.0f;
        const float lrx = float((w0 >> 12) & 0xFFF) / 4.0f;
        const float lry = float(w0 & 0xFFF) / 4.0f;
        Extent e;
        e.min_x = ulx / 160.0f - 1.0f;
        e.max_x = lrx / 160.0f - 1.0f;
        e.min_y = 1.0f - lry / 120.0f;
        e.max_y = 1.0f - uly / 120.0f;
        return e;
    }

    void classify_rect(const Extent& e, uint32_t dl_segmented, const std::vector<uint32_t>& textures) {
        ++draws_2d;
        const std::string tex_id = hex_identity("tex", textures.empty() ? texture : textures.front());
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        Class next = classify(e, tex_id, dl_id);
        // A tag on any texture of the run applies to the run.
        for (uint32_t t : textures) {
            Class tagged;
            if (tags != nullptr && tags->lookup(hex_identity("tex", t), tagged)) {
                next = (tagged == Class::Stretch || anchors) ? tagged : Class::Auto;
                break;
            }
        }
        if (trace != nullptr) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "[hud] state 0x%02X rect %s %s px x[%.0f..%.0f] y[%.0f..%.0f] under %s -> %s" "\n",
                          state, tex_id.c_str(), dl_id.empty() ? "dl:-" : dl_id.c_str(),
                          (e.min_x + 1.0f) * 160.0f, (e.max_x + 1.0f) * 160.0f,
                          (1.0f - e.max_y) * 120.0f, (1.0f - e.min_y) * 120.0f,
                          projection_is_perspective ? "persp" : "ortho", class_name(next));
            trace->append(line);
        }
        // Every texture of the run, not just the run's identity: a run is a
        // set of rectangles that were classified together, and which
        // rectangles chain into one run changes as the elements move, so an
        // element can be stretched one frame and centred the next without the
        // run it was in ever being reported as having changed class.
        if (textures.empty()) {
            report_flip(tex_id, next);
        }
        else {
            for (uint32_t t : textures) report_flip(hex_identity("tex", t), next);
        }
        report_flip(dl_id, next);
        if (!noemit) set_rect_class(next, e);
    }

    // Whether a draw spans the frame from side to side, and so should be
    // stretched across the widened one.
    //
    // The test is on width alone, against the width of the region the game
    // draws into, and asks for nine tenths of it rather than the whole thing.
    //
    // Neither looseness is arbitrary. Height is out because the pause screen's
    // dim is drawn as horizontal strips, each a slice of the height. And the
    // overlays do not sit where a full-screen overlay might be expected to: the
    // dim spans columns 32 to 336 of a 320-wide screen -- offset right, and
    // over the edge at the far end -- while the tint over a race spans 9 to
    // 310. Demanding that a draw reach both edges of the drawn region rejected
    // the dim, which was measured off a capture of the pause screen: it left
    // an undimmed band 267 window pixels wide down the left, exactly where an
    // unstretched rectangle starting at column 32 lands. Nine tenths of the
    // width takes both overlays and still refuses the widest HUD element,
    // which reaches five sixths.
    bool covers_width(const Extent& e) const {
        const float left = has_scissor() ? float(scissor_left) : 0.0f;
        const float right = has_scissor() ? float(scissor_right) : float(kFramebufferWidth);
        return (e.right_px() - e.left_px()) >= 0.9f * (right - left);
    }

    Class classify(const Extent& e, const std::string& identity, const std::string& identity2) {
        Class tagged;
        if (tags != nullptr && (tags->lookup(identity, tagged) || tags->lookup(identity2, tagged))) {
            if (tagged == Class::Stretch || anchors) return tagged;
            return Class::Auto;
        }
        if (e.empty()) return Class::Auto;
        // Stretching is for the 2D layer's own full-screen elements, and those
        // are drawn under an orthographic projection: the tint over a race,
        // the pause screen's dim, the fades between screens, the menus'
        // backgrounds. Measured across the title, the main menu, the options
        // and name-entry screens and a race, every one of them is orthographic.
        //
        // A rectangle that covers the frame under a perspective projection is
        // a different thing -- it belongs to the 3D pass, which RT64 has
        // already drawn at the frame's full width -- and stretching it scales
        // the picture instead of widening an overlay. The intro is where that
        // shows: the game composes it from full-frame rectangles under the
        // world's own projection, present in some of its camera shots and not
        // others, so the picture jumped between its proper width and a
        // magnified one from shot to shot. Measured on the Wave Race logo in
        // the corner, which the stretch dragged towards the frame's edge: 420
        // window pixels wide in a shot without one, 524 in a shot with, at the
        // same height.
        if (covers_width(e) && !projection_is_perspective) return Class::Stretch;
        // Anchoring is for the race HUD, which is drawn under an orthographic
        // projection in a frame that drew its world first. A menu's layout is
        // 4:3 by design and stays so whatever the setting says.
        if (!anchors || projection_is_perspective || !race_test.race()) return Class::Auto;
        // A HUD element sits inside the frame. Anything reaching beyond it is
        // a world object drawn through an orthographic projection, and moving
        // it with an edge would tear it from its neighbours.
        if (e.min_x < -1.0f || e.max_x > 1.0f || e.min_y < -1.0f || e.max_y > 1.0f) return Class::Auto;
        const float c = e.center_x();
        if (c < -kAnchorThird) return Class::Left;
        if (c > kAnchorThird) return Class::Right;
        return Class::Auto;
    }

    // A 2D element that is given one class in one frame and another in the
    // next flickers between them, and that is a defect however defensible
    // each classification is on its own. Under WR64_HUD_TRACE it is reported
    // as it happens, with both classes, so the element can be named and
    // pinned in hud.json.
    void report_flip(const std::string& identity, Class next) {
        if (trace == nullptr || identity.empty()) return;
        static std::unordered_map<std::string, Class> last;
        const auto it = last.find(identity);
        if (it == last.end()) {
            last.emplace(identity, next);
            return;
        }
        if (it->second == next) return;
        std::fprintf(stderr, "[hud] flip: state 0x%02X %s %s -> %s\n", state, identity.c_str(),
                     class_name(it->second), class_name(next));
        std::fflush(stderr);
        it->second = next;
    }

    static std::string hex_identity(const char* kind, uint32_t address) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s:0x%08X", kind, address);
        return buf;
    }

    void classify_draw(const Extent& e, uint32_t dl_segmented) {
        const std::string tex_id = hex_identity("tex", texture);
        const std::string dl_id = dl_segmented != 0 ? hex_identity("dl", dl_segmented) : std::string{};
        const Class next = classify(e, tex_id, dl_id);
        if (trace != nullptr) {
            char line[200];
            std::snprintf(line, sizeof(line),
                          "[hud] state 0x%02X %s %s clip x[%.2f..%.2f] y[%.2f..%.2f] -> %s\n",
                          state, tex_id.c_str(), dl_id.empty() ? "dl:-" : dl_id.c_str(),
                          e.empty() ? 0.0f : e.min_x, e.empty() ? 0.0f : e.max_x,
                          e.empty() ? 0.0f : e.min_y, e.empty() ? 0.0f : e.max_y,
                          class_name(next));
            trace->append(line);
        }
        report_flip(dl_id.empty() ? tex_id : dl_id, next);
        if (!noemit) set_class(next);
    }

    // A read-only scan of a called list for its extent and the texture it
    // sets, so a HUD element drawn from a static list can be classified. The
    // modelview stack is copied, since the list may push and pop its own.
    Extent scan(uint32_t physical_addr, int depth, Mat4 mv[16], int& mv_depth, Extent& rects) {
        Extent e;
        uint32_t cursor = physical_addr;
        for (uint32_t steps = 0; steps < 4096; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            if (op == kOpEndDisplayList) break;
            if (op == kOpDisplayList) {
                const bool branch = ((w0 >> 16) & 0xFF) != 0;
                if (branch) { cursor = physical(w1); continue; }
                if (depth < 4) e.add(scan(physical(w1), depth + 1, mv, mv_depth, rects));
                continue;
            }
            if (op == kOpSetTextureImage) { texture = w1; continue; }
            if (is_rect(op)) { rects.add(rect_extent(w0, w1)); continue; }
            if (op == kOpPopMtx) { if (mv_depth > 0) --mv_depth; continue; }
            if (op == kOpMtx) {
                const uint32_t params = (w0 >> 16) & 0xFF;
                if (params & kMtxProjection) continue;    // rare in a called list; ignored
                const Mat4 loaded = read_matrix(physical(w1));
                if ((params & kMtxPush) && mv_depth < 15) { mv[mv_depth + 1] = mv[mv_depth]; ++mv_depth; }
                mv[mv_depth] = (params & kMtxLoad) ? loaded : loaded * mv[mv_depth];
                continue;
            }
            if (op == kOpVtx) {
                const uint32_t count = (w0 >> 9) & 0x7F;
                e.add(extent(physical(w1), count, mv[mv_depth]));
            }
        }
        return e;
    }

    // ---- state tracking -------------------------------------------------

    // Whether a matrix is affine: no perspective terms in its last column.
    static bool affine(const Mat4& m) {
        return m.m[0][3] == 0.0f && m.m[1][3] == 0.0f && m.m[2][3] == 0.0f && m.m[3][3] == 1.0f;
    }

    static bool identity(const Mat4& m) {
        const Mat4 i = Mat4::identity();
        return std::memcmp(&m, &i, sizeof(Mat4)) == 0;
    }

    void on_matrix(uint32_t w0, uint32_t w1) {
        const uint32_t params = (w0 >> 16) & 0xFF;
        const Mat4 loaded = read_matrix(physical(w1));
        if (params & kMtxProjection) {
            // As RT64 keeps it: the product of everything on the projection
            // stack is what transforms vertices, but an affine matrix
            // multiplied onto the stack is the camera, not the projection, and
            // the projection's type is judged from the projection alone. The
            // game loads its perspective projection and then multiplies the
            // camera onto it; the product has a non-zero [3][3] and would read
            // as orthographic.
            if (params & kMtxLoad) {
                projection = loaded;
                projection_only = loaded;
                have_projection = true;
                projection_w0 = w0;
                projection_w1 = w1;
            }
            else {
                projection = loaded * projection;
                if (!affine(loaded)) {
                    projection_only = loaded * projection_only;
                }
                else if (!have_camera && perspective(projection_only) && !identity(loaded)) {
                    have_camera = true;
                    camera = loaded;
                    camera_lens = projection_only;
                }
            }
            projection_is_perspective = perspective(projection_only);
            if (!race_test.first_projection_seen) {
                race_test.first_projection_seen = true;
                race_test.world_first = projection_is_perspective;
            }
            return;
        }
        if ((params & kMtxPush) && modelview_depth < 15) {
            modelview[modelview_depth + 1] = modelview[modelview_depth];
            ++modelview_depth;
        }
        modelview[modelview_depth] = (params & kMtxLoad) ? loaded : loaded * modelview[modelview_depth];
    }

    void on_scissor(uint32_t w0, uint32_t w1) {
        const int ulx = static_cast<int>((w0 >> 12) & 0xFFF) >> 2;
        const int uly = static_cast<int>(w0 & 0xFFF) >> 2;
        const int lrx = static_cast<int>((w1 >> 12) & 0xFFF) >> 2;
        const int lry = static_cast<int>(w1 & 0xFFF) >> 2;
        if (ulx == 0 && uly == 0 && lrx >= 319 && lry >= 239) return;
        race_test.inset_scissor = true;
        scissor_left = std::min(scissor_left, ulx);
        scissor_top = std::min(scissor_top, uly);
        scissor_right = std::max(scissor_right, lrx);
        scissor_bottom = std::max(scissor_bottom, lry);
    }

    bool has_scissor() const {
        return scissor_right > scissor_left && scissor_bottom > scissor_top;
    }
};


// ---- the 3D trace ------------------------------------------------------
//
// Diagnosis only, and read-only: WR64_3D_TRACE names a file, and the first
// few race frames' drawing is written to it in full -- every matrix load,
// vertex load, call and triangle batch, with each vertex block's hash and
// its clip-space extent. Two consecutive frames diffed against each other
// say which geometry the game rebuilds from scratch every frame, which is
// the geometry RT64 has nothing to interpolate between unless it is told to
// interpolate the vertices themselves.
struct Tracer {
    uint8_t* rdram = nullptr;
    std::FILE* f = nullptr;
    uint32_t segments[16] = {};
    Mat4 projection = Mat4::identity();
    Mat4 modelview[16];
    int modelview_depth = 0;
    uint32_t texture = 0;
    uint32_t vtx_loads = 0;

    Tracer() { modelview[0] = Mat4::identity(); }

    uint32_t physical(uint32_t segmented) const {
        return (segments[(segmented >> 24) & 0xF] + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }
    const uint32_t* words(uint32_t addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (addr & 0x00FFFFFFu));
    }
    uint16_t half(uint32_t addr) const {
        return *reinterpret_cast<const uint16_t*>(rdram + ((addr & 0x00FFFFFFu) ^ 2));
    }
    int16_t signed_half(uint32_t addr) const { return static_cast<int16_t>(half(addr)); }

    Mat4 read_matrix(uint32_t addr) const {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            const uint32_t full = (uint32_t(half(addr + 2 * i)) << 16) | half(addr + 32 + 2 * i);
            r.m[i / 4][i % 4] = static_cast<float>(static_cast<int32_t>(full)) / 65536.0f;
        }
        return r;
    }

    // FNV-1a over a block of RDRAM, so a frame-to-frame diff says at a glance
    // whether the game rewrote it.
    uint32_t hash(uint32_t addr, uint32_t bytes) const {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < bytes; ++i) {
            h = (h ^ rdram[((addr + i) & 0x00FFFFFFu) ^ 3]) * 16777619u;
        }
        return h;
    }

    void walk(uint32_t cursor, int depth) {
        for (uint32_t steps = 0; steps < 8192; ++steps) {
            const uint32_t* c = words(cursor);
            const uint32_t w0 = c[0];
            const uint32_t w1 = c[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            // WR64_3D_TRACE_RAW=1 also prints every command's two words.
            static const bool raw = std::getenv("WR64_3D_TRACE_RAW") != nullptr;
            if (raw) std::fprintf(f, "%*s[%08X %08X]\n", depth * 2, "", w0, w1);

            switch (op) {
                case kOpEndDisplayList:
                    std::fprintf(f, "%*send\n", depth * 2, "");
                    return;
                case kOpDisplayList: {
                    const bool branch = ((w0 >> 16) & 0xFF) != 0;
                    std::fprintf(f, "%*s%s 0x%08X (phys 0x%06X)\n", depth * 2, "",
                                 branch ? "branch" : "call", w1, physical(w1));
                    if (branch) { cursor = physical(w1); continue; }
                    if (depth < 6) {
                        const int saved = modelview_depth;
                        walk(physical(w1), depth + 1);
                        modelview_depth = saved;
                    }
                    continue;
                }
                case kOpMoveWord:
                    if ((w0 & 0xFF) == kMoveWordSegment) {
                        segments[(w0 >> 10) & 0xF] = w1 & 0x00FFFFFFu;
                        std::fprintf(f, "%*sseg %u = 0x%06X\n", depth * 2, "",
                                     (w0 >> 10) & 0xF, w1 & 0x00FFFFFFu);
                    }
                    continue;
                case kOpPopMtx:
                    if (modelview_depth > 0) --modelview_depth;
                    std::fprintf(f, "%*spop -> depth %d\n", depth * 2, "", modelview_depth);
                    continue;
                case kOpMtx: {
                    const uint32_t params = (w0 >> 16) & 0xFF;
                    const uint32_t addr = physical(w1);
                    const Mat4 loaded = read_matrix(addr);
                    if (params & kMtxProjection) {
                        projection = (params & kMtxLoad) ? loaded : loaded * projection;
                        std::fprintf(f, "%*sproj %s 0x%08X (phys 0x%06X) hash %08X"
                                        " [1][1]=%.4f [3][3]=%.4f t=(%.1f,%.1f,%.1f)\n",
                                     depth * 2, "", (params & kMtxLoad) ? "load" : "mul", w1, addr,
                                     hash(addr, 64), loaded.m[1][1], loaded.m[3][3],
                                     loaded.m[3][0], loaded.m[3][1], loaded.m[3][2]);
                        continue;
                    }
                    if ((params & kMtxPush) && modelview_depth < 15) {
                        modelview[modelview_depth + 1] = modelview[modelview_depth];
                        ++modelview_depth;
                    }
                    modelview[modelview_depth] =
                        (params & kMtxLoad) ? loaded : loaded * modelview[modelview_depth];
                    std::fprintf(f, "%*smtx %s%s 0x%08X (phys 0x%06X) hash %08X t=(%.1f,%.1f,%.1f)"
                                    " depth %d\n",
                                 depth * 2, "", (params & kMtxLoad) ? "load" : "mul",
                                 (params & kMtxPush) ? "+push" : "", w1, addr, hash(addr, 64),
                                 loaded.m[3][0], loaded.m[3][1], loaded.m[3][2], modelview_depth);
                    continue;
                }
                case kOpSetTextureImage:
                    texture = w1;
                    std::fprintf(f, "%*stimg 0x%08X\n", depth * 2, "", w1);
                    continue;
                case kOpSetScissor:
                    std::fprintf(f, "%*sscissor (%u,%u)-(%u,%u)\n", depth * 2, "", ((w0 >> 12) & 0xFFF) >> 2,
                                 (w0 & 0xFFF) >> 2, ((w1 >> 12) & 0xFFF) >> 2, (w1 & 0xFFF) >> 2);
                    continue;
                case kOpFillRect:
                    std::fprintf(f, "%*sfill rect (%u,%u)-(%u,%u)\n", depth * 2, "", ((w1 >> 12) & 0xFFF) >> 2,
                                 (w1 & 0xFFF) >> 2, ((w0 >> 12) & 0xFFF) >> 2, (w0 & 0xFFF) >> 2);
                    continue;
                case kOpTexRect:
                case kOpTexRectFlip:
                    std::fprintf(f, "%*stex rect (%u,%u)-(%u,%u) tex 0x%08X\n", depth * 2, "", ((w1 >> 12) & 0xFFF) >> 2,
                                 (w1 & 0xFFF) >> 2, ((w0 >> 12) & 0xFFF) >> 2, (w0 & 0xFFF) >> 2, texture);
                    continue;
                case 0xFF: case 0xFE: case 0xF7:
                    std::fprintf(f, "%*s%s 0x%08X\n", depth * 2, "",
                                 op == 0xFF ? "color image" : op == 0xFE ? "depth image" : "fill color", w1);
                    continue;
                case kOpMoveMem:
                    if (((w0 >> 16) & 0xFF) == kMoveMemViewport) {
                        const uint32_t vp = physical(w1);
                        std::fprintf(f, "%*sviewport 0x%08X scale (%.1f,%.1f) translate (%.1f,%.1f)\n", depth * 2, "",
                                     w1, signed_half(vp) / 4.0f, signed_half(vp + 2) / 4.0f,
                                     signed_half(vp + 8) / 4.0f, signed_half(vp + 10) / 4.0f);
                    }
                    continue;
                case kOpVtx: {
                    const uint32_t count = (w0 >> 9) & 0x7F;
                    const uint32_t addr = physical(w1);
                    const Mat4 mvp = modelview[modelview_depth] * projection;
                    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
                    for (uint32_t i = 0; i < count && i < 64; ++i) {
                        const uint32_t v = addr + i * 16;
                        const float x = signed_half(v + 0);
                        const float y = signed_half(v + 2);
                        const float z = signed_half(v + 4);
                        const float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
                        const float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
                        const float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];
                        if (cw <= 0.0f) continue;
                        min_x = std::min(min_x, cx / cw); max_x = std::max(max_x, cx / cw);
                        min_y = std::min(min_y, cy / cw); max_y = std::max(max_y, cy / cw);
                    }
                    std::fprintf(f, "%*svtx #%u 0x%08X (phys 0x%06X) n=%u hash %08X tex 0x%08X"
                                    " v0=(%d,%d,%d) clip x[%.2f..%.2f] y[%.2f..%.2f]\n",
                                 depth * 2, "", vtx_loads++, w1, addr, count, hash(addr, count * 16),
                                 texture, signed_half(addr), signed_half(addr + 2),
                                 signed_half(addr + 4),
                                 max_x < min_x ? 0.0f : min_x, max_x < min_x ? 0.0f : max_x,
                                 min_y > max_y ? 0.0f : min_y, min_y > max_y ? 0.0f : max_y);
                    // WR64_3D_TRACE_VERTS=1 also lists each vertex through the
                    // model matrix (world space), for mapping a course.
                    static const bool verts = std::getenv("WR64_3D_TRACE_VERTS") != nullptr;
                    if (verts) {
                        const Mat4& m = modelview[modelview_depth];
                        for (uint32_t i = 0; i < count && i < 64; ++i) {
                            const uint32_t v = addr + i * 16;
                            const float x = signed_half(v + 0), y = signed_half(v + 2), z = signed_half(v + 4);
                            std::fprintf(f, "%*s  v %.0f %.0f %.0f\n", depth * 2, "",
                                         x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0],
                                         x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1],
                                         x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2]);
                        }
                    }
                    continue;
                }
                default:
                    continue;
            }
        }
        std::fprintf(f, "%*s(walk cut off)\n", depth * 2, "");
    }
};

// Writes one race frame's drawing to the file named by WR64_3D_TRACE, for
// the first WR64_3D_TRACE_FRAMES frames (two by default), and reports when
// it is done.
void trace_3d(uint8_t* rdram, uint32_t list_vaddr, uint32_t state) {
    static const char* path = std::getenv("WR64_3D_TRACE");
    if (path == nullptr) return;
    // Race frames by default; WR64_3D_TRACE_STATE names another game state,
    // in hexadecimal, for the screens that are not races.
    static const char* state_env = std::getenv("WR64_3D_TRACE_STATE");
    // "all" writes one frame of every game state, 20 frames after it begins,
    // to the named file with the state added ("trace.txt" -> "trace.txt_36"):
    // for screens a script cannot reach, played through once by hand.
    static const bool every_state = state_env != nullptr && std::strcmp(state_env, "all") == 0;
    if (every_state) {
        static uint32_t current = ~0u;
        static int frames = 0;
        static bool traced[256] = {};
        if (state != current) { current = state; frames = 0; }
        if (++frames != 20 || state > 0xFF || traced[state]) return;
        traced[state] = true;
        char name[512];
        std::snprintf(name, sizeof(name), "%s_%02X", path, state);
        std::FILE* one = std::fopen(name, "w");
        if (one == nullptr) return;
        std::fprintf(one, "==== frame 1 (state 0x%02X, list 0x%08X) ====\n", state, list_vaddr);
        Tracer t{};
        t.rdram = rdram;
        t.f = one;
        t.walk(list_vaddr & 0x00FFFFFFu, 0);
        std::fclose(one);
        std::fprintf(stderr, "[wr64] 3D trace: state 0x%02X written to %s\n", state, name);
        std::fflush(stderr);
        return;
    }
    static const long wanted_state = state_env != nullptr ? std::strtol(state_env, nullptr, 16) : -1;
    if (wanted_state >= 0 ? (state != uint32_t(wanted_state)) : !racing(state)) return;
    // WR64_3D_TRACE_BUOY=n waits until the arrow points at buoy n (the
    // human's race record, 0x801C2938 + slot * 0x378, +0x0C) and then a
    // further 15 frames, for the moment a buoy goes missing in a played race.
    static const char* buoy_env = std::getenv("WR64_3D_TRACE_BUOY");
    if (buoy_env != nullptr) {
        static int after = -1;
        if (after < 0) {
            const int32_t slot = std::clamp(*reinterpret_cast<const int32_t*>(rdram + 0x0D48DC), 0, 3);
            const int32_t next = *reinterpret_cast<const int32_t*>(rdram + 0x1C2938 + slot * 0x378 + 0x0C);
            if (next != std::atoi(buoy_env)) return;
            after = 15;
        }
        if (after > 0) { --after; return; }
    }

    static const char* frames_env = std::getenv("WR64_3D_TRACE_FRAMES");
    static const int wanted = frames_env != nullptr ? std::atoi(frames_env) : 2;
    // WR64_3D_TRACE_SKIP passes over that many matching frames first.
    static const char* skip_env = std::getenv("WR64_3D_TRACE_SKIP");
    static int skip = skip_env != nullptr ? std::atoi(skip_env) : 0;
    if (skip > 0) { --skip; return; }
    static int written = 0;
    static std::FILE* f = nullptr;
    if (written >= wanted) return;
    if (f == nullptr) {
        f = std::fopen(path, "w");
        if (f == nullptr) {
            written = wanted;
            std::fprintf(stderr, "[wr64] 3D trace: cannot write %s\n", path);
            return;
        }
    }

    ++written;
    std::fprintf(f, "==== frame %d (state 0x%02X, list 0x%08X) ====\n", written, state, list_vaddr);
    Tracer t{};
    t.rdram = rdram;
    t.f = f;
    t.walk(list_vaddr & 0x00FFFFFFu, 0);
    std::fflush(f);
    if (written >= wanted) {
        std::fclose(f);
        f = nullptr;
        std::fprintf(stderr, "[wr64] 3D trace: %d frames written to %s\n", written, path);
        std::fflush(stderr);
    }
}

// ---- the cull survey ---------------------------------------------------
//
// Diagnosis only: WR64_CULL_SURVEY=1 measures what the game chooses to draw
// in a race, so the rules it uses to leave things out can be read off rather
// than guessed. A headset sees all round, while the game draws only what its
// flat camera would show; making it draw the rest starts with knowing where
// that boundary is.
//
// Every race frame is walked in full, calls followed, as the 3D trace walks
// it. Each block of vertices drawn has its centre taken into the camera's
// space -- the world is drawn under the model matrices and the camera rides
// on the projection stack, so modelview gives world space and the camera
// matrix the rest -- and is filed by its direction from the camera's heading
// (yaw, left negative) and its distance. Water (segment 3), sky (segment 6)
// and everything else are counted apart. Every twenty seconds of racing, and
// when a race ends, the totals so far are printed: in which directions
// anything was ever drawn, and how far away.
struct Surveyor {
    uint8_t* rdram = nullptr;
    uint32_t segments[16] = {};
    Mat4 projection_only = Mat4::identity();
    bool projection_perspective = false;
    bool have_camera = false;
    Mat4 camera = Mat4::identity();
    Mat4 modelview[16];
    int modelview_depth = 0;

    struct Category {
        static constexpr int kBins = 36;    // ten degrees each, from -180
        uint64_t count[kBins] = {};
        float farthest[kBins] = {};
        uint64_t total = 0;
        float farthest_overall = 0.0f;
        // A block filed again by its vertex nearest the camera's heading, in
        // ten-degree steps from 0 to 180 either side: a large block whose
        // centre is behind can still reach into view, and a game that keeps
        // whatever reaches into view keeps nothing past some angle here.
        static constexpr int kNearestBins = 18;
        uint64_t nearest[kNearestBins] = {};
    };
    Category* scene = nullptr;
    Category* water = nullptr;
    Category* sky = nullptr;

    // Per drawing list the frame calls, by its address: how often it was
    // drawn, and the widest angle off the camera's heading -- of its corner
    // nearest the heading -- at which it still was. An object the game leaves
    // out when it is outside the view tops out near the view's edge; one that
    // is always drawn reaches round behind. The address is also a constant in
    // the code that draws it, which leads to the test that decides.
    struct ListStats {
        uint64_t draws = 0;
        float widest = 0.0f;
        float farthest = 0.0f;
    };
    std::unordered_map<uint32_t, ListStats>* lists = nullptr;
    uint32_t current_list = 0;
    float current_nearest = 180.0f;
    float current_farthest = 0.0f;
    bool current_drawn = false;

    Surveyor() { modelview[0] = Mat4::identity(); }

    uint32_t physical(uint32_t segmented) const {
        return (segments[(segmented >> 24) & 0xF] + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }
    const uint32_t* words(uint32_t addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (addr & 0x00FFFFFFu));
    }
    int16_t signed_half(uint32_t addr) const {
        return static_cast<int16_t>(*reinterpret_cast<const uint16_t*>(rdram + ((addr & 0x00FFFFFFu) ^ 2)));
    }
    Mat4 read_matrix(uint32_t addr) const {
        Mat4 r{};
        for (int i = 0; i < 16; ++i) {
            const uint32_t full = (uint32_t(static_cast<uint16_t>(signed_half(addr + 2 * i))) << 16) |
                                  static_cast<uint16_t>(signed_half(addr + 32 + 2 * i));
            r.m[i / 4][i % 4] = static_cast<float>(static_cast<int32_t>(full)) / 65536.0f;
        }
        return r;
    }

    void record(uint32_t segmented, uint32_t count) {
        if (!have_camera || count == 0) return;
        const uint32_t addr = physical(segmented);
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        const uint32_t n = std::min<uint32_t>(count, 64);
        for (uint32_t i = 0; i < n; ++i) {
            sx += signed_half(addr + i * 16 + 0);
            sy += signed_half(addr + i * 16 + 2);
            sz += signed_half(addr + i * 16 + 4);
        }
        sx /= float(n); sy /= float(n); sz /= float(n);
        const Mat4& mv = modelview[modelview_depth];
        const Mat4& c = camera;
        // A point of the block's model space in the camera's space, and its
        // direction from the camera's heading.
        auto to_camera = [&](float x, float y, float z, float out[3]) {
            const float wx = x * mv.m[0][0] + y * mv.m[1][0] + z * mv.m[2][0] + mv.m[3][0];
            const float wy = x * mv.m[0][1] + y * mv.m[1][1] + z * mv.m[2][1] + mv.m[3][1];
            const float wz = x * mv.m[0][2] + y * mv.m[1][2] + z * mv.m[2][2] + mv.m[3][2];
            out[0] = wx * c.m[0][0] + wy * c.m[1][0] + wz * c.m[2][0] + c.m[3][0];
            out[1] = wx * c.m[0][1] + wy * c.m[1][1] + wz * c.m[2][1] + c.m[3][1];
            out[2] = wx * c.m[0][2] + wy * c.m[1][2] + wz * c.m[2][2] + c.m[3][2];
        };
        float nearest_yaw = 180.0f;
        for (uint32_t i = 0; i < n; ++i) {
            float p[3];
            to_camera(signed_half(addr + i * 16 + 0), signed_half(addr + i * 16 + 2), signed_half(addr + i * 16 + 4), p);
            nearest_yaw = std::min(nearest_yaw, std::fabs(std::atan2(p[0], -p[2]) * 57.2957795f));
        }
        float v[3];
        to_camera(sx, sy, sz, v);
        const float vx = v[0], vy = v[1], vz = v[2];
        const float distance = std::sqrt(vx * vx + vy * vy + vz * vz);
        const float yaw = std::atan2(vx, -vz) * 57.2957795f;
        int bin = int(std::floor((yaw + 180.0f) / 10.0f));
        bin = std::max(0, std::min(Category::kBins - 1, bin));

        const uint32_t segment = (segmented >> 24) & 0xF;
        if (segment != 3 && segment != 6) {
            current_drawn = true;
            current_nearest = std::min(current_nearest, nearest_yaw);
            current_farthest = std::max(current_farthest, distance);
        }
        Category* cat = segment == 3 ? water : segment == 6 ? sky : scene;
        cat->count[bin]++;
        cat->farthest[bin] = std::max(cat->farthest[bin], distance);
        cat->nearest[std::min(Category::kNearestBins - 1, int(nearest_yaw / 10.0f))]++;
        cat->total++;
        cat->farthest_overall = std::max(cat->farthest_overall, distance);
    }

    void walk(uint32_t cursor, int depth) {
        for (uint32_t steps = 0; steps < 8192; ++steps) {
            const uint32_t* cmd = words(cursor);
            const uint32_t w0 = cmd[0];
            const uint32_t w1 = cmd[1];
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            cursor += 8;
            switch (op) {
                case kOpEndDisplayList:
                    return;
                case kOpDisplayList: {
                    if (((w0 >> 16) & 0xFF) != 0) { cursor = physical(w1); continue; }
                    if (depth < 6) {
                        // A list the frame itself calls is one object, whatever
                        // it calls in turn.
                        if (depth == 0) {
                            current_list = w1;
                            current_nearest = 180.0f;
                            current_farthest = 0.0f;
                            current_drawn = false;
                        }
                        const int saved = modelview_depth;
                        walk(physical(w1), depth + 1);
                        modelview_depth = saved;
                        if (depth == 0 && current_drawn && lists != nullptr) {
                            ListStats& stats = (*lists)[current_list];
                            stats.draws++;
                            stats.widest = std::max(stats.widest, current_nearest);
                            stats.farthest = std::max(stats.farthest, current_farthest);
                        }
                    }
                    continue;
                }
                case kOpMoveWord:
                    if ((w0 & 0xFF) == kMoveWordSegment) segments[(w0 >> 10) & 0xF] = w1 & 0x00FFFFFFu;
                    continue;
                case kOpPopMtx:
                    if (modelview_depth > 0) --modelview_depth;
                    continue;
                case kOpMtx: {
                    const uint32_t params = (w0 >> 16) & 0xFF;
                    const Mat4 loaded = read_matrix(physical(w1));
                    if (params & kMtxProjection) {
                        if (params & kMtxLoad) {
                            projection_only = loaded;
                        }
                        else if (Walker::affine(loaded)) {
                            if (!have_camera && projection_perspective && !Walker::identity(loaded)) {
                                have_camera = true;
                                camera = loaded;
                            }
                        }
                        else {
                            projection_only = loaded * projection_only;
                        }
                        projection_perspective = Walker::perspective(projection_only);
                        continue;
                    }
                    if ((params & kMtxPush) && modelview_depth < 15) {
                        modelview[modelview_depth + 1] = modelview[modelview_depth];
                        ++modelview_depth;
                    }
                    modelview[modelview_depth] = (params & kMtxLoad) ? loaded : loaded * modelview[modelview_depth];
                    continue;
                }
                case kOpVtx:
                    if (projection_perspective) record(w1, (w0 >> 9) & 0x7F);
                    continue;
                default:
                    continue;
            }
        }
    }
};

void print_survey(const char* name, const Surveyor::Category& cat) {
    if (cat.total == 0) {
        std::fprintf(stderr, "[wr64-cull] %s: nothing drawn\n", name);
        return;
    }
    int first = -1, last = -1;
    for (int b = 0; b < Surveyor::Category::kBins; ++b) {
        if (cat.count[b] != 0) {
            if (first < 0) first = b;
            last = b;
        }
    }
    std::fprintf(stderr, "[wr64-cull] %s: %llu blocks, drawn between %d and %d degrees of the camera's heading,"
                         " as far as %.0f units\n",
                 name, static_cast<unsigned long long>(cat.total), first * 10 - 180, last * 10 - 170,
                 cat.farthest_overall);
    std::string line;
    for (int b = 0; b < Surveyor::Category::kBins; ++b) {
        if (cat.count[b] == 0) continue;
        char cell[48];
        std::snprintf(cell, sizeof(cell), " %+d:%llu/%.0f", b * 10 - 180,
                      static_cast<unsigned long long>(cat.count[b]), cat.farthest[b]);
        line += cell;
    }
    std::fprintf(stderr, "[wr64-cull]   by direction (degrees:blocks/farthest):%s\n", line.c_str());
    line.clear();
    for (int b = 0; b < Surveyor::Category::kNearestBins; ++b) {
        if (cat.nearest[b] == 0) continue;
        char cell[32];
        std::snprintf(cell, sizeof(cell), " %d:%llu", b * 10, static_cast<unsigned long long>(cat.nearest[b]));
        line += cell;
    }
    std::fprintf(stderr, "[wr64-cull]   by nearest corner's angle off heading (degrees:blocks):%s\n", line.c_str());
}

void survey_frame(uint8_t* rdram, uint32_t list_vaddr, uint32_t state) {
    static const bool enabled = std::getenv("WR64_CULL_SURVEY") != nullptr;
    if (!enabled) return;
    static Surveyor::Category scene, water, sky;
    static std::unordered_map<uint32_t, Surveyor::ListStats> lists;
    static uint64_t frames = 0;
    static bool was_racing = false;

    auto report = [&](const char* why) {
        std::fprintf(stderr, "[wr64-cull] ---- %s: %llu race frames surveyed ----\n", why,
                     static_cast<unsigned long long>(frames));
        print_survey("scenery", scene);
        print_survey("water", water);
        print_survey("sky", sky);
        // The lists drawn most, each with the widest angle off the heading it
        // was ever drawn at; those that never pass the view's edge are the
        // ones the game leaves out.
        std::vector<std::pair<uint32_t, Surveyor::ListStats>> sorted(lists.begin(), lists.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second.draws > b.second.draws; });
        std::fprintf(stderr, "[wr64-cull] %zu drawing lists; the 40 drawn most (address: draws, widest angle"
                             " drawn at, farthest):\n", sorted.size());
        for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
            std::fprintf(stderr, "[wr64-cull]   0x%08X: %llu, %.0f deg, %.0f\n", sorted[i].first,
                         static_cast<unsigned long long>(sorted[i].second.draws), sorted[i].second.widest,
                         sorted[i].second.farthest);
        }
        std::fflush(stderr);
    };

    const bool is_racing = racing(state);
    if (!is_racing) {
        if (was_racing && frames > 0) report("race ended");
        was_racing = false;
        return;
    }
    was_racing = true;

    Surveyor s{};
    s.rdram = rdram;
    s.scene = &scene;
    s.water = &water;
    s.sky = &sky;
    s.lists = &lists;
    s.walk(list_vaddr & 0x00FFFFFFu, 0);
    if (s.have_camera && ++frames % 600 == 0) report("so far");
}

// ---- the camera log ----------------------------------------------------
//
// Diagnosis only: WR64_CAMERA_LOG=1 prints, about once a second of racing,
// where the game's camera is and the shape of its lens. The VR work needs
// both -- the field of view says how much more a headset sees than the game
// draws, and the camera's distance from the craft says how big a game unit
// is.
//
// The camera is the affine matrix multiplied onto the perspective projection
// (Walker::on_matrix). Row vectors, so a world point p is seen at p * camera:
// the upper 3x3 is the rotation R and row 3 the translation t, and the eye
// sits where p * R + t = 0, which is -t * R^T while R is a pure rotation --
// `scale` is the length of R's first row, and anything far from 1 says it is
// not. The view looks down its own -Z, which in the world is minus R's third
// column.
//
// The lens is a guPerspective matrix, which libultra multiplies through by a
// scale the game chooses. guPerspective sets [2][3] to -scale, so dividing by
// -[2][3] takes it back out before the angles and distances are read.
void log_camera(uint32_t state, const Mat4& v, const Mat4& p) {
    constexpr float kDegrees = 57.2957795f;
    const float px = -(v.m[3][0] * v.m[0][0] + v.m[3][1] * v.m[0][1] + v.m[3][2] * v.m[0][2]);
    const float py = -(v.m[3][0] * v.m[1][0] + v.m[3][1] * v.m[1][1] + v.m[3][2] * v.m[1][2]);
    const float pz = -(v.m[3][0] * v.m[2][0] + v.m[3][1] * v.m[2][1] + v.m[3][2] * v.m[2][2]);
    const float scale = std::sqrt(v.m[0][0] * v.m[0][0] + v.m[0][1] * v.m[0][1] + v.m[0][2] * v.m[0][2]);

    const float s = -p.m[2][3];
    if (s == 0.0f || p.m[0][0] == 0.0f) return;
    const float fov_y = 2.0f * std::atan(s / p.m[1][1]) * kDegrees;
    const float aspect = p.m[1][1] / p.m[0][0];
    const float a = p.m[2][2] / s;
    const float b = p.m[3][2] / s;
    const float near_z = (a != 1.0f) ? b / (a - 1.0f) : 0.0f;
    const float far_z = (a != -1.0f) ? b / (a + 1.0f) : 0.0f;

    std::fprintf(stderr, "[wr64-cam] state 0x%02X pos=(%.1f, %.1f, %.1f) forward=(%.3f, %.3f, %.3f)"
                         " scale=%.3f fov_y=%.1f aspect=%.3f near=%.1f far=%.1f\n",
                 state, px, py, pz, -v.m[0][2], -v.m[1][2], -v.m[2][2],
                 scale, fov_y, aspect, near_z, far_z);
    std::fflush(stderr);
}

}  // namespace

namespace wr64::dlrewrite {

// With the first-person eye below the waves, the water's fog round it goes
// in once the scene is drawn -- before the frame's first flat projection or
// its first 2D rectangle, whichever comes first (Warm Up's HUD has no flat
// projection) -- in a state of its own that RT64 puts back after:
// untextured, unlit, unfogged, not culled, shaded by its vertices' colours
// and their opacity, tested against depth but not writing it.
void draw_water_fog(Walker& w, uint32_t state) {
    if (w.haze_list == 0 || w.haze_drawn || !racing(state)) return;
    w.haze_drawn = true;
    const uint32_t ex = uint32_t(RT64_EXTENDED_OPCODE) << 24;
    w.emit(ex | G_EX_PUSHGEOMETRYMODE_V1, 0);
    w.emit(ex | G_EX_PUSHOTHERMODE_V1, 0);
    w.emit(ex | G_EX_PUSHCOMBINE_V1, 0);
    w.emit(0xB6000000u, 0x000F3000u);                   // clear cull, fog, lighting, texgen
    w.emit(0xB7000000u, 0x00000205u);                   // set z-buffer, shade, smooth shading
    w.emit(0xBA001402u, 0x00000000u);                   // one cycle
    w.emit(0xB9000002u, 0x00000000u);                   // no alpha compare
    w.emit(0xB9000201u, 0x00000000u);                   // depth from the pixel
    w.emit(0xB900031Du, 0x00504A50u);                   // translucent surface, depth tested, not written
    w.emit(0xFCFFFFFFu, 0xFFFE793Cu);                   // shade colour and opacity, both cycles
    w.emit((uint32_t(kOpMtx) << 24) | ((kMtxLoad | kMtxPush) << 16) | 0x40, w.haze_identity);
    w.emit(uint32_t(kOpDisplayList) << 24, w.haze_list);
    w.emit(0xBD000000u, 0);                             // pop the modelview
    w.emit(ex | G_EX_POPCOMBINE_V1, 0);
    w.emit(ex | G_EX_POPOTHERMODE_V1, 0);
    w.emit(ex | G_EX_POPGEOMETRYMODE_V1, 0);
}

uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr) {
    static const bool disabled = std::getenv("WR64_NO_REWRITE") != nullptr;
    static const bool hud_off = std::getenv("WR64_HUD_OFF") != nullptr;
    static const bool tracing = std::getenv("WR64_HUD_TRACE") != nullptr;
    static Tags tags;
    static uint32_t traced_state = 0xFFFFFFFFu;
    static uint32_t traced_lists = 0;
    static std::string trace_text;

    if (disabled) {
        return 0;
    }
    if (!tags.loaded) {
        tags.load();
    }

    const uint32_t state = wr64::current_game_state();
    const auto& config = ultramodern::renderer::get_graphics_config();

    wr64::water::begin_frame(list_vaddr);
    trace_3d(rdram, list_vaddr, state);
    survey_frame(rdram, list_vaddr, state);
    // WR64_RAM_DUMP=<file>: the whole of RDRAM written out once, 300 race
    // frames in, for looking at what the game holds where.
    if (racing(state)) {
        static const char* ram_dump = std::getenv("WR64_RAM_DUMP");
        static int race_frames = 0;
        ++race_frames;
        // WR64_3D_TRACE_REWRITTEN=<file>: the last frame's list as rewritten
        // (still in the scratch space), traced as WR64_3D_TRACE traces the
        // game's, at the same moment.
        static const char* rewritten = std::getenv("WR64_3D_TRACE_REWRITTEN");
        if (rewritten != nullptr && race_frames == 300) {
            if (std::FILE* one = std::fopen(rewritten, "w")) {
                Tracer t{};
                t.rdram = rdram;
                t.f = one;
                t.walk(kScratch & 0x00FFFFFFu, 0);
                std::fclose(one);
            }
        }
        if (ram_dump != nullptr && race_frames == 300) {
            if (FILE* f = std::fopen(ram_dump, "wb")) {
                std::fwrite(rdram, 1, 0x800000, f);
                std::fclose(f);
                std::fprintf(stderr, "[wr64] RDRAM written to %s\n", ram_dump);
            }
        }
    }
    wr64::swim_frame(rdram);
    wr64::unlock_frame(rdram);

    Walker w{};
    w.rdram = rdram;
    w.state = state;
    w.menu = !racing(state);
    w.hud = !hud_off;
    static const bool noemit = std::getenv("WR64_HUD_NOEMIT") != nullptr;
    w.noemit = noemit;
    // The menus' layouts are 4:3 by design and stay so whatever the setting
    // says; the setting is about the race HUD. Anchoring the race HUD is
    // whether a frame is a race is read from the frame (see RaceTest), not
    // from this state number. WR64_HUD_NO_ANCHORS switches anchoring off.
    static const bool anchors_disabled = std::getenv("WR64_HUD_NO_ANCHORS") != nullptr;
    static const bool sky_interp_off = std::getenv("WR64_NO_SKY_INTERP") != nullptr;
    w.sky_interp = !sky_interp_off;
    static const bool water_interp_off = std::getenv("WR64_NO_WATER_INTERP") != nullptr;
    w.water_interp = !water_interp_off;
    w.anchors = !anchors_disabled &&
                config.hr_option != ultramodern::renderer::HUDRatioMode::Original;
    w.inset = wr64::display::anchor_inset();
    w.tags = &tags;
    w.out = reinterpret_cast<uint32_t*>(rdram + (kScratch & 0x00FFFFFFu));
    if (tracing) {
        if (state != traced_state) {
            traced_state = state;
            traced_lists = 0;
        }
        // Every list is traced into the buffer; the buffer is printed only for
        // the first few of a state and, after that, whenever the list draws a
        // different number of 2D elements than the one before it. Screens that
        // appear without changing the game's state -- the pause menu over a
        // race is the one that matters -- are otherwise never seen.
        trace_text.clear();
        w.trace = &trace_text;
    }

    // RT64 forgets the extended GBI at the end of every list, so every list
    // that uses it has to enable it first.
    if (GfxCommand* cmd = w.reserve(1)) {
        gEXEnable(cmd);
    }

    uint32_t cursor = list_vaddr & 0x00FFFFFFu;
    bool backdrop_called = false;
    for (uint32_t steps = 0; steps < kMaxCommands && !w.overflow; ++steps) {
        const uint32_t* c = w.words(cursor);
        const uint32_t w0 = c[0];
        const uint32_t w1 = c[1];
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        cursor += 8;
        if (op == kOpDisplayList && w1 == kBackdropSetupList && wr64::vr::enabled() && wr64::water::full_grid_screen(state)) {
            w.backdrop_next = true;
        }
        // A screen not yet known to be one of these that draws the same way
        // (the backdrop's list, then the full grid's) is learnt, for its
        // next frames.
        if (op == kOpDisplayList && wr64::vr::enabled() && !racing(state)) {
            if (w1 == kBackdropSetupList) backdrop_called = true;
            if (w1 == kFullGridList && backdrop_called && wr64::water::learn_full_grid_screen(state)) {
                std::fprintf(stderr, "[wr64-vr] state 0x%02X draws over the full grid of waves; treated as such from now on\n",
                             state);
                std::fflush(stderr);
            }
        }
        if (op == kOpTexRect || op == kOpFillRect) draw_water_fog(w, state);

        // VR: the sun's lens flare, glints and glare are left out (see 6. at
        // the top).
        if (op == kOpTexRect && w.projection_is_perspective && wr64::vr::enabled() &&
            (w.texture == kLensFlareTexture || w.texture == kSunGlintTexture || w.texture == kSunGlareTexture)) {
            continue;
        }
        // VR, the results and course select screens: the rectangle over the
        // whole picture painted behind the water is left out (see 6. at the
        // top).
        if (op == kOpFillRect && w.backdrop_next) {
            w.backdrop_next = false;
            if (((w1 >> 12) & 0xFFF) <= 8 * 4 && (w1 & 0xFFF) <= 20 * 4 &&
                ((w0 >> 12) & 0xFFF) >= 311 * 4 && (w0 & 0xFFF) >= 219 * 4) {
                static uint32_t reported = 0;
                if (reported != state) {
                    reported = state;
                    std::fprintf(stderr, "[wr64-vr] state 0x%02X: the backdrop behind the water is left out\n", state);
                    std::fflush(stderr);
                }
                continue;
            }
        }

        // Rectangles are held back with the commands between them until their
        // run ends (see push_rect); anything else ends the run.
        if (w.hud && is_rect(op)) {
            w.push_rect(w0, w1);
            continue;
        }
        if (w.pending_active && Walker::run_command(op)) {
            if (op == kOpSetTextureImage) w.texture = w1;
            w.pending.emplace_back(w0, w1);
            continue;
        }
        w.flush_run();

        if (op == kOpEndDisplayList) {
            // Nothing this walk turned on may outlive the list. RT64 forgets
            // its extended state at the end of one, but the scissor is plain
            // RDP state and has to be put back by hand.
            w.set_rect_class(Class::Auto);
            w.leave_class(false);
            w.sky_close();
            w.emit(w0, w1);
            break;
        }

        // The sky's gradient is loaded here and drawn by a call; both are
        // replaced when the sky is (see replace_sky_call).
        if (op == kOpVtx && w.sky_replaced && w.sky_lists.gradient != 0 && w1 == Walker::kSkyGradientVertices) {
            continue;
        }

        if (op == kOpDisplayList) {
            const bool branch = ((w0 >> 16) & 0xFF) != 0;
            if (branch) {
                cursor = w.physical(w1);
                continue;
            }
            if (w.projection_is_perspective && w.replace_sky_call(w1)) {
                continue;
            }
            // VR, in a race and its finish: a call that loads a perspective
            // projection of its own -- not the game's camera's -- is given the
            // group a top-level one would get, before it, so that projection
            // stays off the lens too; the next top-level load puts RT64's
            // own back. Reported once per game state and address.
            if (wr64::vr::enabled() && (state == 0x07 || (state >= 0x28 && state <= 0x2F)) && !w.own_group) {
                if (const uint32_t nested = w.nested_projection(w.physical(w1), kSceneProjection)) {
                    w.projection_group(G_EX_ASPECT_STRETCH);
                    w.own_group = true;
                    static std::vector<uint64_t> reported;
                    const uint64_t key = (uint64_t(state) << 32) | nested;
                    if (std::find(reported.begin(), reported.end(), key) == reported.end()) {
                        reported.push_back(key);
                        std::fprintf(stderr, "[wr64-vr] state 0x%02X calls 0x%08X, which loads a perspective projection"
                                             " from 0x%08X (on the panel)\n", state, w1, nested);
                        std::fflush(stderr);
                    }
                }
            }
            // A call under an orthographic projection is a 2D element drawn
            // from a static list: classify it by what the list draws.
            if (w.hud && w.have_projection && !w.projection_is_perspective) {
                Mat4 mv[16];
                std::memcpy(mv, w.modelview, sizeof(mv));
                int depth = w.modelview_depth;
                Extent rects;
                const Extent e = w.scan(w.physical(w1), 0, mv, depth, rects);
                if (!e.empty()) w.classify_draw(e, w1);
                if (!rects.empty()) w.classify_rect(rects, w1, {});
            }
            // The water surface is drawn from one of the game's static lists;
            // the group goes around the call, since the transform inside it is
            // created at its first vertex rather than at its matrix load.
            const bool water = w.draws_water(w1);
            const bool modern_water = water && w.known_water_material(w1) && w.underside_list == 0;
            if (water && w.water_interp) w.vertex_interp_group(Walker::kWaterId);
            if (modern_water) w.water_material(true);
            // VR: the wave mesh merged from builds all round the camera
            // (patches/water_all_round.cpp) is drawn in place of the game's,
            // in the same state, by a list of its own reached through segment
            // 3, which is moved to it for the call and then put back. The
            // full grid the results screen draws is ringed the same way.
            uint32_t merged_base = 0, merged_list = 0;
            // With the first-person eye below the waves, their underside
            // (src/first_person.cpp) in place of them, opaque and shaded by
            // its vertices, in a state of its own that RT64 puts back after.
            if (water && w.underside_list != 0) {
                const uint32_t ex = uint32_t(RT64_EXTENDED_OPCODE) << 24;
                w.emit(ex | G_EX_PUSHGEOMETRYMODE_V1, 0);
                w.emit(ex | G_EX_PUSHOTHERMODE_V1, 0);
                w.emit(ex | G_EX_PUSHCOMBINE_V1, 0);
                w.emit(0xB6000000u, 0x000F3000u);                   // clear cull, fog, lighting, texgen
                w.emit(0xB7000000u, 0x00000205u);                   // set z-buffer, shade, smooth shading
                w.emit(0xBA001402u, 0x00000000u);                   // one cycle
                w.emit(0xB9000002u, 0x00000000u);                   // no alpha compare
                w.emit(0xB9000201u, 0x00000000u);                   // depth from the pixel
                w.emit(0xB900031Du, 0x00552078u);                   // opaque surface, depth tested and written
                w.emit(0xFCFFFFFFu, 0xFFFE793Cu);                   // shade colour
                w.emit((uint32_t(kOpMtx) << 24) | ((kMtxLoad | kMtxPush) << 16) | 0x40, w.haze_identity);
                w.emit(uint32_t(kOpDisplayList) << 24, w.underside_list);
                w.emit(0xBD000000u, 0);                             // pop the modelview
                w.emit(ex | G_EX_POPCOMBINE_V1, 0);
                w.emit(ex | G_EX_POPOTHERMODE_V1, 0);
                w.emit(ex | G_EX_POPGEOMETRYMODE_V1, 0);
            }
            else if (water && w1 == 0x010082F0 && w.ring_full_grid()) {
                w.emit((uint32_t(kOpMoveWord) << 24) | ((3u * 4u) << 8) | kMoveWordSegment, w.full_grid_base);
                w.emit(uint32_t(kOpDisplayList) << 24, w.full_grid_list);
                w.emit((uint32_t(kOpMoveWord) << 24) | ((3u * 4u) << 8) | kMoveWordSegment, w.segments[3]);
            }
            else if (water && w1 == 0x0100B590 && wr64::water_merged(w.segments[3], &merged_base, &merged_list)) {
                static const char* dump_path = std::getenv("WR64_WATER_DUMP");
                static int dump_countdown = std::getenv("WR64_WATER_DUMP_AT") ? std::atoi(std::getenv("WR64_WATER_DUMP_AT")) : 300;
                if (dump_path != nullptr && dump_countdown >= 0 && dump_countdown-- == 0) {
                    float eye_x, eye_z, heading;
                    wr64::water_copy_camera(&eye_x, &eye_z, &heading);
                    // The merged mesh to the file named, the game's own beside it.
                    const std::string game_path = std::string(dump_path) + ".game";
                    FILE* merged = std::fopen(dump_path, "w");
                    FILE* game = std::fopen(game_path.c_str(), "w");
                    if (merged != nullptr && game != nullptr) {
                        std::fprintf(merged, "camera %f %f %f\n", eye_x, eye_z, heading);
                        std::fprintf(game, "camera %f %f %f\n", eye_x, eye_z, heading);
                        w.dump_water(w1, nullptr, 0, 0, game);
                        // The sky's three draws: two static lists with their
                        // own loads, and one whose seven vertices are loaded
                        // first from segment 6 at 0xE0.
                        std::fprintf(game, "sky\n");
                        w.dump_water(0x010103F0, nullptr, 0, 0, game);
                        const uint32_t band = (w.segments[6] + 0xE0) & 0x00FFFFF8u;
                        std::fprintf(game, "cmd 04000EEF 060000E0\n");
                        for (uint32_t i = 0; i < 7; ++i) {
                            const uint32_t v = band + i * 16;
                            std::fprintf(game, "v 0 %u %d %d %d %d %d %u %u %u %u\n", i,
                                         w.signed_half(v), w.signed_half(v + 2), w.signed_half(v + 4),
                                         w.signed_half(v + 8), w.signed_half(v + 10),
                                         rdram[(v + 12) ^ 3], rdram[(v + 13) ^ 3], rdram[(v + 14) ^ 3], rdram[(v + 15) ^ 3]);
                        }
                        w.dump_water(0x010103B8, nullptr, 0, 0, game);
                        w.dump_water(0x01010470, nullptr, 0, 0, game);
                        const uint32_t segment3 = w.segments[3];
                        w.segments[3] = merged_base;
                        w.dump_water(merged_list, nullptr, 0, 0, merged);
                        w.segments[3] = segment3;
                        if (w.sky_replaced) {
                            const uint32_t segment6 = w.segments[6];
                            w.segments[6] = w.sky_lists.base;
                            std::fprintf(merged, "sky\n");
                            w.dump_water(w.sky_lists.floor, nullptr, 0, 0, merged);
                            w.dump_water(w.sky_lists.gradient, nullptr, 0, 0, merged);
                            w.dump_water(w.sky_lists.clouds, nullptr, 0, 0, merged);
                            w.segments[6] = segment6;
                        }
                        std::fprintf(stderr, "[wr64-water] dumped the water meshes to %s\n", dump_path);
                    }
                    if (merged != nullptr) std::fclose(merged);
                    if (game != nullptr) std::fclose(game);
                }
                w.emit((uint32_t(kOpMoveWord) << 24) | ((3u * 4u) << 8) | kMoveWordSegment, merged_base);
                w.emit(uint32_t(kOpDisplayList) << 24, merged_list);
                w.emit((uint32_t(kOpMoveWord) << 24) | ((3u * 4u) << 8) | kMoveWordSegment, w.segments[3]);
            }
            else {
                w.emit(w0, w1);
                // First person: the game's bow splash round the thrown rider's
                // skidding body, after the game's own particles, which a race
                // draws from a list it builds each frame behind segment 5.
                if (w1 == 0x05000000u && racing(state)) {
                    if (const uint32_t splash = wr64::body_splash_list(rdram, w.physical(w1), w.segments[5], w.segments[3])) {
                        w.emit(uint32_t(kOpDisplayList) << 24, splash);
                    }
                    if (const uint32_t splash = wr64::body_splash_companion_list(rdram, w.segments[3])) {
                        w.emit(uint32_t(kOpDisplayList) << 24, splash);
                    }
                }
                // Fellow riders out on the water (src/companions.cpp), drawn
                // as the player's rider is, just after it.
                if (w1 == 0x02000000u && racing(state)) {
                    static std::vector<std::pair<uint32_t, uint32_t>> companions;
                    companions.clear();
                    const uint32_t course = *reinterpret_cast<const uint32_t*>(rdram + (kCourseId & 0x00FFFFFFu));
                    wr64::companions_draw(rdram, w.segments, course, companions);
                    for (const auto& [a, b] : companions) w.emit(a, b);
                }
            }
            if (modern_water) w.water_material(false);
            if (water && w.water_interp) w.vertex_interp_group(G_EX_ID_AUTO);
            continue;
        }

        if (op == kOpMoveWord && (w0 & 0xFF) == kMoveWordSegment) {
            w.segments[(w0 >> 10) & 0xF] = w1 & 0x00FFFFFFu;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpMoveMem && ((w0 >> 16) & 0xFF) == kMoveMemViewport) {
            w.have_viewport = true;
            w.viewport_w0 = w0;
            w.viewport_w1 = w1;
            // VR mode depends on the race's 3D viewport covering the whole
            // framebuffer, since the headset's lens is fitted to it; say once
            // what it is. A Vp is scale then translation, four halves each, in
            // quarter pixels.
            static bool reported_viewport = false;
            if (!reported_viewport && wr64::vr::enabled() && racing(state)) {
                reported_viewport = true;
                const uint32_t vp = w.physical(w1);
                std::fprintf(stderr, "[wr64-vr] the race's viewport: centre (%.1f, %.1f), half-size (%.1f, %.1f) pixels\n",
                             w.signed_half(vp + 8) / 4.0f, w.signed_half(vp + 10) / 4.0f,
                             w.signed_half(vp + 0) / 4.0f, w.signed_half(vp + 2) / 4.0f);
                std::fflush(stderr);
            }
            if (w.trace != nullptr) {
                char line[96];
                std::snprintf(line, sizeof(line), "[hud] state 0x%02X viewport load 0x%08X (class %s)\n",
                              state, w1, class_name(w.cls));
                w.trace->append(line);
            }
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpSetScissor) {
            w.on_scissor(w0, w1);
            w.have_scissor_cmd = true;
            w.scissor_w0 = w0;
            w.scissor_w1 = w1;
            // VR mode: the world's scissor becomes the whole framebuffer (see 5.
            // at the top). Coordinates are 10.2 fixed point in the low 24 bits
            // of each word, the mode bits above them are kept.
            static const bool open_scissor = wr64::vr::enabled() && std::getenv("WR64_VR_KEEP_SCISSOR") == nullptr;
            if (open_scissor) {
                const int ulx = static_cast<int>((w0 >> 12) & 0xFFF) >> 2;
                const int uly = static_cast<int>(w0 & 0xFFF) >> 2;
                const int lrx = static_cast<int>((w1 >> 12) & 0xFFF) >> 2;
                const int lry = static_cast<int>(w1 & 0xFFF) >> 2;
                if (ulx == wr64::display::kContentLeft && uly == wr64::display::kContentTop &&
                    lrx == wr64::display::kContentRight && lry == wr64::display::kContentBottom) {
                    w.emit(w0 & 0xFF000000u,
                           (w1 & 0xFF000000u) | (uint32_t(kFramebufferWidth * 4) << 12) | uint32_t(kFramebufferHeight * 4));
                    continue;
                }
            }
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpSetTextureImage) {
            w.texture = w1;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpPopMtx) {
            if (w.modelview_depth > 0) --w.modelview_depth;
            w.emit(w0, w1);
            continue;
        }

        // Dolphin Park's fish, each a top-level matrix load and its pieces:
        // drawn twice, in sizes, depths and colours of their own.
        if (op == kOpMtx && ((w0 >> 16) & 0xFF) == kMtxLoad && racing(state)) {
            static std::vector<std::pair<uint32_t, uint32_t>> fish;
            fish.clear();
            uint32_t after = 0;
            const uint32_t course = *reinterpret_cast<const uint32_t*>(rdram + (kCourseId & 0x00FFFFFFu));
            if (wr64::fish_rewrite(rdram, w.segments, course, cursor - 8, &after, fish)) {
                w.on_matrix(w0, w1);
                for (const auto& [a, b] : fish) w.emit(a, b);
                cursor = after;
                continue;
            }
        }

        if (op == kOpMtx) {
            const uint32_t params = (w0 >> 16) & 0xFF;
            const bool projection_load = (params & kMtxProjection) && (params & kMtxLoad);

            // A 2D class must not outlive the projection it was set for.
            if (projection_load && w.cls != Class::Auto) {
                w.leave_class(true);
            }
            if (projection_load && w.rect_cls != Class::Auto) {
                w.set_rect_class(Class::Auto);
            }

            // First person: the game's camera, multiplied onto its projection
            // while a race is run, is replaced by a view from the player's
            // rider. Once it is over -- finished or retired, state 0x29 on --
            // the game's own camera shows its ending, which looks right only
            // from outside.
            uint32_t matrix = w1;
            if ((params & kMtxProjection) && !(params & kMtxLoad) && w1 == kSceneView && racing(state)) {
                if (!w.first_person_tried) {
                    w.first_person_tried = true;
                    w.first_person_view = wr64::first_person_view(rdram, w.segments[3], &w.haze_list, &w.haze_identity, &w.underside_list);
                }
                if (w.first_person_view != 0) matrix = w.first_person_view;
            }

            // A projection that is not the scene's ends it: the water's fog
            // goes in first (see draw_water_fog).
            if (projection_load && !Walker::perspective(w.read_matrix(w.physical(w1)))) draw_water_fog(w, state);

            w.on_matrix(w0, matrix);

            // VR: 3D that is not the frame's scene is not seen through the
            // headset's lens but drawn on its panel with the frame's flat
            // elements (see 6. at the top). The group goes in before the load
            // it is for, and the next load puts RT64's own back.
            if (projection_load && wr64::vr::enabled()) {
                const bool persp = w.projection_is_perspective;
                // Each perspective projection a game state loads is reported
                // once, so a screen drawn wrongly in the headset can be matched
                // to what drew it.
                if (persp) {
                    static std::vector<uint64_t> reported;
                    const uint64_t key = (uint64_t(state) << 32) | w1;
                    if (std::find(reported.begin(), reported.end(), key) == reported.end()) {
                        reported.push_back(key);
                        std::fprintf(stderr, "[wr64-vr] state 0x%02X loads a perspective projection from 0x%08X (%s)\n",
                                     state, w1, w1 == kSceneProjection ? "the scene" : "on the panel");
                        std::fflush(stderr);
                    }
                }
                if (persp && w1 != kSceneProjection) {
                    const bool race_state = state == 0x07 || (state >= 0x28 && state <= 0x2F);
                    w.projection_group(race_state ? G_EX_ASPECT_STRETCH : G_EX_ASPECT_ADJUST);
                    w.own_group = true;
                }
                else if (w.own_group) {
                    w.projection_group(G_EX_ASPECT_AUTO);
                    w.own_group = false;
                }
            }

            if (projection_load && w.trace != nullptr) {
                char line[160];
                std::snprintf(line, sizeof(line), "[hud] state 0x%02X projection load %s [1][1]=%.3f [3][3]=%.3f"
                              " (class %s, viewport %s)\n",
                              state, w.projection_is_perspective ? "perspective" : "orthographic",
                              w.projection.m[1][1], w.projection.m[3][3], class_name(w.cls),
                              w.have_viewport ? "seen" : "not yet seen");
                w.trace->append(line);
            }

            if (projection_load && w.menu) {
                const bool persp = w.projection_is_perspective;
                if (persp && !w.centred && w.seen_ortho) {
                    w.align_viewport(G_EX_ORIGIN_CENTER, origin_cancel(G_EX_ORIGIN_CENTER));
                    w.centred = true;
                    ++w.centred_sections;
                }
                else if (!persp && w.centred) {
                    w.align_viewport(G_EX_ORIGIN_NONE, 0);
                    w.centred = false;
                }
            }
            if (projection_load && !w.projection_is_perspective) {
                w.seen_ortho = true;
            }
            w.sky_matrix(params);
            w.emit(w0, matrix);
            continue;
        }

        if (op == kOpVtx && w.hud && w.have_projection && !w.projection_is_perspective) {
            const uint32_t count = (w0 >> 9) & 0x7F;
            const Extent e = w.extent(w.physical(w1), count, w.modelview[w.modelview_depth]);
            w.classify_draw(e, 0);
            w.emit(w0, w1);
            continue;
        }

        w.emit(w0, w1);
    }

    if (w.overflow) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[wr64] a display list did not fit the rewriter's scratch space;"
                                 " it was passed through unchanged\n");
            std::fflush(stderr);
        }
        return 0;
    }

    // The drawn region follows the game's scissor, once it has held for a few
    // frames: a transition frame can draw into less than the whole region, and
    // the crop must not twitch with it.
    if (w.has_scissor()) {
        static int seen_left = -1, seen_top = -1, seen_right = -1, seen_bottom = -1;
        static int seen_for = 0;
        if (w.scissor_left == seen_left && w.scissor_top == seen_top &&
            w.scissor_right == seen_right && w.scissor_bottom == seen_bottom) {
            if (++seen_for == 3) {
                wr64::display::set_content(seen_left, seen_top, seen_right, seen_bottom);
            }
        }
        else {
            seen_left = w.scissor_left;
            seen_top = w.scissor_top;
            seen_right = w.scissor_right;
            seen_bottom = w.scissor_bottom;
            seen_for = 1;
        }
    }

    static uint32_t last_draws = 0xFFFFFFFFu;
    const bool composition_changed = w.draws_2d != last_draws;
    last_draws = w.draws_2d;
    if (w.trace != nullptr && w.written >= 30 && (traced_lists < 6 || composition_changed)) {
        ++traced_lists;
        std::fprintf(stderr, "[hud] ---- state 0x%02X list %u: %u commands, %u class changes, inset %.1f,"
                             " anchors %s, race %s (world-first %d, scissor %d,%d-%d,%d) ----\n%s",
                     state, traced_lists, w.written, w.class_changes, w.inset,
                     w.anchors ? "on" : "off", w.race_test.race() ? "yes" : "no",
                     w.race_test.world_first ? 1 : 0, w.scissor_left, w.scissor_top,
                     w.scissor_right, w.scissor_bottom,
                     trace_text.c_str());
        std::fflush(stderr);
    }

    // The sky section is reported the first time it is found and the first
    // time a race frame goes without it, so a course that draws its sky some
    // other way says so in the log rather than quietly staying at the game's
    // rate.
    static bool reported_sky = false, reported_no_sky = false;
    if (w.sky != Walker::SkySection::Before && !reported_sky) {
        reported_sky = true;
        std::fprintf(stderr, "[wr64] the sky's vertices are interpolated (state 0x%02X)\n", state);
        std::fflush(stderr);
    }
    else if (w.sky == Walker::SkySection::Before && w.sky_interp && w.race_test.race() &&
             !reported_no_sky) {
        reported_no_sky = true;
        std::fprintf(stderr, "[wr64] no sky section found in a race frame (state 0x%02X);"
                             " the sky stays at the game's rate\n", state);
        std::fflush(stderr);
    }

    // Drawing objects all round the camera (wr64::vr::all_round) uses more of
    // the game's per-frame matrices, of which there is room for 454: the Mtx
    // array at 0x4140 of the frame's buffer runs up to the LookAt at 0xB2C0.
    // D_801AE950 is how many this frame took by the time its list is handed
    // over. The peak is reported every twenty seconds or so, and as a warning
    // the first time it comes close.
    if (wr64::vr::all_round()) {
        static int32_t peak = 0;
        static uint32_t matrix_lists = 0;
        static bool warned = false;
        const int32_t used = *reinterpret_cast<const int32_t*>(rdram + 0x1AE950);
        peak = std::max(peak, used);
        if (!warned && used >= 400) {
            warned = true;
            std::fprintf(stderr, "[wr64-vr] WARNING: a frame used %d of the game's 454 matrices;"
                                 " lower WR64_VR_DRAW_DISTANCE if objects misbehave\n", used);
            std::fflush(stderr);
        }
        if (++matrix_lists % 600 == 0) {
            std::fprintf(stderr, "[wr64-vr] most matrices a frame has used: %d of 454\n", peak);
            std::fflush(stderr);
        }
    }

    // One race list in thirty is about once a second at the game's 30 Hz.
    static const bool camera_log = std::getenv("WR64_CAMERA_LOG") != nullptr;
    static uint32_t camera_lists = 0;
    if (camera_log && w.have_camera && racing(state) && camera_lists++ % 30 == 0) {
        log_camera(state, w.camera, w.camera_lens);
    }

    static uint32_t lists = 0;
    if (++lists == 1 || lists == 600) {
        std::fprintf(stderr, "[wr64] display list rewritten: %u commands, %u centred sections,"
                             " %u class changes (state 0x%02X)\n",
                     w.written, w.centred_sections, w.class_changes, state);
        std::fflush(stderr);
    }
    return kScratch;
}

}  // namespace wr64::dlrewrite

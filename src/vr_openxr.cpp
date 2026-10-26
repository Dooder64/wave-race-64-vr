// VR, phase 4: the OpenXR session, on RT64's own graphics card.
//
// WR64_VR=1 turns VR mode on. Once RT64 is set up, the renderer hands this
// file RT64's Direct3D 12 device and the command queue its present thread
// uses, and an OpenXR session is created on them -- the same objects RT64
// draws with, so the eye images can be drawn straight into the headset's
// swapchains. OpenXR insists the session lives on the graphics card the
// headset is attached to; RT64 chooses its card itself, so the two are
// compared, and VR is refused with a message rather than failing later if
// they differ.
//
// The headset is a second output of RT64's present thread
// (tools/patch_rt64_vr.py). Before each displayed frame the present thread
// calls begin_frame, which handles the session's events, waits until the
// headset wants a frame and hands over this frame's two eye images; RT64 draws
// its left and right eyes into them, and end_frame gives them back and submits
// the frame. So the headset sets the pace, and RT64's frame rate is set to
// the headset's while it does. When the session is not running -- the headset
// is off, or not yet ready -- begin_frame says so and RT64 paces itself to the
// window as before.
//
// Each eye is drawn through the headset's own lens for that eye, passed to
// RT64 every frame along with the size to render at, so the two images line
// up as the headset expects. The head is tracked on top of the game's camera:
// each frame both eyes' poses in the seated space become a transform RT64
// applies after the game's camera, metres turned into game units by the world
// scale (WR64_VR_SCALE), and published under a tag that comes back with the
// finished frame, so the headset is told the pose each frame was rendered
// with. The projection layer is placed in the seated space, and the
// compositor corrects for the head's movement since. F6 recentres.
//
// tools/xr_test is the same path stripped of the game, for when something
// here does not work and the question is whether it is the headset.

#include "wr64/vr.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>

#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// RT64's side of the headset output; these mirror the definitions in
// tools/patch_rt64_vr.py and must stay identical to them.
namespace RT64 {
struct RT64_XrEye {
    void* resource;
    uint32_t width;
    uint32_t height;
};

struct RT64_XrFrame {
    RT64_XrEye eyes[2];
    int writeRaw;
};

typedef int (*RT64_XrBeginFrame)(void* user, RT64_XrFrame* frame);
typedef void (*RT64_XrEndFrame)(void* user, int rendered, uint64_t headTag);
}  // namespace RT64

extern "C" void RT64_SetXrOutput(RT64::RT64_XrBeginFrame begin, RT64::RT64_XrEndFrame end, void* user);
extern "C" void RT64_SetXrRefreshRate(uint32_t rate);
extern "C" void RT64_SetXrRenderHeight(uint32_t height);
extern "C" void RT64_SetXrLens(int active, const float* angles);
extern "C" void RT64_SetXrHead(int active, const float* eyes, uint64_t tag);
extern "C" void RT64_SetStereo(int mode, float separation);

namespace wr64::vr {

namespace {

// Game units to a metre: the camera log puts the chase camera about 55 units
// above the water, which is around 1.7 m if a unit is 3 cm. The larger the
// scale, the smaller the world feels. WR64_VR_SCALE overrides it.
constexpr float kDefaultWorldScale = 33.3f;

float world_scale() {
    static const float scale = [] {
        const char* value = std::getenv("WR64_VR_SCALE");
        const float parsed = value != nullptr ? float(std::atof(value)) : 0.0f;
        return parsed > 0.0f ? parsed : kDefaultWorldScale;
    }();
    return scale;
}

// The poses recently published to RT64, by tag, so each frame can be handed
// to the headset with the pose it was rendered with. RT64 renders a few frames
// ahead of the one being shown; sixteen is far more than it ever holds.
struct TaggedViews {
    uint64_t tag = 0;
    XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
};
constexpr size_t kPoseHistory = 16;

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D12KHR> images;
    uint32_t acquired = 0;
};

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
XrSpace g_head = XR_NULL_HANDLE;
Eye g_eyes[2];
bool g_write_raw = false;

// Every OpenXR call after attach is made under this lock: by the present
// thread between begin_frame and end_frame (held across both, so a frame is
// never interrupted), and by shutdown on the game's thread.
std::mutex g_mutex;
bool g_shutting_down = false;

// Session and frame state, under g_mutex.
bool g_running = false;
XrFrameState g_frame_state{ XR_TYPE_FRAME_STATE };
XrView g_views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
bool g_images_acquired = false;
XrDuration g_reported_period = 0;
bool g_lens_reported = false;

// Head tracking, under g_mutex: the poses published so far, and the seated
// position and heading "straight ahead" is measured from. F6 asks for the
// head's current ones to become it.
TaggedViews g_pose_history[kPoseHistory];
uint64_t g_next_tag = 1;
std::atomic<bool> g_recenter_requested{ false };
XrVector3f g_centre_position{ 0.0f, 0.0f, 0.0f };
float g_centre_yaw = 0.0f;
uint64_t g_frames = 0;
uint64_t g_drawn = 0;
std::chrono::steady_clock::time_point g_last_report;

bool ok(XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "unknown";
    if (g_instance != XR_NULL_HANDLE) xrResultToString(g_instance, result, name);
    std::fprintf(stderr, "[wr64-vr] %s failed: %s (%d)\n", what, name, int(result));
    std::fflush(stderr);
    return false;
}

const char* state_name(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_IDLE:         return "idle";
        case XR_SESSION_STATE_READY:        return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE:      return "visible";
        case XR_SESSION_STATE_FOCUSED:      return "focused";
        case XR_SESSION_STATE_STOPPING:     return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
        case XR_SESSION_STATE_EXITING:      return "exiting";
        default:                            return "unknown";
    }
}

void destroy() {
    for (Eye& eye : g_eyes) {
        if (eye.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye.swapchain);
        eye = Eye{};
    }
    if (g_head != XR_NULL_HANDLE) xrDestroySpace(g_head);
    if (g_local != XR_NULL_HANDLE) xrDestroySpace(g_local);
    if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
    if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
    g_head = g_local = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
}

// The session's events, under g_mutex. Returns false once the session has
// ended for good.
bool poll_events() {
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    bool alive = true;
    while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            std::fprintf(stderr, "[wr64-vr] session %s\n", state_name(changed.state));
            std::fflush(stderr);
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                g_running = ok(xrBeginSession(g_session, &begin), "xrBeginSession");
            }
            else if (changed.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_running = false;
                // RT64 renders for and paces itself to the window again until
                // the session is back.
                RT64_SetXrRefreshRate(0);
                RT64_SetXrRenderHeight(0);
                RT64_SetXrLens(0, nullptr);
                RT64_SetXrHead(0, nullptr, 0);
                g_reported_period = 0;
                g_lens_reported = false;
            }
            else if (changed.state == XR_SESSION_STATE_EXITING ||
                     changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                alive = false;
            }
        }
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            alive = false;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
    return alive;
}

// ---- head tracking ----------------------------------------------------------
//
// OpenXR's spaces and the game's camera space agree: right-handed, +Y up,
// looking down -Z. So an eye's pose, measured from the seated centre, can be
// put straight after the game's camera -- RT64 does it after turning the view
// to the middle of the game's frame -- once its metres are game units.

// The heading of an orientation: its turn about the vertical, left positive.
float yaw_of(const XrQuaternionf& q) {
    const float forward_x = -2.0f * (q.x * q.z + q.w * q.y);
    const float forward_z = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(-forward_x, -forward_z);
}

XrQuaternionf multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// The transform RT64 multiplies on after the game's camera for one eye: from
// the seated space into the eye's own, in game units, as row vectors. The
// eye's pose is first taken relative to the centre -- its position less the
// centre's, both turned back by the centre's heading -- so straight ahead is
// wherever the head faced when it was last recentred. A point p of the seated
// space then lands at (p - position) * R, R being the orientation's rotation
// written for column vectors, which as row vectors is its inverse.
void eye_transform(const XrPosef& pose, float out[16]) {
    const float scale = world_scale();
    const float c = std::cos(-g_centre_yaw), s = std::sin(-g_centre_yaw);
    const float dx = pose.position.x - g_centre_position.x;
    const float dy = pose.position.y - g_centre_position.y;
    const float dz = pose.position.z - g_centre_position.z;
    const float px = (c * dx + s * dz) * scale;
    const float py = dy * scale;
    const float pz = (-s * dx + c * dz) * scale;

    const XrQuaternionf unturn{ 0.0f, std::sin(-g_centre_yaw * 0.5f), 0.0f, std::cos(-g_centre_yaw * 0.5f) };
    const XrQuaternionf q = multiply(unturn, pose.orientation);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const float r[3][3] = {
        { 1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy) },
        { 2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx) },
        { 2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy) },
    };

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out[row * 4 + column] = r[row][column];
        }
        out[row * 4 + 3] = 0.0f;
    }
    for (int column = 0; column < 3; ++column) {
        out[12 + column] = -(px * r[0][column] + py * r[1][column] + pz * r[2][column]);
    }
    out[15] = 1.0f;
}

// RT64's present thread, before each displayed frame. The lock taken here is
// held until end_frame whenever a frame is begun (0 or 1 returned).
int begin_frame(void*, RT64::RT64_XrFrame* frame) {
    g_mutex.lock();
    if (g_shutting_down || !poll_events() || !g_running) {
        g_mutex.unlock();
        return -1;
    }

    g_frame_state = { XR_TYPE_FRAME_STATE };
    if (!ok(xrWaitFrame(g_session, nullptr, &g_frame_state), "xrWaitFrame") ||
        !ok(xrBeginFrame(g_session, nullptr), "xrBeginFrame")) {
        g_mutex.unlock();
        return -1;
    }

    if (g_frame_state.predictedDisplayPeriod != g_reported_period && g_frame_state.predictedDisplayPeriod > 0) {
        g_reported_period = g_frame_state.predictedDisplayPeriod;
        const double hz = 1e9 / double(g_frame_state.predictedDisplayPeriod);
        RT64_SetXrRefreshRate(uint32_t(std::lround(hz)));
        RT64_SetXrRenderHeight(g_eyes[0].height);
        std::fprintf(stderr, "[wr64-vr] the headset refreshes at %.1f Hz; RT64 now renders for it\n", hz);
        std::fflush(stderr);
    }

    // The eyes in the seated space, where the head is now -- predicted for
    // when this frame is shown, which is as close to the frames RT64 is about
    // to render as anything known.
    XrViewLocateInfo locate{ XR_TYPE_VIEW_LOCATE_INFO };
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = g_frame_state.predictedDisplayTime;
    locate.space = g_local;
    XrViewState view_state{ XR_TYPE_VIEW_STATE };
    uint32_t located = 0;
    g_views[0] = { XR_TYPE_VIEW };
    g_views[1] = { XR_TYPE_VIEW };
    xrLocateViews(g_session, &locate, &view_state, 2, &located, g_views);
    const bool tracked = located == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                         (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

    g_images_acquired = false;
    if (!g_frame_state.shouldRender || !tracked) {
        return 0;
    }

    // F6: straight ahead becomes wherever the head is facing, from wherever it
    // is -- a heading only, so up stays up.
    if (g_recenter_requested.exchange(false)) {
        XrSpaceLocation where{ XR_TYPE_SPACE_LOCATION };
        xrLocateSpace(g_head, g_local, g_frame_state.predictedDisplayTime, &where);
        g_centre_position = where.pose.position;
        g_centre_yaw = yaw_of(where.pose.orientation);
        std::fprintf(stderr, "[wr64-vr] recentred: facing %.1f degrees, at (%+.3f, %+.3f, %+.3f) m\n",
                     g_centre_yaw * 57.2957795f, g_centre_position.x, g_centre_position.y, g_centre_position.z);
        std::fflush(stderr);
    }

    // Publish the head to RT64 under a new tag, and remember the poses by it.
    float heads[2][16];
    eye_transform(g_views[0].pose, heads[0]);
    eye_transform(g_views[1].pose, heads[1]);
    const uint64_t tag = g_next_tag++;
    TaggedViews& kept = g_pose_history[tag % kPoseHistory];
    kept.tag = tag;
    kept.views[0] = g_views[0];
    kept.views[1] = g_views[1];
    RT64_SetXrHead(1, &heads[0][0], tag);

    // Each eye's lens, for RT64 to draw that eye through. The runtime reports
    // it every frame; it only changes if the headset's settings do.
    const float angles[8] = {
        g_views[0].fov.angleLeft, g_views[0].fov.angleRight, g_views[0].fov.angleUp, g_views[0].fov.angleDown,
        g_views[1].fov.angleLeft, g_views[1].fov.angleRight, g_views[1].fov.angleUp, g_views[1].fov.angleDown,
    };
    RT64_SetXrLens(1, angles);
    if (!g_lens_reported) {
        g_lens_reported = true;
        constexpr float kDegrees = 57.2957795f;
        std::fprintf(stderr, "[wr64-vr] eye lenses (left, right, up, down, degrees): left eye %.1f %.1f %.1f %.1f,"
                             " right eye %.1f %.1f %.1f %.1f\n",
                     angles[0] * kDegrees, angles[1] * kDegrees, angles[2] * kDegrees, angles[3] * kDegrees,
                     angles[4] * kDegrees, angles[5] * kDegrees, angles[6] * kDegrees, angles[7] * kDegrees);
        std::fflush(stderr);
    }

    for (uint32_t e = 0; e < 2; ++e) {
        Eye& eye = g_eyes[e];
        xrAcquireSwapchainImage(eye.swapchain, nullptr, &eye.acquired);
        XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(eye.swapchain, &wait);
        frame->eyes[e].resource = eye.images[eye.acquired].texture;
        frame->eyes[e].width = eye.width;
        frame->eyes[e].height = eye.height;
    }
    frame->writeRaw = g_write_raw ? 1 : 0;
    g_images_acquired = true;
    return 1;
}

// RT64's present thread, after the frame begun above, drawn or not. Its
// command list has finished, so the images can go straight back. The frame is
// handed to the headset with the poses it was rendered with, found by the tag
// RT64 kept with it, so the compositor corrects for exactly the head's
// movement since; a tag no longer remembered falls back to this frame's.
void end_frame(void*, int rendered, uint64_t head_tag) {
    if (g_images_acquired) {
        for (Eye& eye : g_eyes) {
            xrReleaseSwapchainImage(eye.swapchain, nullptr);
        }
    }

    const XrView* rendered_views = g_views;
    const TaggedViews& kept = g_pose_history[head_tag % kPoseHistory];
    if (head_tag != 0 && kept.tag == head_tag) {
        rendered_views = kept.views;
    }

    XrCompositionLayerProjectionView views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                  { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    for (uint32_t e = 0; e < 2; ++e) {
        views[e].pose = rendered_views[e].pose;
        views[e].fov = rendered_views[e].fov;
        views[e].subImage.swapchain = g_eyes[e].swapchain;
        views[e].subImage.imageRect.extent = { int32_t(g_eyes[e].width), int32_t(g_eyes[e].height) };
    }
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = g_local;
    layer.viewCount = 2;
    layer.views = views;
    const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };

    const bool submit = g_images_acquired && rendered != 0;
    XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
    end.displayTime = g_frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = submit ? 1 : 0;
    end.layers = layers;
    ok(xrEndFrame(g_session, &end), "xrEndFrame");
    g_images_acquired = false;

    ++g_frames;
    if (submit) ++g_drawn;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_report >= std::chrono::seconds(2)) {
        const double interval = std::chrono::duration<double>(now - g_last_report).count();
        if (g_last_report.time_since_epoch().count() != 0) {
            std::fprintf(stderr, "[wr64-vr] %.0f headset frames a second, %.0f of them drawn\n",
                         double(g_frames) / interval, double(g_drawn) / interval);
            std::fflush(stderr);
        }
        g_last_report = now;
        g_frames = 0;
        g_drawn = 0;
    }

    g_mutex.unlock();
}

// One swapchain per eye at the size the headset recommends. sRGB is asked
// for, so the compositor reads the images as the gamma-encoded pictures they
// are. What the runtime makes of that decides how RT64 writes to them: a
// typeless image takes the VI's output as it is through a plain view, while
// an image made in the sRGB format itself encodes as it is written, and is
// given the output linearised.
bool create_swapchains() {
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data());
    if (view_count != 2) {
        std::fprintf(stderr, "[wr64-vr] expected two views, the runtime reports %u\n", view_count);
        return false;
    }

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats.data());
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT wanted : { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM }) {
        for (int64_t f : formats) {
            if (f == wanted && format == DXGI_FORMAT_UNKNOWN) format = wanted;
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        std::fprintf(stderr, "[wr64-vr] the runtime offers no RGBA8 swapchain format\n");
        return false;
    }

    for (uint32_t e = 0; e < 2; ++e) {
        Eye& eye = g_eyes[e];
        eye.width = views[e].recommendedImageRectWidth;
        eye.height = views[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = eye.width;
        info.height = eye.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!ok(xrCreateSwapchain(g_session, &info, &eye.swapchain), "xrCreateSwapchain")) return false;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(eye.swapchain, 0, &count, nullptr);
        eye.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        xrEnumerateSwapchainImages(eye.swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    }

    const DXGI_FORMAT made = g_eyes[0].images[0].texture->GetDesc().Format;
    g_write_raw = (made == DXGI_FORMAT_R8G8B8A8_TYPELESS) || (format == DXGI_FORMAT_R8G8B8A8_UNORM);
    std::fprintf(stderr, "[wr64-vr] eye images %u x %u, %zu per eye, %s, %s\n", g_eyes[0].width,
                 g_eyes[0].height, g_eyes[0].images.size(),
                 format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? "sRGB" : "linear",
                 g_write_raw ? "written as they are" : "encoded as they are written");
    std::fflush(stderr);
    return true;
}

}  // namespace

bool enabled() {
    static const bool on = [] {
        const char* value = std::getenv("WR64_VR");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return on;
}

void attach(void* d3d12_device, void* d3d12_queue) {
    if (!enabled() || g_instance != XR_NULL_HANDLE) return;
    auto* device = static_cast<ID3D12Device*>(d3d12_device);
    auto* queue = static_cast<ID3D12CommandQueue*>(d3d12_queue);

    const char* extensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instance_info{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::strcpy(instance_info.applicationInfo.applicationName, "Wave Race 64: Recompiled");
    instance_info.applicationInfo.applicationVersion = 1;
    std::strcpy(instance_info.applicationInfo.engineName, "WaveRace64Recomp");
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = extensions;
    if (!ok(xrCreateInstance(&instance_info, &g_instance), "xrCreateInstance")) {
        std::fprintf(stderr, "[wr64-vr] no OpenXR runtime answered; is Quest Link or Air Link running?"
                             " VR is off for this run.\n");
        std::fflush(stderr);
        g_instance = XR_NULL_HANDLE;
        return;
    }

    XrInstanceProperties instance_props{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(g_instance, &instance_props);
    XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(g_instance, &system_info, &g_system), "xrGetSystem")) {
        std::fprintf(stderr, "[wr64-vr] the runtime (%s) sees no headset. VR is off for this run.\n",
                     instance_props.runtimeName);
        std::fflush(stderr);
        destroy();
        return;
    }
    XrSystemProperties system_props{ XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(g_instance, g_system, &system_props);
    std::fprintf(stderr, "[wr64-vr] %s through %s %u.%u.%u\n", system_props.systemName, instance_props.runtimeName,
                 XR_VERSION_MAJOR(instance_props.runtimeVersion), XR_VERSION_MINOR(instance_props.runtimeVersion),
                 XR_VERSION_PATCH(instance_props.runtimeVersion));

    // OpenXR requires this call before a session, and its answer is the card
    // the headset is attached to. RT64 has already chosen its card.
    PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(g_instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D12KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    if (get_requirements == nullptr ||
        !ok(get_requirements(g_instance, g_system, &requirements), "xrGetD3D12GraphicsRequirementsKHR")) {
        destroy();
        return;
    }
    const LUID rt64_luid = device->GetAdapterLuid();
    if (std::memcmp(&rt64_luid, &requirements.adapterLuid, sizeof(LUID)) != 0) {
        std::fprintf(stderr, "[wr64-vr] RT64 is drawing on a different graphics card from the one the headset"
                             " is attached to. VR is off for this run.\n");
        std::fflush(stderr);
        destroy();
        return;
    }
    std::fprintf(stderr, "[wr64-vr] RT64's graphics card is the headset's\n");

    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo session_info{ XR_TYPE_SESSION_CREATE_INFO };
    session_info.next = &binding;
    session_info.systemId = g_system;
    if (!ok(xrCreateSession(g_instance, &session_info, &g_session), "xrCreateSession")) {
        destroy();
        return;
    }

    // LOCAL is the seated space: its origin is where the head was when the
    // session began, or when it was last recentred from the headset's menu.
    XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    if (!ok(xrCreateReferenceSpace(g_session, &space_info, &g_local), "xrCreateReferenceSpace (local)")) {
        destroy();
        return;
    }
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!ok(xrCreateReferenceSpace(g_session, &space_info, &g_head), "xrCreateReferenceSpace (view)")) {
        destroy();
        return;
    }

    if (!create_swapchains()) {
        destroy();
        return;
    }

    // The headset needs two eyes, so stereo is on whatever F8 last chose; the
    // eyes' separation is the headset's own, in each eye's pose, so RT64 adds
    // none of its own.
    RT64_SetStereo(1, 0.0f);
    g_shutting_down = false;
    RT64_SetXrOutput(begin_frame, end_frame, nullptr);
    std::fprintf(stderr, "[wr64-vr] RT64 will draw the headset's eye images; %.1f game units to a metre\n",
                 world_scale());
    std::fflush(stderr);
}

void recenter() {
    if (!enabled()) return;
    g_recenter_requested = true;
}

void shutdown() {
    if (g_instance == XR_NULL_HANDLE) return;

    // RT64 stops calling in; a frame already begun finishes first, since its
    // lock is held until it ends.
    RT64_SetXrOutput(nullptr, nullptr, nullptr);
    RT64_SetXrRefreshRate(0);
    RT64_SetXrRenderHeight(0);
    RT64_SetXrLens(0, nullptr);
    RT64_SetXrHead(0, nullptr, 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shutting_down = true;

    // Ask the runtime to end the session, and wait for it to say so -- but not
    // for ever, so a runtime that never answers cannot keep the game from
    // closing. Empty frames keep the session moving towards its end, since
    // RT64 no longer supplies any.
    if (g_running) {
        xrRequestExitSession(g_session);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (g_running && std::chrono::steady_clock::now() < deadline) {
            if (!poll_events()) break;
            if (!g_running) break;
            XrFrameState state{ XR_TYPE_FRAME_STATE };
            if (XR_SUCCEEDED(xrWaitFrame(g_session, nullptr, &state)) &&
                XR_SUCCEEDED(xrBeginFrame(g_session, nullptr))) {
                XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
                end.displayTime = state.predictedDisplayTime;
                end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                xrEndFrame(g_session, &end);
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (g_running) {
            std::fprintf(stderr, "[wr64-vr] the runtime did not end the session; leaving it\n");
        }
    }

    destroy();
    std::fprintf(stderr, "[wr64-vr] OpenXR shut down\n");
    std::fflush(stderr);
}

}  // namespace wr64::vr

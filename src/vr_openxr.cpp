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
// What the game draws flat -- a race's HUD, the title's lettering, a menu's
// labels -- cannot be stretched over each eye's view with the rest, since
// the eyes' lenses look in different directions and the two copies would
// never line up. In a frame with a 3D scene RT64 moves it onto a panel,
// WR64_VR_HUD_DISTANCE metres ahead and WR64_VR_HUD_WIDTH wide, placed in
// each eye as that eye sees it. The panel stays in front of the eyes as the
// head turns; WR64_VR_HUD_FIXED=1 hangs it straight ahead of the seated
// position instead, where the game's camera looks. A frame with nothing drawn
// through the lens -- a menu with no 3D behind it -- RT64 draws onto a third
// image instead, which is hung in front of the seated position as a flat
// screen (a quad layer), WR64_VR_SCREEN_DISTANCE metres away and
// WR64_VR_SCREEN_WIDTH wide; the headset draws it for each eye itself.
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

#include <algorithm>
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
    RT64_XrEye screen;
    int writeRaw;
};

typedef int (*RT64_XrBeginFrame)(void* user, RT64_XrFrame* frame);
// rendered: 0 nothing, 1 the eyes, 2 the flat screen.
typedef void (*RT64_XrEndFrame)(void* user, int rendered, uint64_t headTag);
}  // namespace RT64

extern "C" void RT64_SetXrOutput(RT64::RT64_XrBeginFrame begin, RT64::RT64_XrEndFrame end, void* user);
extern "C" void RT64_SetXrRefreshRate(uint32_t rate);
extern "C" void RT64_SetXrRenderHeight(uint32_t height);
extern "C" void RT64_SetXrLens(int active, const float* angles);
extern "C" void RT64_SetXrHead(int active, const float* eyes, uint64_t tag);
extern "C" void RT64_SetStereo(int mode, float separation);
extern "C" void RT64_SetXrPanel(float distance, float width, float separation, int followHead);
extern "C" void RT64_SetXrFramed(float scale, float x, float y);
extern "C" void RT64_SetXrPanelPose(const float* centre, const float* right, const float* up);
extern "C" void RT64_SetXrPanelLayer(int on);

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

// The flat screen's image: the game's 4:3, about as many pixels across as
// the screen covers of an eye's.
constexpr uint32_t kScreenWidth = 1920;
constexpr uint32_t kScreenHeight = 1440;

float setting(const char* name, float fallback) {
    const char* value = std::getenv(name);
    const float parsed = value != nullptr ? float(std::atof(value)) : 0.0f;
    return parsed > 0.0f ? parsed : fallback;
}

float screen_distance() {
    static const float distance = setting("WR64_VR_SCREEN_DISTANCE", 2.5f);
    return distance;
}

// The settings' panel and menu size: how much bigger than their own widths
// below the flat screen and the panel are drawn (set_panel_size).
std::atomic<float> g_panel_size{ 1.0f };

float screen_width() {
    static const float width = setting("WR64_VR_SCREEN_WIDTH", 2.0f);
    return width * g_panel_size.load(std::memory_order_relaxed);
}

// The panel a frame's flat elements are drawn on in front of the eyes.
float hud_distance() {
    static const float distance = setting("WR64_VR_HUD_DISTANCE", 2.0f);
    return distance;
}

float hud_width() {
    static const float width = setting("WR64_VR_HUD_WIDTH", 1.6f);
    return width * g_panel_size.load(std::memory_order_relaxed);
}

// Framed 3D -- a model a menu turns in a box of its own, such as a jet ski
// on the selection screens -- stays in its box on the panel; WR64_VR_FRAMED_SCALE,
// WR64_VR_FRAMED_X and WR64_VR_FRAMED_Y scale and move it within the box, in
// the panel's own -1 to 1 (so 1 is the panel's whole half-width).
float signed_setting(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? float(std::atof(value)) : fallback;
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
XrSpace g_head = XR_NULL_HANDLE;
Eye g_eyes[2];
Eye g_screen;
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
// For tests through the mirror with the headset lying still,
// WR64_VR_TEST_RECENTRE=1 asks for it at the first frame.
std::atomic<bool> g_recenter_requested{ std::getenv("WR64_VR_TEST_RECENTRE") != nullptr };
XrVector3f g_centre_position{ 0.0f, 0.0f, 0.0f };
// WR64_VR_TEST_YAW turns straight ahead that many degrees to the left, as if
// the head were turned right by it -- for checking, with the headset still,
// what stays put in the seated space and what follows the head.
float g_centre_yaw = float(std::atof(std::getenv("WR64_VR_TEST_YAW") ? std::getenv("WR64_VR_TEST_YAW") : "0")) * 0.0174532925f;
// WR64_VR_TEST_PITCH tilts the eyes that many degrees up, likewise, for
// looking with the headset still at what lies above or below straight ahead.
const float g_test_pitch = float(std::atof(std::getenv("WR64_VR_TEST_PITCH") ? std::getenv("WR64_VR_TEST_PITCH") : "0")) * 0.0174532925f;
// ---- the Touch controllers ---------------------------------------------------
//
// Read through OpenXR actions, bound to Meta's Touch controller profile: each
// hand's thumbstick, trigger and grip, the right hand's A and B, the left's
// menu button and thumbstick click, and each hand's vibration. The present
// thread syncs them each headset frame, under g_mutex, and leaves what it read
// in atomics for the game's input (controllers()) -- only while the session
// is focused, so the headset's own menus never steer the jet ski. Vibration
// asked for by the port's haptics (set_controller_rumble) is sent from the
// same place, a pulse a little longer than a frame, renewed each frame.
XrActionSet g_action_set = XR_NULL_HANDLE;
XrAction g_stick_action[2] = {}, g_trigger_action[2] = {}, g_grip_action[2] = {};
XrAction g_a_action = XR_NULL_HANDLE, g_b_action = XR_NULL_HANDLE, g_menu_action = XR_NULL_HANDLE;
XrAction g_stick_click_action = XR_NULL_HANDLE, g_haptic_action = XR_NULL_HANDLE;
XrPath g_hand_paths[2] = { XR_NULL_PATH, XR_NULL_PATH };
std::atomic<bool> g_pads_live{ false };
std::atomic<float> g_pad_stick[2][2], g_pad_trigger[2], g_pad_grip[2];
std::atomic<bool> g_pad_a{ false }, g_pad_b{ false }, g_pad_menu{ false };
std::atomic<float> g_rumble[2];
bool g_rumbling[2] = { false, false };
bool g_stick_click_was = false;

uint64_t g_frames = 0;
uint64_t g_drawn = 0;
uint64_t g_screen_frames = 0;
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

// The Touch controllers' actions, bound and attached to the session. False,
// with a message, if anything is refused; the game then simply has no
// controllers from the headset.
bool create_controllers() {
    XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
    std::strcpy(set_info.actionSetName, "wr64");
    std::strcpy(set_info.localizedActionSetName, "Wave Race 64");
    if (!ok(xrCreateActionSet(g_instance, &set_info, &g_action_set), "xrCreateActionSet")) return false;

    xrStringToPath(g_instance, "/user/hand/left", &g_hand_paths[0]);
    xrStringToPath(g_instance, "/user/hand/right", &g_hand_paths[1]);
    auto action = [&](XrAction* out, XrActionType type, const char* name, const char* shown, bool both_hands) {
        XrActionCreateInfo info{ XR_TYPE_ACTION_CREATE_INFO };
        info.actionType = type;
        std::strcpy(info.actionName, name);
        std::strcpy(info.localizedActionName, shown);
        if (both_hands) {
            info.countSubactionPaths = 2;
            info.subactionPaths = g_hand_paths;
        }
        return ok(xrCreateAction(g_action_set, &info, out), name);
    };
    if (!action(&g_stick_action[0], XR_ACTION_TYPE_VECTOR2F_INPUT, "left_stick", "Left thumbstick", false) ||
        !action(&g_stick_action[1], XR_ACTION_TYPE_VECTOR2F_INPUT, "right_stick", "Right thumbstick", false) ||
        !action(&g_trigger_action[0], XR_ACTION_TYPE_FLOAT_INPUT, "left_trigger", "Left trigger", false) ||
        !action(&g_trigger_action[1], XR_ACTION_TYPE_FLOAT_INPUT, "right_trigger", "Right trigger", false) ||
        !action(&g_grip_action[0], XR_ACTION_TYPE_FLOAT_INPUT, "left_grip", "Left grip", false) ||
        !action(&g_grip_action[1], XR_ACTION_TYPE_FLOAT_INPUT, "right_grip", "Right grip", false) ||
        !action(&g_a_action, XR_ACTION_TYPE_BOOLEAN_INPUT, "a", "A", false) ||
        !action(&g_b_action, XR_ACTION_TYPE_BOOLEAN_INPUT, "b", "B", false) ||
        !action(&g_menu_action, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", false) ||
        !action(&g_stick_click_action, XR_ACTION_TYPE_BOOLEAN_INPUT, "left_stick_click", "Left thumbstick click", false) ||
        !action(&g_haptic_action, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble", true)) {
        return false;
    }

    const struct { XrAction action; const char* path; } bindings[] = {
        { g_stick_action[0], "/user/hand/left/input/thumbstick" },
        { g_stick_action[1], "/user/hand/right/input/thumbstick" },
        { g_trigger_action[0], "/user/hand/left/input/trigger/value" },
        { g_trigger_action[1], "/user/hand/right/input/trigger/value" },
        { g_grip_action[0], "/user/hand/left/input/squeeze/value" },
        { g_grip_action[1], "/user/hand/right/input/squeeze/value" },
        { g_a_action, "/user/hand/right/input/a/click" },
        { g_b_action, "/user/hand/right/input/b/click" },
        { g_menu_action, "/user/hand/left/input/menu/click" },
        { g_stick_click_action, "/user/hand/left/input/thumbstick/click" },
        { g_haptic_action, "/user/hand/left/output/haptic" },
        { g_haptic_action, "/user/hand/right/output/haptic" },
    };
    std::vector<XrActionSuggestedBinding> suggested;
    for (const auto& binding : bindings) {
        XrPath path = XR_NULL_PATH;
        xrStringToPath(g_instance, binding.path, &path);
        suggested.push_back({ binding.action, path });
    }
    XrInteractionProfileSuggestedBinding profile{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    xrStringToPath(g_instance, "/interaction_profiles/oculus/touch_controller", &profile.interactionProfile);
    profile.countSuggestedBindings = uint32_t(suggested.size());
    profile.suggestedBindings = suggested.data();
    if (!ok(xrSuggestInteractionProfileBindings(g_instance, &profile), "xrSuggestInteractionProfileBindings")) {
        return false;
    }

    XrSessionActionSetsAttachInfo attach_info{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach_info.countActionSets = 1;
    attach_info.actionSets = &g_action_set;
    return ok(xrAttachSessionActionSets(g_session, &attach_info), "xrAttachSessionActionSets");
}

// Each headset frame, under g_mutex: the controllers read, and the rumble
// asked for sent.
void update_controllers() {
    if (g_action_set == XR_NULL_HANDLE) return;
    XrActiveActionSet active{ g_action_set, XR_NULL_PATH };
    XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    // Anything but XR_SUCCESS -- XR_SESSION_NOT_FOCUSED, with the headset's
    // own menu up, say -- leaves the game with no input from them.
    const XrResult synced = xrSyncActions(g_session, &sync);
    const bool live = synced == XR_SUCCESS;
    g_pads_live.store(live, std::memory_order_relaxed);
    // Each change reported, so a log says when the controllers' input stopped
    // reaching the game, and why.
    static XrResult reported_sync = XR_RESULT_MAX_ENUM;
    if (synced != reported_sync) {
        reported_sync = synced;
        char name[XR_MAX_RESULT_STRING_SIZE] = "unknown";
        xrResultToString(g_instance, synced, name);
        std::fprintf(stderr, "[wr64-vr] the Touch controllers' input: %s\n", live ? "reaching the game" : name);
        std::fflush(stderr);
    }
    if (!live) {
        g_stick_click_was = false;
        return;
    }
    {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = g_trigger_action[1];
        XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
        const bool bound = XR_SUCCEEDED(xrGetActionStateFloat(g_session, &info, &state)) && state.isActive;
        static int reported_bound = -1;
        if (int(bound) != reported_bound) {
            reported_bound = int(bound);
            std::fprintf(stderr, "[wr64-vr] the right trigger is %s\n",
                         bound ? "bound to the throttle" : "not bound to anything (controller off, or another profile)");
            std::fflush(stderr);
        }
    }

    auto get_float = [](XrAction action) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
        return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &info, &state)) && state.isActive ? state.currentState : 0.0f;
    };
    auto get_bool = [](XrAction action) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
        return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &info, &state)) && state.isActive && state.currentState;
    };
    for (int hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = g_stick_action[hand];
        XrActionStateVector2f stick{ XR_TYPE_ACTION_STATE_VECTOR2F };
        const bool got = XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &info, &stick)) && stick.isActive;
        g_pad_stick[hand][0].store(got ? stick.currentState.x : 0.0f, std::memory_order_relaxed);
        g_pad_stick[hand][1].store(got ? stick.currentState.y : 0.0f, std::memory_order_relaxed);
        g_pad_trigger[hand].store(get_float(g_trigger_action[hand]), std::memory_order_relaxed);
        g_pad_grip[hand].store(get_float(g_grip_action[hand]), std::memory_order_relaxed);
    }
    g_pad_a.store(get_bool(g_a_action), std::memory_order_relaxed);
    g_pad_b.store(get_bool(g_b_action), std::memory_order_relaxed);
    g_pad_menu.store(get_bool(g_menu_action), std::memory_order_relaxed);

    // The left thumbstick's click switches first person, as F5 does.
    const bool click = get_bool(g_stick_click_action);
    if (click && !g_stick_click_was) toggle_first_person();
    g_stick_click_was = click;

    for (int hand = 0; hand < 2; ++hand) {
        const float strength = std::clamp(g_rumble[hand].load(std::memory_order_relaxed), 0.0f, 1.0f);
        XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
        info.action = g_haptic_action;
        info.subactionPath = g_hand_paths[hand];
        if (strength > 0.01f) {
            XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
            vibration.amplitude = strength;
            vibration.duration = 40'000'000;       // 40 ms, renewed each frame
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(g_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
            g_rumbling[hand] = true;
        }
        else if (g_rumbling[hand]) {
            xrStopHapticFeedback(g_session, &info);
            g_rumbling[hand] = false;
        }
    }
}

void destroy() {
    if (g_action_set != XR_NULL_HANDLE) xrDestroyActionSet(g_action_set);     // and its actions
    g_action_set = XR_NULL_HANDLE;
    g_pads_live.store(false, std::memory_order_relaxed);
    for (Eye* eye : { &g_eyes[0], &g_eyes[1], &g_screen }) {
        if (eye->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye->swapchain);
        *eye = Eye{};
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
        else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED && g_action_set != XR_NULL_HANDLE) {
            // Which controllers the runtime says are in each hand.
            for (int hand = 0; hand < 2; ++hand) {
                XrInteractionProfileState profile{ XR_TYPE_INTERACTION_PROFILE_STATE };
                char path[XR_MAX_PATH_LENGTH] = "none";
                uint32_t length = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g_session, g_hand_paths[hand], &profile)) &&
                    profile.interactionProfile != XR_NULL_PATH) {
                    xrPathToString(g_instance, profile.interactionProfile, sizeof(path), &length, path);
                }
                std::fprintf(stderr, "[wr64-vr] %s hand: %s\n", hand == 0 ? "left" : "right", path);
            }
            std::fflush(stderr);
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
    const XrQuaternionf tilt{ std::sin(g_test_pitch * 0.5f), 0.0f, 0.0f, std::cos(g_test_pitch * 0.5f) };
    const XrQuaternionf q = multiply(tilt, multiply(unturn, pose.orientation));
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

// ---- the panel ---------------------------------------------------------------
//
// The panel the flat elements of a frame with a 3D scene are drawn on. Drawn
// into the eyes' pictures, something held still in front of the eyes judders
// as the head turns: the runtime shifts each picture at the last moment to
// where the head has got to since it was drawn, which is right for the world
// but drags what should stay with the eyes, and by an amount that changes
// with each picture's age -- and RT64 places it only once a game frame. So by
// default (WR64_VR_HUD_MODE=layer) the panel is a layer of its own: RT64 draws
// the flat elements apart from the eyes, over black and over white, combines
// the two into a see-through image on the screen's swapchain
// (RT64_SetXrPanelLayer), and the runtime holds it in front of the eyes itself
// every refresh. =soft hangs it in the seated space and follows the head
// there, straight ahead of it again within about WR64_VR_HUD_LAG seconds
// (0.03); =locked holds it in front of the eyes in their pictures; =fixed
// hangs it straight ahead of the seated position for good (or
// WR64_VR_HUD_FIXED=1).
enum class HudMode { Layer, Soft, Locked, Fixed };

HudMode hud_mode() {
    static const HudMode mode = [] {
        if (std::getenv("WR64_VR_HUD_FIXED") != nullptr) return HudMode::Fixed;
        const char* value = std::getenv("WR64_VR_HUD_MODE");
        if (value != nullptr && std::strcmp(value, "soft") == 0) return HudMode::Soft;
        if (value != nullptr && std::strcmp(value, "locked") == 0) return HudMode::Locked;
        if (value != nullptr && std::strcmp(value, "fixed") == 0) return HudMode::Fixed;
        return HudMode::Layer;
    }();
    return mode;
}

// Where the soft panel's middle is aimed from, and which way, in the seated
// space (game units, the heading left positive, up positive).
bool g_panel_placed = false;
float g_panel_at[3] = { 0.0f, 0.0f, 0.0f };
float g_panel_yaw = 0.0f, g_panel_pitch = 0.0f;

// Under g_mutex, each headset frame: the soft panel moved a step towards
// straight ahead of the head as this frame's views have it, and handed to
// RT64 as its middle and its right and up directions.
void place_panel(float distance, float frame_time) {
    static const float lag = setting("WR64_VR_HUD_LAG", 0.03f);
    const float scale = world_scale();
    const float c = std::cos(-g_centre_yaw), s = std::sin(-g_centre_yaw);

    // The head: between the eyes, and the way the first eye faces.
    float at[3];
    {
        const float dx = 0.5f * (g_views[0].pose.position.x + g_views[1].pose.position.x) - g_centre_position.x;
        const float dy = 0.5f * (g_views[0].pose.position.y + g_views[1].pose.position.y) - g_centre_position.y;
        const float dz = 0.5f * (g_views[0].pose.position.z + g_views[1].pose.position.z) - g_centre_position.z;
        at[0] = (c * dx + s * dz) * scale;
        at[1] = dy * scale;
        at[2] = (-s * dx + c * dz) * scale;
    }
    const XrQuaternionf unturn{ 0.0f, std::sin(-g_centre_yaw * 0.5f), 0.0f, std::cos(-g_centre_yaw * 0.5f) };
    const XrQuaternionf q = multiply(unturn, g_views[0].pose.orientation);
    // -z turned by q: the way the head faces.
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fy = -2.0f * (q.y * q.z - q.w * q.x);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    const float pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));

    if (!g_panel_placed) {
        g_panel_placed = true;
        g_panel_yaw = yaw;
        g_panel_pitch = pitch;
        std::memcpy(g_panel_at, at, sizeof(at));
    }
    const float step = lag > 0.0f ? 1.0f - std::exp(-frame_time / lag) : 1.0f;
    float turn = yaw - g_panel_yaw;
    turn -= 6.2831853f * std::round(turn / 6.2831853f);
    g_panel_yaw += turn * step;
    g_panel_pitch += (pitch - g_panel_pitch) * step;
    for (int i = 0; i < 3; ++i) g_panel_at[i] += (at[i] - g_panel_at[i]) * step;

    const float cy = std::cos(g_panel_yaw), sy = std::sin(g_panel_yaw);
    const float cp = std::cos(g_panel_pitch), sp = std::sin(g_panel_pitch);
    const float ahead[3] = { -sy * cp, sp, -cy * cp };
    const float right[3] = { cy, 0.0f, -sy };
    const float up[3] = { right[1] * ahead[2] - right[2] * ahead[1], right[2] * ahead[0] - right[0] * ahead[2],
                          right[0] * ahead[1] - right[1] * ahead[0] };
    const float centre[3] = { g_panel_at[0] + ahead[0] * distance, g_panel_at[1] + ahead[1] * distance,
                              g_panel_at[2] + ahead[2] * distance };
    RT64_SetXrPanelPose(centre, right, up);
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
    update_controllers();

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
    const float sx = g_views[1].pose.position.x - g_views[0].pose.position.x;
    const float sy = g_views[1].pose.position.y - g_views[0].pose.position.y;
    const float sz = g_views[1].pose.position.z - g_views[0].pose.position.z;
    const float scale = world_scale();
    RT64_SetXrPanel(hud_distance() * scale, hud_width() * scale, std::sqrt(sx * sx + sy * sy + sz * sz) * scale,
                    hud_mode() == HudMode::Locked ? 1 : 0);
    RT64_SetXrPanelLayer(hud_mode() == HudMode::Layer ? 1 : 0);
    if (hud_mode() == HudMode::Soft) {
        place_panel(hud_distance() * scale,
                    float(std::max<XrDuration>(g_frame_state.predictedDisplayPeriod, 1)) / 1e9f);
    }
    static const float framed_scale = setting("WR64_VR_FRAMED_SCALE", 1.0f);
    static const float framed_x = signed_setting("WR64_VR_FRAMED_X", 0.0f);
    static const float framed_y = signed_setting("WR64_VR_FRAMED_Y", 0.0f);
    RT64_SetXrFramed(framed_scale, framed_x, framed_y);
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
    xrAcquireSwapchainImage(g_screen.swapchain, nullptr, &g_screen.acquired);
    XrSwapchainImageWaitInfo screen_wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    screen_wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_screen.swapchain, &screen_wait);
    frame->screen.resource = g_screen.images[g_screen.acquired].texture;
    frame->screen.width = g_screen.width;
    frame->screen.height = g_screen.height;
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
        for (Eye* eye : { &g_eyes[0], &g_eyes[1], &g_screen }) {
            xrReleaseSwapchainImage(eye->swapchain, nullptr);
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

    // The flat screen: straight ahead of the seated position, at its height,
    // facing it, so it stays where it hangs as the head moves.
    XrCompositionLayerQuad screen{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    screen.space = g_local;
    screen.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    screen.subImage.swapchain = g_screen.swapchain;
    screen.subImage.imageRect.extent = { int32_t(g_screen.width), int32_t(g_screen.height) };
    const float distance = screen_distance();
    screen.pose.orientation = { 0.0f, std::sin(0.5f * g_centre_yaw), 0.0f, std::cos(0.5f * g_centre_yaw) };
    screen.pose.position = { g_centre_position.x - distance * std::sin(g_centre_yaw), g_centre_position.y,
                             g_centre_position.z - distance * std::cos(g_centre_yaw) };
    screen.size = { screen_width(), screen_width() * float(g_screen.height) / float(g_screen.width) };

    // The HUD on a layer of its own (3): the see-through image RT64 left on the
    // screen's swapchain, straight ahead of the eyes in the head's own space,
    // so the runtime holds it there itself at every refresh. Its colour is
    // already multiplied by how solid it is.
    XrCompositionLayerQuad panel{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    panel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    panel.space = g_head;
    panel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    panel.subImage.swapchain = g_screen.swapchain;
    panel.subImage.imageRect.extent = { int32_t(g_screen.width), int32_t(g_screen.height) };
    panel.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
    panel.pose.position = { 0.0f, 0.0f, -hud_distance() };
    panel.size = { hud_width(), hud_width() * float(g_screen.height) / float(g_screen.width) };

    const XrCompositionLayerBaseHeader* layers[2] = {
        rendered == 2 ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&screen)
                      : reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer),
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&panel) };
    static int last_rendered = 0;
    if (rendered != 0 && rendered != last_rendered) {
        last_rendered = rendered;
        if (rendered == 2) {
            std::fprintf(stderr, "[wr64-vr] frames go to the flat screen, %.1f m away and %.1f m wide\n",
                         distance, screen.size.width);
        }
        else if (rendered == 3) {
            std::fprintf(stderr, "[wr64-vr] frames go to the eyes, flat elements on a layer of their own %.1f m away"
                                 " and %.1f m wide\n", hud_distance(), hud_width());
        }
        else {
            std::fprintf(stderr, "[wr64-vr] frames go to the eyes, flat elements on a panel %.1f m away and %.1f m wide\n",
                         hud_distance(), hud_width());
        }
        std::fflush(stderr);
    }

    const bool submit = g_images_acquired && rendered != 0;
    XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
    end.displayTime = g_frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = submit ? (rendered == 3 ? 2 : 1) : 0;
    end.layers = layers;
    ok(xrEndFrame(g_session, &end), "xrEndFrame");
    g_images_acquired = false;

    ++g_frames;
    if (submit) ++g_drawn;
    if (submit && rendered == 2) ++g_screen_frames;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_report >= std::chrono::seconds(2)) {
        const double interval = std::chrono::duration<double>(now - g_last_report).count();
        if (g_last_report.time_since_epoch().count() != 0) {
            std::fprintf(stderr, "[wr64-vr] %.0f headset frames a second, %.0f of them drawn, %.0f on the flat screen\n",
                         double(g_frames) / interval, double(g_drawn) / interval, double(g_screen_frames) / interval);
            std::fflush(stderr);
        }
        g_last_report = now;
        g_frames = 0;
        g_drawn = 0;
        g_screen_frames = 0;
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

    {
        g_screen.width = kScreenWidth;
        g_screen.height = kScreenHeight;
        XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        // Also written by RT64's combination of the HUD's two passes, a compute
        // shader, when the panel is a layer of its own.
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = g_screen.width;
        info.height = g_screen.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!ok(xrCreateSwapchain(g_session, &info, &g_screen.swapchain), "xrCreateSwapchain (screen)")) return false;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(g_screen.swapchain, 0, &count, nullptr);
        g_screen.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        xrEnumerateSwapchainImages(g_screen.swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(g_screen.images.data()));
    }

    const DXGI_FORMAT made = g_eyes[0].images[0].texture->GetDesc().Format;
    g_write_raw = (made == DXGI_FORMAT_R8G8B8A8_TYPELESS) || (format == DXGI_FORMAT_R8G8B8A8_UNORM);
    std::fprintf(stderr, "[wr64-vr] eye images %u x %u, %zu per eye, %s, %s\n", g_eyes[0].width,
                 g_eyes[0].height, g_eyes[0].images.size(),
                 format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? "sRGB" : "linear",
                 g_write_raw ? "written as they are" : "encoded as they are written");
    const D3D12_RESOURCE_DESC screen_desc = g_screen.images[0].texture->GetDesc();
    std::fprintf(stderr, "[wr64-vr] screen images %u x %u, format %d, %s\n", g_screen.width, g_screen.height,
                 int(screen_desc.Format),
                 (screen_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? "writable by compute shaders"
                                                                                  : "NOT writable by compute shaders");
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
    if (create_controllers()) {
        std::fprintf(stderr, "[wr64-vr] the Touch controllers are bound\n");
    }
    else if (g_action_set != XR_NULL_HANDLE) {
        xrDestroyActionSet(g_action_set);
        g_action_set = XR_NULL_HANDLE;
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

Controllers controllers() {
    Controllers c{};
    c.live = g_pads_live.load(std::memory_order_relaxed);
    if (!c.live) return c;
    c.left_x = g_pad_stick[0][0].load(std::memory_order_relaxed);
    c.left_y = g_pad_stick[0][1].load(std::memory_order_relaxed);
    c.right_x = g_pad_stick[1][0].load(std::memory_order_relaxed);
    c.right_y = g_pad_stick[1][1].load(std::memory_order_relaxed);
    c.left_trigger = g_pad_trigger[0].load(std::memory_order_relaxed);
    c.right_trigger = g_pad_trigger[1].load(std::memory_order_relaxed);
    c.left_grip = g_pad_grip[0].load(std::memory_order_relaxed);
    c.right_grip = g_pad_grip[1].load(std::memory_order_relaxed);
    c.a = g_pad_a.load(std::memory_order_relaxed);
    c.b = g_pad_b.load(std::memory_order_relaxed);
    c.menu = g_pad_menu.load(std::memory_order_relaxed);
    return c;
}

void set_panel_size(float size) {
    g_panel_size.store(std::clamp(size, 0.25f, 4.0f), std::memory_order_relaxed);
}

void set_controller_rumble(float left, float right) {
    g_rumble[0].store(left, std::memory_order_relaxed);
    g_rumble[1].store(right, std::memory_order_relaxed);
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

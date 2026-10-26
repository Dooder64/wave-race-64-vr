// VR, phase 4: the OpenXR session, on RT64's own graphics card.
//
// WR64_VR=1 turns VR mode on. Once RT64 is set up, the renderer hands this
// file RT64's Direct3D 12 device and the command queue its present thread
// uses, and an OpenXR session is created on them -- the same objects RT64
// draws with, so the eye images can later be copied straight into the
// headset's swapchains. OpenXR insists the session lives on the graphics
// card the headset is attached to; RT64 chooses its card itself, so the two
// are compared, and VR is refused with a message rather than failing later
// if they differ.
//
// For now the session runs a frame loop of its own on a thread of its own,
// submitting no images: it proves the lifecycle -- ready, focused, paused
// when the headset comes off, resumed, stopped -- inside the game. The game
// itself draws to the monitor exactly as before. Moving the frame loop into
// RT64's present thread, where the eye images are, comes next.
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
#include <thread>
#include <vector>

namespace wr64::vr {

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local = XR_NULL_HANDLE;
XrSpace g_head = XR_NULL_HANDLE;
std::thread g_thread;
std::atomic<bool> g_stop{ false };

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
    if (g_head != XR_NULL_HANDLE) xrDestroySpace(g_head);
    if (g_local != XR_NULL_HANDLE) xrDestroySpace(g_local);
    if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
    if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
    g_head = g_local = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_instance = XR_NULL_HANDLE;
}

// The session's own loop: events, and while the session is running an empty
// frame each display period, which is what keeps the runtime from deciding
// the application has stopped responding. Once every two seconds it reports
// the frame rate the headset is pacing it at and where the head is.
void session_loop() {
    bool running = false;
    bool exit_requested = false;
    std::chrono::steady_clock::time_point exit_requested_at;
    uint64_t frames = 0;
    XrDuration reported_period = 0;
    auto last_report = std::chrono::steady_clock::now();

    for (;;) {
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        bool exiting = false;
        while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                std::fprintf(stderr, "[wr64-vr] session %s\n", state_name(changed.state));
                std::fflush(stderr);
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = ok(xrBeginSession(g_session, &begin), "xrBeginSession");
                }
                else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(g_session);
                    running = false;
                }
                else if (changed.state == XR_SESSION_STATE_EXITING ||
                         changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    exiting = true;
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                exiting = true;
            }
            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        if (exiting) break;

        // Shutting down: ask the runtime to end the session, and wait for it
        // to say so -- but not for ever, so a runtime that never answers
        // cannot keep the game from closing. A session that was never running
        // can simply be left.
        if (g_stop.load()) {
            if (!running) break;
            if (!exit_requested) {
                exit_requested = true;
                exit_requested_at = std::chrono::steady_clock::now();
                xrRequestExitSession(g_session);
            }
            else if (std::chrono::steady_clock::now() - exit_requested_at > std::chrono::seconds(3)) {
                std::fprintf(stderr, "[wr64-vr] the runtime did not end the session; leaving it\n");
                break;
            }
        }

        if (!running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
        if (!ok(xrWaitFrame(g_session, nullptr, &frame_state), "xrWaitFrame")) break;
        if (!ok(xrBeginFrame(g_session, nullptr), "xrBeginFrame")) break;
        XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if (!ok(xrEndFrame(g_session, &end), "xrEndFrame")) break;
        ++frames;

        if (frame_state.predictedDisplayPeriod != reported_period && frame_state.predictedDisplayPeriod > 0) {
            reported_period = frame_state.predictedDisplayPeriod;
            std::fprintf(stderr, "[wr64-vr] the headset refreshes at %.1f Hz\n",
                         1e9 / double(frame_state.predictedDisplayPeriod));
            std::fflush(stderr);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(2)) {
            const double interval = std::chrono::duration<double>(now - last_report).count();
            last_report = now;
            XrSpaceLocation where{ XR_TYPE_SPACE_LOCATION };
            xrLocateSpace(g_head, g_local, frame_state.predictedDisplayTime, &where);
            std::fprintf(stderr, "[wr64-vr] %.0f frames a second, head at (%+.3f, %+.3f, %+.3f) m\n",
                         double(frames) / interval, where.pose.position.x, where.pose.position.y,
                         where.pose.position.z);
            std::fflush(stderr);
            frames = 0;
        }
    }

    std::fprintf(stderr, "[wr64-vr] session loop finished\n");
    std::fflush(stderr);
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

    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data());
    if (view_count == 2) {
        std::fprintf(stderr, "[wr64-vr] the headset asks for %u x %u per eye\n",
                     views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
    }
    std::fflush(stderr);

    g_stop = false;
    g_thread = std::thread(session_loop);
}

void shutdown() {
    if (g_instance == XR_NULL_HANDLE) return;
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    destroy();
    std::fprintf(stderr, "[wr64-vr] OpenXR shut down\n");
    std::fflush(stderr);
}

}  // namespace wr64::vr

// Phase 4a: the smallest OpenXR program that proves the headset path.
//
// Before OpenXR goes anywhere near RT64, this checks every link in the chain
// on its own: the OpenXR loader, the active runtime (Meta's for Quest Link, or
// SteamVR's), the graphics card the runtime asks for, a Direct3D 12 session
// on it, and the frame loop. The left eye is cleared red and the right eye
// blue, both pulsing slowly so a frozen frame is obvious, and the head's
// position and direction are printed once a second.
//
// It is built on its own (tools/build_xr_test.ps1), not into the game, so a
// failure here is a headset or runtime problem and nothing else.
//
// Usage: wr64_xr_test [seconds]     (default 60; the headset can also end it)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

bool check(XrInstance instance, XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "unknown";
    if (instance != XR_NULL_HANDLE) xrResultToString(instance, result, name);
    std::fprintf(stderr, "[xr-test] %s failed: %s (%d)\n", what, name, int(result));
    return false;
}

bool check(HRESULT result, const char* what) {
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "[xr-test] %s failed: 0x%08lX\n", what, static_cast<unsigned long>(result));
    return false;
}

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    std::vector<XrSwapchainImageD3D12KHR> images;
    UINT rtv_base = 0;      // first of this eye's descriptors in the RTV heap
};

// Yaw and pitch of an orientation, in degrees, for a readable log line. The
// view looks down its own -Z, as in OpenGL.
void describe(const XrPosef& pose, float* yaw, float* pitch) {
    const XrQuaternionf& q = pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fy = -2.0f * (q.y * q.z - q.w * q.x);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    constexpr float kDegrees = 57.2957795f;
    *yaw = std::atan2(-fx, -fz) * kDegrees;
    *pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fy))) * kDegrees;
}

}  // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;

    // ---- instance and system ------------------------------------------------
    const char* extensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instance_info{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::strcpy(instance_info.applicationInfo.applicationName, "WR64 XR Test");
    instance_info.applicationInfo.applicationVersion = 1;
    std::strcpy(instance_info.applicationInfo.engineName, "WaveRace64Recomp");
    instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = extensions;
    XrInstance instance = XR_NULL_HANDLE;
    if (!check(XR_NULL_HANDLE, xrCreateInstance(&instance_info, &instance), "xrCreateInstance")) {
        std::fprintf(stderr, "[xr-test] Is Quest Link or Air Link connected, and is an OpenXR runtime active?\n");
        return 1;
    }

    XrInstanceProperties instance_props{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(instance, &instance_props);
    std::printf("[xr-test] runtime: %s %u.%u.%u\n", instance_props.runtimeName,
                XR_VERSION_MAJOR(instance_props.runtimeVersion), XR_VERSION_MINOR(instance_props.runtimeVersion),
                XR_VERSION_PATCH(instance_props.runtimeVersion));

    XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!check(instance, xrGetSystem(instance, &system_info, &system), "xrGetSystem")) {
        std::fprintf(stderr, "[xr-test] The runtime sees no headset. Put it on and connect Link, then try again.\n");
        return 1;
    }
    XrSystemProperties system_props{ XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties(instance, system, &system_props);
    std::printf("[xr-test] headset: %s\n", system_props.systemName);

    // ---- the graphics card the runtime asks for -----------------------------
    PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements));
    XrGraphicsRequirementsD3D12KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    if (get_requirements == nullptr ||
        !check(instance, get_requirements(instance, system, &requirements), "xrGetD3D12GraphicsRequirementsKHR")) {
        return 1;
    }

    IDXGIFactory4* factory = nullptr;
    if (!check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1")) return 1;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID)) == 0) {
            std::printf("[xr-test] graphics card: %ls\n", desc.Description);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    if (adapter == nullptr) {
        std::fprintf(stderr, "[xr-test] the runtime asked for a graphics card DXGI does not list\n");
        return 1;
    }

    ID3D12Device* device = nullptr;
    if (!check(D3D12CreateDevice(adapter, requirements.minFeatureLevel, IID_PPV_ARGS(&device)), "D3D12CreateDevice")) return 1;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    if (!check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue")) return 1;
    ID3D12CommandAllocator* allocator = nullptr;
    if (!check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator")) return 1;
    ID3D12GraphicsCommandList* list = nullptr;
    if (!check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)), "CreateCommandList")) return 1;
    list->Close();
    ID3D12Fence* fence = nullptr;
    if (!check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence")) return 1;
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;

    // ---- session and space --------------------------------------------------
    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo session_info{ XR_TYPE_SESSION_CREATE_INFO };
    session_info.next = &binding;
    session_info.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (!check(instance, xrCreateSession(instance, &session_info, &session), "xrCreateSession")) return 1;

    // LOCAL: seated, with the origin where the head was when the session
    // began (or was last recentred from the headset's menu).
    XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace local = XR_NULL_HANDLE;
    if (!check(instance, xrCreateReferenceSpace(session, &space_info, &local), "xrCreateReferenceSpace (local)")) return 1;
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrSpace head = XR_NULL_HANDLE;
    if (!check(instance, xrCreateReferenceSpace(session, &space_info, &head), "xrCreateReferenceSpace (view)")) return 1;

    // ---- one swapchain per eye ----------------------------------------------
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    std::vector<XrViewConfigurationView> config_views(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, config_views.data());
    if (view_count != 2) {
        std::fprintf(stderr, "[xr-test] expected two views, the runtime reports %u\n", view_count);
        return 1;
    }

    uint32_t format_count = 0;
    xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr);
    std::vector<int64_t> formats(format_count);
    xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data());
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    for (DXGI_FORMAT wanted : { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM }) {
        for (int64_t f : formats) {
            if (f == wanted && format == DXGI_FORMAT_UNKNOWN) format = wanted;
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        std::fprintf(stderr, "[xr-test] the runtime offers no 8-bit colour format\n");
        return 1;
    }

    Eye eyes[2];
    UINT image_total = 0;
    for (uint32_t e = 0; e < 2; ++e) {
        eyes[e].width = config_views[e].recommendedImageRectWidth;
        eyes[e].height = config_views[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc_info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc_info.format = format;
        sc_info.sampleCount = 1;
        sc_info.width = eyes[e].width;
        sc_info.height = eyes[e].height;
        sc_info.faceCount = 1;
        sc_info.arraySize = 1;
        sc_info.mipCount = 1;
        if (!check(instance, xrCreateSwapchain(session, &sc_info, &eyes[e].swapchain), "xrCreateSwapchain")) return 1;
        uint32_t count = 0;
        xrEnumerateSwapchainImages(eyes[e].swapchain, 0, &count, nullptr);
        eyes[e].images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
        xrEnumerateSwapchainImages(eyes[e].swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(eyes[e].images.data()));
        eyes[e].rtv_base = image_total;
        image_total += count;
    }
    std::printf("[xr-test] eye images: %u x %u, %u per eye\n", eyes[0].width, eyes[0].height,
                uint32_t(eyes[0].images.size()));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = image_total;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    if (!check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap")) return 1;
    const UINT rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (Eye& eye : eyes) {
        for (size_t i = 0; i < eye.images.size(); ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(eye.rtv_base + i) * rtv_size;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = format;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(eye.images[i].texture, &rtv, handle);
        }
    }

    // ---- the frame loop -----------------------------------------------------
    std::printf("[xr-test] running for %d seconds; left eye red, right eye blue\n", seconds);
    std::fflush(stdout);
    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    bool running = false;       // between xrBeginSession and xrEndSession
    bool quit = false;
    bool exit_requested = false;
    uint64_t frames = 0;

    while (!quit) {
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                std::printf("[xr-test] session state %d\n", int(changed.state));
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{ XR_TYPE_SESSION_BEGIN_INFO };
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = check(instance, xrBeginSession(session, &begin), "xrBeginSession");
                }
                else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                }
                else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = true;
            }
            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - start).count();
        if (!exit_requested && elapsed > seconds) {
            exit_requested = true;
            if (running) xrRequestExitSession(session);
            else quit = true;
        }
        if (!running) {
            Sleep(10);
            continue;
        }

        XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
        if (!check(instance, xrWaitFrame(session, nullptr, &frame_state), "xrWaitFrame")) break;
        if (!check(instance, xrBeginFrame(session, nullptr), "xrBeginFrame")) break;

        XrViewLocateInfo locate{ XR_TYPE_VIEW_LOCATE_INFO };
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frame_state.predictedDisplayTime;
        locate.space = local;
        XrViewState view_state{ XR_TYPE_VIEW_STATE };
        XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t located = 0;
        xrLocateViews(session, &locate, &view_state, 2, &located, views);

        XrCompositionLayerProjectionView projection_views[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
                                                                 { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        const bool render = frame_state.shouldRender &&
                            (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                            (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (render) {
            // A slow pulse, so a frame that stops updating is obvious.
            const float pulse = 0.6f + 0.4f * float(std::sin(elapsed * 2.0));
            const float colours[2][4] = { { pulse, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, pulse, 1.0f } };
            allocator->Reset();
            list->Reset(allocator, nullptr);
            uint32_t acquired[2] = {};
            for (uint32_t e = 0; e < 2; ++e) {
                xrAcquireSwapchainImage(eyes[e].swapchain, nullptr, &acquired[e]);
                XrSwapchainImageWaitInfo wait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wait.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(eyes[e].swapchain, &wait);
                // The runtime hands a colour image over in the render-target
                // state, so it can be cleared without a barrier.
                D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += SIZE_T(eyes[e].rtv_base + acquired[e]) * rtv_size;
                list->ClearRenderTargetView(handle, colours[e], 0, nullptr);

                projection_views[e].pose = views[e].pose;
                projection_views[e].fov = views[e].fov;
                projection_views[e].subImage.swapchain = eyes[e].swapchain;
                projection_views[e].subImage.imageRect.extent = { int32_t(eyes[e].width), int32_t(eyes[e].height) };
            }
            list->Close();
            ID3D12CommandList* lists[] = { list };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence, ++fence_value);
            fence->SetEventOnCompletion(fence_value, fence_event);
            WaitForSingleObject(fence_event, INFINITE);
            for (uint32_t e = 0; e < 2; ++e) {
                xrReleaseSwapchainImage(eyes[e].swapchain, nullptr);
            }
        }

        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = local;
        layer.viewCount = 2;
        layer.views = projection_views;
        const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };
        XrFrameEndInfo end{ XR_TYPE_FRAME_END_INFO };
        end.displayTime = frame_state.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = render ? 1 : 0;
        end.layers = layers;
        if (!check(instance, xrEndFrame(session, &end), "xrEndFrame")) break;
        ++frames;

        // Once a second: where the head is, relative to where it started.
        if (now - last_report >= std::chrono::seconds(1)) {
            const double interval = std::chrono::duration<double>(now - last_report).count();
            last_report = now;
            XrSpaceLocation where{ XR_TYPE_SPACE_LOCATION };
            xrLocateSpace(head, local, frame_state.predictedDisplayTime, &where);
            float yaw = 0.0f, pitch = 0.0f;
            describe(where.pose, &yaw, &pitch);
            const float ipd = std::sqrt(std::pow(views[1].pose.position.x - views[0].pose.position.x, 2.0f) +
                                        std::pow(views[1].pose.position.y - views[0].pose.position.y, 2.0f) +
                                        std::pow(views[1].pose.position.z - views[0].pose.position.z, 2.0f));
            std::printf("[xr-test] %.0f fps  head=(%+.3f, %+.3f, %+.3f) m  yaw=%+6.1f  pitch=%+5.1f  eyes %.1f mm apart\n",
                        double(frames) / interval, where.pose.position.x, where.pose.position.y, where.pose.position.z,
                        yaw, pitch, ipd * 1000.0f);
            std::fflush(stdout);
            frames = 0;
        }
    }

    // ---- teardown -----------------------------------------------------------
    for (Eye& eye : eyes) {
        if (eye.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye.swapchain);
    }
    xrDestroySpace(head);
    xrDestroySpace(local);
    xrDestroySession(session);
    xrDestroyInstance(instance);
    rtv_heap->Release();
    CloseHandle(fence_event);
    fence->Release();
    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    std::printf("[xr-test] done\n");
    return 0;
}

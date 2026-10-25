"""The VR hook in RT64: an offset applied to the camera of every 3D scene.

Wave Race 64 puts its camera on the projection stack, and RT64 splits that
stack back into a view matrix (the camera) and a projection (the lens) for
each projection the game draws with. When it presents a frame,
ProjectionProcessor::processScene takes each of those, interpolates the
camera between the game's two most recent frames, and multiplies the result
by the lens. That point -- after the interpolation, before the multiply -- is
where a headset's pose belongs: it is evaluated at the display's rate rather
than the game's 30 Hz, and nothing the game computes depends on it.

This first patch adds only a translation, in the camera's own space: right,
up and back from where the game put it, in game units. The port sets it
through an extern "C" function so it needs no RT64 headers, the same way the
content crop is set (patch_rt64.py), and it is zero by default, so RT64
behaves as before until a port asks. It is applied to perspective scenes
only; the HUD and menus are orthographic and stay where the game drew them.

The previous frame's camera is moved by the same amount. RT64 keeps it to
judge how far things moved between frames, and moving only the current one
would read as the camera jumping by the whole offset.

Side-by-side stereo is switched on the same way, through RT64_SetStereo. The
VI renderer can then draw into a region of the window rather than the whole
of it, and the present queue draws the frame into each half: the layout a
headset's two eyes use, on a monitor. The VI's own fitting -- the content
crop's included -- lays the picture out inside the half as it would inside
a window that shape, so each eye keeps its proportions.

Each eye is its own rendering. RT64 already draws one game frame several
times when it interpolates -- once per displayed frame, each into its own
spare target -- so the right eye is one more of those drawings: the same
frame and the same interpolation weights, into a target of its own, with the
camera moved right by half the separation where the left eye's was moved
left. It is drawn before the frame is reported ready, so the present queue
never pairs eyes from different moments, and whether a frame is stereo is
decided once and kept with the frame's counters, so the two queues cannot
disagree about it. A frame RT64 has no target to redraw shows the left eye
in both halves.

Scripted and idempotent because it patches a submodule: a submodule update
would otherwise revert it silently.

Run from the repository root, after patch_rt64.py:
    python tools/patch_rt64_vr.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RT64 = REPO / "lib" / "RT64" / "src"
PROJ_PROCESSOR = RT64 / "render" / "rt64_projection_processor.cpp"
VI_HEADER = RT64 / "render" / "rt64_vi_renderer.h"
VI_RENDERER = RT64 / "render" / "rt64_vi_renderer.cpp"
PRESENT_QUEUE = RT64 / "hle" / "rt64_present_queue.cpp"
PRESENT_HEADER = RT64 / "hle" / "rt64_present_queue.h"
WORKLOAD_QUEUE = RT64 / "hle" / "rt64_workload_queue.cpp"
SHARED_RESOURCES = RT64 / "hle" / "rt64_shared_queue_resources.h"

# --- the setter -------------------------------------------------------------

SETTER_ANCHOR = """#include "hle/rt64_workload_queue.h"

namespace RT64 {
"""

SETTER_REPLACEMENT = """#include "hle/rt64_workload_queue.h"

#include <atomic>

namespace RT64 {
    // wr64: an offset for the camera of every perspective scene, in the
    // camera's own space and the game's units: x right, y up, z back. Set by
    // the port from its own thread, read here on the render thread, hence
    // the atomics. All zero (the default) leaves the camera where the game
    // put it.
    static std::atomic<float> wr64ViewOffsetX{ 0.0f };
    static std::atomic<float> wr64ViewOffsetY{ 0.0f };
    static std::atomic<float> wr64ViewOffsetZ{ 0.0f };

    extern "C" void RT64_SetViewOffset(float x, float y, float z) {
        wr64ViewOffsetX.store(x, std::memory_order_relaxed);
        wr64ViewOffsetY.store(y, std::memory_order_relaxed);
        wr64ViewOffsetZ.store(z, std::memory_order_relaxed);
    }

"""

# --- the offset -------------------------------------------------------------

OFFSET_ANCHOR = """            viewProjMatrix = hlslpp::mul(viewMatrix, projMatrix);
"""

OFFSET_REPLACEMENT = """            // wr64: move the camera by the port's offset, after the interpolation
            // above so it is applied at the display's rate. The view matrix
            // takes the world into the camera's space, row vectors as
            // everywhere here; moving the camera by +d there is moving
            // everything it sees by -d, a translation multiplied on after it.
            // Both frames' cameras move, or the offset would read as motion.
            if (proj.type == Projection::Type::Perspective) {
                const float offsetX = wr64ViewOffsetX.load(std::memory_order_relaxed) + wr64EyeOffsetX;
                const float offsetY = wr64ViewOffsetY.load(std::memory_order_relaxed);
                const float offsetZ = wr64ViewOffsetZ.load(std::memory_order_relaxed);
                if ((offsetX != 0.0f) || (offsetY != 0.0f) || (offsetZ != 0.0f)) {
                    const hlslpp::float4x4 offset(
                        1.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 1.0f, 0.0f,
                        -offsetX, -offsetY, -offsetZ, 1.0f);
                    viewMatrix = hlslpp::mul(hlslpp::float4x4(viewMatrix), offset);
                    prevViewTransform = hlslpp::mul(hlslpp::float4x4(prevViewTransform), offset);
                }
            }

            viewProjMatrix = hlslpp::mul(viewMatrix, projMatrix);
"""

# --- the stereo switch ------------------------------------------------------

STEREO_ANCHOR = """        wr64ViewOffsetZ.store(z, std::memory_order_relaxed);
    }
"""

STEREO_REPLACEMENT = """        wr64ViewOffsetZ.store(z, std::memory_order_relaxed);
    }

    // wr64: side-by-side stereo. Mode 0 is off; otherwise each eye is drawn
    // into its half of the window, and the separation is the distance between
    // the eyes in game units. Not static: the present queue reads them too.
    std::atomic<int> wr64StereoMode{ 0 };
    std::atomic<float> wr64StereoSeparation{ 0.0f };

    extern "C" void RT64_SetStereo(int mode, float separation) {
        wr64StereoSeparation.store(separation, std::memory_order_relaxed);
        wr64StereoMode.store(mode, std::memory_order_relaxed);
    }
"""

# --- the eye's own offset ---------------------------------------------------

EYE_ANCHOR = """        wr64StereoMode.store(mode, std::memory_order_relaxed);
    }
"""

EYE_REPLACEMENT = """        wr64StereoMode.store(mode, std::memory_order_relaxed);
    }

    // wr64: the eye being drawn, as a sideways offset on top of the port's.
    // Set by the workload queue around each eye's rendering and read here,
    // both on the render thread, so it needs no atomic.
    float wr64EyeOffsetX = 0.0f;

    // Whether anything moves the camera, which needs the projections
    // processed even in a frame RT64 would otherwise leave alone.
    bool wr64CameraOffsetActive() {
        return (wr64EyeOffsetX != 0.0f) ||
            (wr64ViewOffsetX.load(std::memory_order_relaxed) != 0.0f) ||
            (wr64ViewOffsetY.load(std::memory_order_relaxed) != 0.0f) ||
            (wr64ViewOffsetZ.load(std::memory_order_relaxed) != 0.0f);
    }
"""

# --- the VI renderer draws into a region ------------------------------------

VI_PARAMS_ANCHOR = """            bool removeBlackBorders = false;
        };
"""

VI_PARAMS_REPLACEMENT = """            bool removeBlackBorders = false;

            // wr64: the part of the window to draw into, from regionX and
            // regionWidth pixels wide, for side-by-side stereo. A width of
            // zero (the default) is the whole window.
            float regionX = 0.0f;
            float regionWidth = 0.0f;
        };
"""

VI_DECL_ANCHOR = """        static void getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor);
"""

VI_DECL_REPLACEMENT = """        static void getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor, float regionX = 0.0f, float regionWidth = 0.0f);
"""

VI_CALL_ANCHOR = """        getViewportAndScissor(p.swapChain, *p.vi, p.resolutionScale, p.downsamplingScale, p.removeBlackBorders, viewport, scissor);
"""

VI_CALL_REPLACEMENT = """        getViewportAndScissor(p.swapChain, *p.vi, p.resolutionScale, p.downsamplingScale, p.removeBlackBorders, viewport, scissor, p.regionX, p.regionWidth);
"""

VI_DEF_ANCHOR = """    void VIRenderer::getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor) {
"""

VI_DEF_REPLACEMENT = """    void VIRenderer::getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor, float regionX, float regionWidth) {
"""

VI_SIZE_ANCHOR = """        const hlslpp::float2 windowSize = { float(swapChain->getWidth()), float(swapChain->getHeight()) };
"""

VI_SIZE_REPLACEMENT = """        // wr64: a region is laid out as a window of its own width, then moved
        // into place at the end.
        const float layoutWidth = (regionWidth > 0.0f) ? regionWidth : float(swapChain->getWidth());
        const hlslpp::float2 windowSize = { layoutWidth, float(swapChain->getHeight()) };
"""

VI_REGION_ANCHOR = """            scissor.bottom = std::min(scissor.bottom, int32_t(windowSize.y));
        }
    }
"""

VI_REGION_REPLACEMENT = """            scissor.bottom = std::min(scissor.bottom, int32_t(windowSize.y));
        }

        // wr64: move a region's layout to where the region starts, and keep the
        // scissor inside it so one eye never draws over the other.
        if (regionWidth > 0.0f) {
            const int32_t regionLeft = int32_t(regionX);
            const int32_t regionRight = int32_t(regionX + regionWidth);
            viewport.x += regionX;
            scissor.left = std::min(std::max(scissor.left + regionLeft, regionLeft), regionRight);
            scissor.right = std::min(std::max(scissor.right + regionLeft, regionLeft), regionRight);
        }
    }
"""

# --- the right eye's targets and the frame's stereo flag ---------------------

COUNTERS_ANCHOR = """        uint32_t count = 0;
        bool skipped = false;
    };
"""

COUNTERS_REPLACEMENT = """        uint32_t count = 0;
        bool skipped = false;

        // wr64: whether this frame's eyes were rendered apart, decided once by
        // the workload queue so the present queue pairs them the same way.
        bool wr64Stereo = false;
    };
"""

TARGETS_ANCHOR = """        std::vector<std::unique_ptr<RenderTarget>> interpolatedColorTargets;
"""

TARGETS_REPLACEMENT = """        std::vector<std::unique_ptr<RenderTarget>> interpolatedColorTargets;

        // wr64: the right eye of each displayed frame, in the order the frames
        // are rendered and presented. The left eye is wherever RT64 put it.
        std::vector<std::unique_ptr<RenderTarget>> wr64StereoTargets;
"""

# --- the workload queue renders the right eye -------------------------------

WORKLOAD_EXTERN_ANCHOR = """#define ENABLE_HIGH_RESOLUTION_RENDERER 1

namespace RT64 {
"""

WORKLOAD_EXTERN_REPLACEMENT = """#define ENABLE_HIGH_RESOLUTION_RENDERER 1

#include <atomic>

namespace RT64 {
    // wr64: see rt64_projection_processor.cpp.
    extern std::atomic<int> wr64StereoMode;
    extern std::atomic<float> wr64StereoSeparation;
    extern float wr64EyeOffsetX;
    bool wr64CameraOffsetActive();

    // wr64: the right eye's targets carry modifier keys of their own, clear of
    // the interpolated targets' (one up from 1), so RT64 never mistakes one
    // target's framebuffer for another's.
    constexpr uint32_t wr64StereoModifierBase = 0x10000;

"""

WORKLOAD_PROJ_ANCHOR = """        const bool processProjections = aspectRatioAdjustment || prevFrame.matched|| curFrame.isDebuggerCameraEnabled(*this);
"""

WORKLOAD_PROJ_REPLACEMENT = """        const bool processProjections = aspectRatioAdjustment || prevFrame.matched|| curFrame.isDebuggerCameraEnabled(*this) || wr64CameraOffsetActive();
"""

WORKLOAD_CLEAR_ANCHOR = """            ext.sharedResources->interpolatedColorTargets.clear();
"""

WORKLOAD_CLEAR_REPLACEMENT = """            ext.sharedResources->interpolatedColorTargets.clear();
            ext.sharedResources->wr64StereoTargets.clear();
"""

WORKLOAD_DECIDE_ANCHOR = """                curFrameCounters.count = displayFrames;
"""

WORKLOAD_DECIDE_REPLACEMENT = """                curFrameCounters.count = displayFrames;

                // wr64: decide once whether this frame's eyes are rendered apart.
                // It takes a target RT64 can redraw the frame into; without one
                // the frame is presented as it is, in both halves.
                const float wr64Separation = wr64StereoSeparation.load(std::memory_order_relaxed);
                const bool wr64Stereo = (wr64StereoMode.load(std::memory_order_relaxed) != 0) && !interpolationTargetKey.isEmpty();
                curFrameCounters.wr64Stereo = wr64Stereo;
                if (wr64Stereo) {
                    auto &stereoTargets = ext.sharedResources->wr64StereoTargets;
                    const bool stereoHDR = ext.sharedResources->renderTargetManager.usesHDR;
                    while (stereoTargets.size() < displayFrames) {
                        stereoTargets.emplace_back(std::make_unique<RenderTarget>(interpolationTargetKey.address, Framebuffer::Type::Color, RenderMultisampling(), stereoHDR));
                    }
                }
"""

WORKLOAD_RENDER_ANCHOR = """                    threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                        interpolationTargetKey, interpolationTargetFbPairIndex, overrideTarget, overrideModifier, velocityUploaderUsed, uploadExtras, tileInterpolationUsed, lookAtInterpolationUsed);
"""

WORKLOAD_RENDER_REPLACEMENT = """                    // wr64: in stereo this is the left eye, moved left by half the
                    // separation.
                    wr64EyeOffsetX = wr64Stereo ? (-0.5f * wr64Separation) : 0.0f;
                    threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                        interpolationTargetKey, interpolationTargetFbPairIndex, overrideTarget, overrideModifier, velocityUploaderUsed, uploadExtras, tileInterpolationUsed, lookAtInterpolationUsed);

                    // wr64: and the right eye: the same frame at the same weights,
                    // moved right, into its own target. It is indexed by the frames
                    // rendered so far, which is the order the present queue shows
                    // them in once skipped frames are left out, and it is drawn
                    // before the frame is reported ready below.
                    if (wr64Stereo && (framesRendered < ext.sharedResources->wr64StereoTargets.size())) {
                        RenderTarget *rightTarget = ext.sharedResources->wr64StereoTargets[framesRendered].get();
                        wr64EyeOffsetX = 0.5f * wr64Separation;
                        threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                            interpolationTargetKey, interpolationTargetFbPairIndex, rightTarget, wr64StereoModifierBase + framesRendered, velocityUploaderUsed, false, tileInterpolationUsed, lookAtInterpolationUsed);
                    }

                    wr64EyeOffsetX = 0.0f;
"""

# --- the present queue draws both halves ------------------------------------

PRESENT_EXTERN_ANCHOR = """#include "rt64_workload_queue.h"

namespace RT64 {
"""

PRESENT_EXTERN_REPLACEMENT = """#include "rt64_workload_queue.h"

#include <atomic>

namespace RT64 {
    // wr64: see rt64_projection_processor.cpp.
    extern std::atomic<int> wr64StereoMode;

"""

PRESENT_DRAW_ANCHOR = """                if (renderParams.texture != nullptr) {
                    commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(renderParams.texture, RenderTextureLayout::SHADER_READ));
                    viRenderer->render(renderParams);
                }
"""

PRESENT_DRAW_REPLACEMENT = """                if (renderParams.texture != nullptr) {
                    commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(renderParams.texture, RenderTextureLayout::SHADER_READ));

                    // wr64: side-by-side stereo draws the left eye into the left
                    // half of the window and the right eye into the right. The
                    // right eye is this frame's own target when the workload
                    // queue rendered one; otherwise the left eye fills both.
                    if (wr64StereoMode.load(std::memory_order_relaxed) != 0) {
                        const float halfWidth = float(ext.swapChain->getWidth()) / 2.0f;
                        renderParams.regionWidth = halfWidth;
                        renderParams.regionX = 0.0f;
                        viRenderer->render(renderParams);

                        VIRenderer::RenderParams rightParams = renderParams;
                        rightParams.regionX = halfWidth;
                        auto &stereoTargets = ext.sharedResources->wr64StereoTargets;
                        RenderTarget *rightTarget = (frameCounters.wr64Stereo && (uint32_t(i) < stereoTargets.size())) ? stereoTargets[i].get() : nullptr;
                        if ((rightTarget != nullptr) && (rightTarget->width > 0)) {
                            rightParams.textureFormat = rightTarget->format;
                            rightParams.resolutionScale = rightTarget->resolutionScale;
                            rightParams.downsamplingScale = 1;
                            if (rightTarget->downsampleMultiplier > 1) {
                                rightTarget->downsampleTarget(ext.presentGraphicsWorker, ext.shaderLibrary);
                                rightParams.texture = rightTarget->downsampledTexture.get();
                                rightParams.textureWidth = rightTarget->width / rightTarget->downsampleMultiplier;
                                rightParams.textureHeight = rightTarget->height / rightTarget->downsampleMultiplier;
                                rightParams.downsamplingScale = rightTarget->downsampleMultiplier;
                            }
                            else {
                                rightTarget->resolveTarget(ext.presentGraphicsWorker, ext.shaderLibrary);
                                rightParams.texture = rightTarget->getResolvedTexture();
                                rightParams.textureWidth = rightTarget->width;
                                rightParams.textureHeight = rightTarget->height;
                            }

                            commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(rightParams.texture, RenderTextureLayout::SHADER_READ));
                        }

                        // Its own VI renderer: each holds the texture it samples in
                        // a descriptor set, and both draws are in one command list.
                        wr64RightViRenderer->render(rightParams);
                    }
                    else {
                        viRenderer->render(renderParams);
                    }
                }
"""

PRESENT_MEMBER_ANCHOR = """        std::unique_ptr<VIRenderer> viRenderer;
"""

PRESENT_MEMBER_REPLACEMENT = """        std::unique_ptr<VIRenderer> viRenderer;
        std::unique_ptr<VIRenderer> wr64RightViRenderer; // wr64: the right eye in side-by-side stereo.
"""

PRESENT_SETUP_ANCHOR = """        viRenderer = std::make_unique<VIRenderer>();
"""

PRESENT_SETUP_REPLACEMENT = """        viRenderer = std::make_unique<VIRenderer>();
        wr64RightViRenderer = std::make_unique<VIRenderer>();
"""


def patch(target, anchor, replacement, name):
    text = target.read_text()
    if replacement in text:
        print(f"  {target.name}: {name} already patched")
        return
    if anchor not in text:
        sys.exit(f"anchor for {name} not found in {target}; upstream has changed "
                 f"and this patch needs revisiting")
    target.write_text(text.replace(anchor, replacement, 1))
    print(f"  {target.name}: {name} patched")


def main():
    for target in (PROJ_PROCESSOR, VI_HEADER, VI_RENDERER, PRESENT_QUEUE, PRESENT_HEADER,
                   WORKLOAD_QUEUE, SHARED_RESOURCES):
        if not target.exists():
            sys.exit(f"missing {target}. Run: git submodule update --init --recursive")

    patch(PROJ_PROCESSOR, SETTER_ANCHOR, SETTER_REPLACEMENT, "view offset (setter)")
    patch(PROJ_PROCESSOR, OFFSET_ANCHOR, OFFSET_REPLACEMENT, "view offset (camera)")
    patch(PROJ_PROCESSOR, STEREO_ANCHOR, STEREO_REPLACEMENT, "stereo (switch)")
    patch(PROJ_PROCESSOR, EYE_ANCHOR, EYE_REPLACEMENT, "stereo (eye offset)")
    patch(SHARED_RESOURCES, COUNTERS_ANCHOR, COUNTERS_REPLACEMENT, "stereo (frame flag)")
    patch(SHARED_RESOURCES, TARGETS_ANCHOR, TARGETS_REPLACEMENT, "stereo (right eye targets)")
    patch(WORKLOAD_QUEUE, WORKLOAD_EXTERN_ANCHOR, WORKLOAD_EXTERN_REPLACEMENT, "stereo (workload switch)")
    patch(WORKLOAD_QUEUE, WORKLOAD_PROJ_ANCHOR, WORKLOAD_PROJ_REPLACEMENT, "stereo (projections processed)")
    patch(WORKLOAD_QUEUE, WORKLOAD_CLEAR_ANCHOR, WORKLOAD_CLEAR_REPLACEMENT, "stereo (targets cleared)")
    patch(WORKLOAD_QUEUE, WORKLOAD_DECIDE_ANCHOR, WORKLOAD_DECIDE_REPLACEMENT, "stereo (frame decided)")
    patch(WORKLOAD_QUEUE, WORKLOAD_RENDER_ANCHOR, WORKLOAD_RENDER_REPLACEMENT, "stereo (right eye rendered)")
    patch(PRESENT_HEADER, PRESENT_MEMBER_ANCHOR, PRESENT_MEMBER_REPLACEMENT, "stereo (right VI renderer)")
    patch(PRESENT_QUEUE, PRESENT_SETUP_ANCHOR, PRESENT_SETUP_REPLACEMENT, "stereo (right VI renderer setup)")
    patch(VI_HEADER, VI_PARAMS_ANCHOR, VI_PARAMS_REPLACEMENT, "stereo (region params)")
    patch(VI_HEADER, VI_DECL_ANCHOR, VI_DECL_REPLACEMENT, "stereo (region declaration)")
    patch(VI_RENDERER, VI_CALL_ANCHOR, VI_CALL_REPLACEMENT, "stereo (region call)")
    patch(VI_RENDERER, VI_DEF_ANCHOR, VI_DEF_REPLACEMENT, "stereo (region definition)")
    patch(VI_RENDERER, VI_SIZE_ANCHOR, VI_SIZE_REPLACEMENT, "stereo (region size)")
    patch(VI_RENDERER, VI_REGION_ANCHOR, VI_REGION_REPLACEMENT, "stereo (region placement)")
    patch(PRESENT_QUEUE, PRESENT_EXTERN_ANCHOR, PRESENT_EXTERN_REPLACEMENT, "stereo (present switch)")
    patch(PRESENT_QUEUE, PRESENT_DRAW_ANCHOR, PRESENT_DRAW_REPLACEMENT, "stereo (present halves)")
    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()

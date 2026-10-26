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

A headset is a second output beside the window. The port registers two
callbacks through RT64_SetXrOutput, and the present queue calls the first
before each displayed frame: it waits until the headset wants a frame and
hands over the two eye images to draw into, which the VI renderer fills from
the same pictures as the window's halves. The second hands the frame back
once the command list that drew it has finished. While a headset sets the
pace this way the present queue does not also pace itself to the window,
and RT64_SetXrRefreshRate lets the frame rate follow the headset's rather
than stop at the window's. RT64 knows nothing of OpenXR: the images arrive
as Direct3D 12 resources, wrapped as plume textures the way the swap chain's
buffers are.

The headset's own lens replaces the game's while it is active. The port
passes each eye's view angles through RT64_SetXrLens, and every full-frame
perspective scene drawn for that eye gets an asymmetric frustum built from
them, keeping the game's near and far planes and scale. RT64 renders each
eye about as tall as the headset's eye images (RT64_SetXrRenderHeight), at
the game's 4:3 rather than widened, and the VI renderer stretches that frame
over the whole eye image, since the lens has already said what it covers.

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

            // wr64: in a headset, a scene the game draws across its whole frame
            // is seen through the eye's own lens rather than the game's. Only a
            // scene RT64 judged for itself (aspect AUTO) and found full-frame:
            // the race's world and sky are, the 3D drawn inside a menu's layout
            // (aspect ADJUST, set by the port's display-list rewriter) is not.
            //
            // The game's frame need not be centred on its camera: an off-centre
            // projection puts the camera's direction somewhere other than the
            // middle of the picture. The eye is first turned so that what the
            // game shows in the middle of its frame is straight ahead, then
            // given the lens, which is centred on the eye.
            if ((proj.type == Projection::Type::Perspective) && (curProjGroup.aspectMode == G_EX_ASPECT_AUTO) &&
                adjustAspectRatio && wr64XrLensActive.load(std::memory_order_relaxed)) {
                const float centreX = (projMatrix[0][0] != 0.0f) ? (projMatrix[2][0] / projMatrix[0][0]) : 0.0f;
                const float centreY = (projMatrix[1][1] != 0.0f) ? (projMatrix[2][1] / projMatrix[1][1]) : 0.0f;
                static bool reportedCentre = false;
                if (!reportedCentre) {
                    reportedCentre = true;
                    fprintf(stderr, "[wr64-vr] the game's frame is centred %.1f degrees right and %.1f degrees up of its camera\\n",
                        atanf(centreX) * 57.2957795f, atanf(centreY) * 57.2957795f);
                    fflush(stderr);
                }

                if ((centreX != 0.0f) || (centreY != 0.0f)) {
                    const hlslpp::float4x4 recentre = wr64Recentre(centreX, centreY);
                    viewMatrix = hlslpp::mul(hlslpp::float4x4(viewMatrix), recentre);
                    prevViewTransform = hlslpp::mul(hlslpp::float4x4(prevViewTransform), recentre);
                }

                projMatrix = wr64EyeLens(projMatrix, wr64EyeIndex);
                prevProjTransform = projMatrix;
            }

            viewProjMatrix = hlslpp::mul(viewMatrix, projMatrix);
"""

# --- the headset's lens -----------------------------------------------------

LENS_ANCHOR = """    inline void adjustProjectionMatrix(interop::float4x4 &matrix, const float aspectRatioScale) {
"""

LENS_REPLACEMENT = """    // wr64: a headset's lens for each eye, as the tangents of the angles its
    // view reaches left, right, up and down (left and down negative), and
    // whether to use them. Set by the port from its present-thread callback,
    // read on the render thread.
    static std::atomic<bool> wr64XrLensActive{ false };
    static std::atomic<float> wr64XrLensTangents[2][4];

    // wr64: the eye being drawn, 0 left and 1 right. Set by the workload queue
    // around each eye's rendering, like wr64EyeOffsetX and on the same thread.
    int wr64EyeIndex = 0;

    extern "C" void RT64_SetXrLens(int active, const float *angles) {
        if (active && (angles != nullptr)) {
            for (int eye = 0; eye < 2; eye++) {
                for (int side = 0; side < 4; side++) {
                    wr64XrLensTangents[eye][side].store(std::tan(angles[eye * 4 + side]), std::memory_order_relaxed);
                }
            }
        }

        wr64XrLensActive.store(active != 0, std::memory_order_relaxed);
    }

    // wr64: the eye's lens in place of a game's perspective projection. The
    // game's near and far planes are kept, read from the projection as the
    // camera log reads them, and so is its overall scale: libultra's
    // guPerspective multiplies the whole matrix through by one, and [2][3] is
    // minus it, so w comes out the same as the game's and nothing downstream
    // sees a difference beyond the shape of the view. An asymmetric frustum,
    // row vectors, OpenGL's clip space, as the N64's own.
    inline interop::float4x4 wr64EyeLens(const interop::float4x4 &game, int eye) {
        const float scale = -game[2][3];
        if (scale == 0.0f) {
            return game;
        }

        const float tanLeft = wr64XrLensTangents[eye][0].load(std::memory_order_relaxed);
        const float tanRight = wr64XrLensTangents[eye][1].load(std::memory_order_relaxed);
        const float tanUp = wr64XrLensTangents[eye][2].load(std::memory_order_relaxed);
        const float tanDown = wr64XrLensTangents[eye][3].load(std::memory_order_relaxed);
        if ((tanRight <= tanLeft) || (tanUp <= tanDown)) {
            return game;
        }

        interop::float4x4 lens = interop::float4x4::identity();
        for (int row = 0; row < 4; row++) {
            for (int column = 0; column < 4; column++) {
                lens[row][column] = 0.0f;
            }
        }

        lens[0][0] = 2.0f / (tanRight - tanLeft);
        lens[1][1] = 2.0f / (tanUp - tanDown);
        lens[2][0] = (tanRight + tanLeft) / (tanRight - tanLeft);
        lens[2][1] = (tanUp + tanDown) / (tanUp - tanDown);
        lens[2][2] = game[2][2] / scale;
        lens[2][3] = -1.0f;
        lens[3][2] = game[3][2] / scale;
        for (int row = 0; row < 4; row++) {
            for (int column = 0; column < 4; column++) {
                lens[row][column] *= scale;
            }
        }

        return lens;
    }

    // wr64: a turn of the view that brings the direction a game's frame is
    // centred on -- tangents centreX right and centreY up, read off its
    // projection -- to straight ahead: a yaw, then a pitch. Row vectors, for
    // multiplying on after a view matrix.
    inline hlslpp::float4x4 wr64Recentre(float centreX, float centreY) {
        const float yawLength = sqrtf(1.0f + centreX * centreX);
        const float yawCos = 1.0f / yawLength;
        const float yawSin = centreX / yawLength;
        const hlslpp::float4x4 yaw(
            yawCos, 0.0f, -yawSin, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            yawSin, 0.0f, yawCos, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);

        // After the yaw the centre is centreY up over a run of yawLength.
        const float pitchTangent = centreY / yawLength;
        const float pitchLength = sqrtf(1.0f + pitchTangent * pitchTangent);
        const float pitchCos = 1.0f / pitchLength;
        const float pitchSin = -pitchTangent / pitchLength;
        const hlslpp::float4x4 pitch(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, pitchCos, pitchSin, 0.0f,
            0.0f, -pitchSin, pitchCos, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);

        return hlslpp::mul(yaw, pitch);
    }

    inline void adjustProjectionMatrix(interop::float4x4 &matrix, const float aspectRatioScale) {
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
            // zero (the default) is the whole window. regionHeight is the
            // height of the target when it is not the window -- a headset's
            // eye image -- and zero is the window's.
            float regionX = 0.0f;
            float regionWidth = 0.0f;
            float regionHeight = 0.0f;

            // wr64: multiplies the VI's gamma. A target that encodes to sRGB
            // as it is written is given the VI's output linearised (2.2), so
            // the two cancel rather than brightening the picture twice.
            float gammaScale = 1.0f;

            // wr64: stretch the whole frame over the whole region, shape and
            // borders ignored -- a headset's eye image, whose lens already
            // describes the frame RT64 rendered. Needs regionWidth and
            // regionHeight.
            bool fillRegion = false;
        };
"""

VI_FILL_ANCHOR = """        p.commandList->setViewports(viewport);
        p.commandList->setScissors(scissor);
"""

VI_FILL_REPLACEMENT = """        // wr64: see fillRegion.
        if (p.fillRegion && (p.regionWidth > 0.0f) && (p.regionHeight > 0.0f)) {
            viewport = RenderViewport(p.regionX, 0.0f, p.regionWidth, p.regionHeight);
            scissor = RenderRect(int32_t(p.regionX), 0, int32_t(p.regionX + p.regionWidth), int32_t(p.regionHeight));
        }

        p.commandList->setViewports(viewport);
        p.commandList->setScissors(scissor);
"""

VI_DECL_ANCHOR = """        static void getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor);
"""

VI_DECL_REPLACEMENT = """        static void getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor, float regionX = 0.0f, float regionWidth = 0.0f, float regionHeight = 0.0f);
"""

VI_CALL_ANCHOR = """        getViewportAndScissor(p.swapChain, *p.vi, p.resolutionScale, p.downsamplingScale, p.removeBlackBorders, viewport, scissor);
"""

VI_CALL_REPLACEMENT = """        getViewportAndScissor(p.swapChain, *p.vi, p.resolutionScale, p.downsamplingScale, p.removeBlackBorders, viewport, scissor, p.regionX, p.regionWidth, p.regionHeight);
"""

VI_DEF_ANCHOR = """    void VIRenderer::getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor) {
"""

VI_DEF_REPLACEMENT = """    void VIRenderer::getViewportAndScissor(const RenderSwapChain *swapChain, const VI &vi, hlslpp::float2 resolutionScale, uint32_t downsamplingScale, bool removeBlackBorders, RenderViewport &viewport, RenderRect &scissor, float regionX, float regionWidth, float regionHeight) {
"""

VI_SIZE_ANCHOR = """        const hlslpp::float2 windowSize = { float(swapChain->getWidth()), float(swapChain->getHeight()) };
"""

VI_SIZE_REPLACEMENT = """        // wr64: a region is laid out as a window of its own width -- and, in a
        // target that is not the window, its own height -- then moved into
        // place at the end.
        const float layoutWidth = (regionWidth > 0.0f) ? regionWidth : float(swapChain->getWidth());
        const float layoutHeight = (regionHeight > 0.0f) ? regionHeight : float(swapChain->getHeight());
        const hlslpp::float2 windowSize = { layoutWidth, layoutHeight };
"""

VI_GAMMA_ANCHOR = """        pushConstants.gamma = p.vi->gamma();
"""

VI_GAMMA_REPLACEMENT = """        pushConstants.gamma = p.vi->gamma() * p.gammaScale; // wr64: see gammaScale.
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
    extern int wr64EyeIndex;
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
                    wr64EyeIndex = 0;
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
                        wr64EyeIndex = 1;
                        wr64EyeOffsetX = 0.5f * wr64Separation;
                        threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                            interpolationTargetKey, interpolationTargetFbPairIndex, rightTarget, wr64StereoModifierBase + framesRendered, velocityUploaderUsed, false, tileInterpolationUsed, lookAtInterpolationUsed);
                    }

                    wr64EyeIndex = 0;
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

PRESENT_DRAW_REPLACEMENT = """                // wr64: what each eye was drawn to the window from, for the
                // headset's images below. Without a picture this frame both stay
                // empty, and the headset's images are only cleared.
                VIRenderer::RenderParams wr64EyeParams[2];

                if (renderParams.texture != nullptr) {
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
                        wr64EyeParams[0] = renderParams;
                        wr64EyeParams[1] = rightParams;
                    }
                    else {
                        viRenderer->render(renderParams);
                        wr64EyeParams[0] = renderParams;
                        wr64EyeParams[1] = renderParams;
                    }
                }
"""

# --- a headset as a second output -------------------------------------------

XR_DECL_ANCHOR = """    // PresentQueue

    PresentQueue::PresentQueue() {
"""

XR_DECL_REPLACEMENT = """    // wr64: a headset as a second output, supplied by the port (src/vr_openxr.cpp,
    // which mirrors these two structs and must keep them identical). begin
    // waits until the headset wants a frame and returns -1 for no headset frame
    // (none attached, or its session not running), 0 for a frame it does not
    // want drawn, and 1 for a frame whose eye images are to be drawn; every
    // frame begun is handed back through end, drawn or not. writeRaw says the
    // images take the VI's output as it is; otherwise they encode to sRGB as
    // they are written, and are given it linearised.
    struct RT64_XrEye {
        void *resource;
        uint32_t width;
        uint32_t height;
    };

    struct RT64_XrFrame {
        RT64_XrEye eyes[2];
        int writeRaw;
    };

    typedef int (*RT64_XrBeginFrame)(void *user, RT64_XrFrame *frame);
    typedef void (*RT64_XrEndFrame)(void *user, int rendered);

    struct Wr64XrOutput {
        RT64_XrBeginFrame begin = nullptr;
        RT64_XrEndFrame end = nullptr;
        void *user = nullptr;
    };

    static std::mutex wr64XrOutputMutex;
    static Wr64XrOutput wr64XrOutput;

    // wr64: the headset's refresh rate while it sets the pace, zero otherwise.
    // Read by the workload queue, which renders frames for that rate.
    std::atomic<uint32_t> wr64XrRefreshRate{ 0 };

    extern "C" void RT64_SetXrOutput(RT64_XrBeginFrame begin, RT64_XrEndFrame end, void *user) {
        std::scoped_lock lock(wr64XrOutputMutex);
        wr64XrOutput.begin = begin;
        wr64XrOutput.end = end;
        wr64XrOutput.user = user;
    }

    extern "C" void RT64_SetXrRefreshRate(uint32_t rate) {
        wr64XrRefreshRate.store(rate, std::memory_order_relaxed);
    }

    // wr64: the height of the headset's eye images while it sets the pace,
    // zero otherwise. The workload queue renders each eye about that tall.
    std::atomic<uint32_t> wr64XrRenderHeight{ 0 };

    extern "C" void RT64_SetXrRenderHeight(uint32_t height) {
        wr64XrRenderHeight.store(height, std::memory_order_relaxed);
    }

    // PresentQueue

    PresentQueue::PresentQueue() {
"""

XR_INCLUDE_ANCHOR = """#include "rt64_present_queue.h"

#include "common/rt64_thread.h"
"""

XR_INCLUDE_REPLACEMENT = """#include "rt64_present_queue.h"

#include "common/rt64_thread.h"

#if defined(_WIN32)
// wr64: the headset's eye images are Direct3D 12 resources, wrapped as plume
// textures to draw into.
#   include "plume_d3d12.h"
#endif
"""

XR_BEGIN_ANCHOR = """            uint32_t swapChainIndex = 0;
"""

XR_BEGIN_REPLACEMENT = """            // wr64: when a headset is attached it sets the pace: beginning its frame
            // waits until it wants one. Without one the wrapped images it left
            // are forgotten.
            Wr64XrOutput wr64Xr;
            {
                std::scoped_lock lock(wr64XrOutputMutex);
                wr64Xr = wr64XrOutput;
            }
            if (wr64Xr.begin == nullptr) {
                wr64XrTargets.clear();
            }

            RT64_XrFrame wr64XrFrame = {};
            const int wr64XrState = (wr64Xr.begin != nullptr) ? wr64Xr.begin(wr64Xr.user, &wr64XrFrame) : -1;
            bool wr64XrRendered = false;

            uint32_t swapChainIndex = 0;
"""

XR_DRAW_ANCHOR = """                RenderHookDraw *drawHook = GetRenderHookDraw();
"""

XR_DRAW_REPLACEMENT = """#               if defined(_WIN32)
                // wr64: the headset's eye images, drawn from the same pictures as
                // the window's halves. Each image is wrapped once, by its resource,
                // as a plume texture with a framebuffer of its own, the way the
                // swap chain's buffers are; the wrapper owns nothing, so the
                // runtime's images are never released here. The runtime hands an
                // image over in the render-target state and takes it back so.
                if (wr64XrState == 1) {
                    for (uint32_t eye = 0; eye < 2; eye++) {
                        const RT64_XrEye &xrEye = wr64XrFrame.eyes[eye];
                        Wr64XrTarget &xrTarget = wr64XrTargets[xrEye.resource];
                        if (xrTarget.texture == nullptr) {
                            auto xrTexture = std::make_unique<D3D12Texture>();
                            xrTexture->d3d = static_cast<ID3D12Resource *>(xrEye.resource);
                            xrTexture->device = static_cast<D3D12Device *>(ext.device);
                            xrTexture->desc.dimension = RenderTextureDimension::TEXTURE_2D;
                            xrTexture->desc.width = xrEye.width;
                            xrTexture->desc.height = xrEye.height;
                            xrTexture->desc.depth = 1;
                            xrTexture->desc.mipLevels = 1;
                            xrTexture->desc.arraySize = 1;
                            // Raw: a view that writes the bytes as they are. Otherwise
                            // the view takes the image's own sRGB format.
                            xrTexture->desc.format = wr64XrFrame.writeRaw ? RenderFormat::R8G8B8A8_UNORM : RenderFormat::UNKNOWN;
                            xrTexture->desc.flags = RenderTextureFlag::RENDER_TARGET;
                            const RenderTexture *xrAttachment = xrTexture.get();
                            xrTarget.framebuffer = ext.device->createFramebuffer(RenderFramebufferDesc(&xrAttachment, 1));
                            xrTarget.texture = std::move(xrTexture);
                        }

                        D3D12Texture *xrTexture = static_cast<D3D12Texture *>(xrTarget.texture.get());
                        xrTexture->resourceStates = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        xrTexture->layout = RenderTextureLayout::COLOR_WRITE;
                        commandList->setFramebuffer(xrTarget.framebuffer.get());
                        commandList->clearColor();
                        if (wr64EyeParams[eye].texture != nullptr) {
                            VIRenderer::RenderParams xrParams = wr64EyeParams[eye];
                            xrParams.regionX = 0.0f;
                            xrParams.regionWidth = float(xrEye.width);
                            xrParams.regionHeight = float(xrEye.height);
                            xrParams.gammaScale = wr64XrFrame.writeRaw ? 1.0f : 2.2f;
                            xrParams.fillRegion = true;
                            if (wr64XrViRenderers[eye] == nullptr) {
                                wr64XrViRenderers[eye] = std::make_unique<VIRenderer>();
                            }

                            wr64XrViRenderers[eye]->render(xrParams);
                        }
                    }

                    commandList->setFramebuffer(swapChainFramebuffer);
                    wr64XrRendered = true;
                }
#               endif

                RenderHookDraw *drawHook = GetRenderHookDraw();
"""

XR_END_ANCHOR = """                    ext.presentGraphicsWorker->wait();
                }
            }
"""

XR_END_REPLACEMENT = """                    ext.presentGraphicsWorker->wait();
                }
            }

            // wr64: hand the headset its frame back, drawn or not. The command list
            // that drew its images has been executed and waited on, so the GPU is
            // done with them before the runtime takes them back.
            if (wr64XrState >= 0) {
                wr64Xr.end(wr64Xr.user, wr64XrRendered ? 1 : 0);
            }
"""

XR_SLEEP_ANCHOR = """                if ((presentTimestamp != Timestamp()) && (targetRate > 0) && (targetRate > viOriginalRate)) {
"""

XR_SLEEP_REPLACEMENT = """                // wr64: not while a headset sets the pace; its frame wait already has.
                if ((wr64XrState < 0) && (presentTimestamp != Timestamp()) && (targetRate > 0) && (targetRate > viOriginalRate)) {
"""

XR_WAIT_ANCHOR = """                if (presentWaitEnabled) {
"""

XR_WAIT_REPLACEMENT = """                // wr64: nor wait for the window's refresh, which would hold the
                // headset to it.
                if (presentWaitEnabled && (wr64XrState < 0)) {
"""

XR_MEMBERS_ANCHOR = """        std::unique_ptr<Inspector> inspector;
"""

XR_MEMBERS_REPLACEMENT = """        std::unique_ptr<Inspector> inspector;

        // wr64: the headset's eye images, wrapped to draw into and kept by the
        // runtime's resource, and a VI renderer for each eye.
        struct Wr64XrTarget {
            std::unique_ptr<RenderTexture> texture;
            std::unique_ptr<RenderFramebuffer> framebuffer;
        };
        std::unordered_map<void *, Wr64XrTarget> wr64XrTargets;
        std::unique_ptr<VIRenderer> wr64XrViRenderers[2];
"""

XR_HEADER_INCLUDE_ANCHOR = """#include "rt64_shared_queue_resources.h"
"""

XR_HEADER_INCLUDE_REPLACEMENT = """#include "rt64_shared_queue_resources.h"

#include <unordered_map> // wr64: the headset's wrapped eye images.
"""

# --- the frame rate follows the headset -------------------------------------

XR_RATE_EXTERN_ANCHOR = """#include "rt64_present_queue.h"

#define ENABLE_HIGH_RESOLUTION_RENDERER 1
"""

XR_RATE_EXTERN_REPLACEMENT = """#include "rt64_present_queue.h"

#include <atomic>

namespace RT64 {
    // wr64: the headset's refresh rate and eye image height while it sets the
    // pace, zero otherwise (rt64_present_queue.cpp).
    extern std::atomic<uint32_t> wr64XrRefreshRate;
    extern std::atomic<uint32_t> wr64XrRenderHeight;
};

#define ENABLE_HIGH_RESOLUTION_RENDERER 1
"""

XR_SIZE_ANCHOR = """        ext.sharedResources->resolutionScale = workloadConfig.resolutionScale;
"""

# Kept a few lines clear of the water patch's change above it: patch_water.py
# recognises its own work by reversing it, which needs its context untouched.
XR_SIZE_REPLACEMENT = """        ext.sharedResources->resolutionScale = workloadConfig.resolutionScale;

        // wr64: while a headset sets the pace, render for its eyes instead of the
        // window: about as tall as its eye images, in whole multiples as RT64
        // prefers, and at the game's own 4:3, since each eye's lens replaces
        // the game's and RT64 never draws narrower than its source. The
        // nearly square eye image then takes the frame squeezed sideways.
        const uint32_t wr64XrHeight = wr64XrRenderHeight.load(std::memory_order_relaxed);
        if (wr64XrHeight > 0) {
            const float wr64Multiplier = std::max(1.0f, std::round(float(wr64XrHeight) / float(referenceHeight)));
            workloadConfig.aspectRatioTarget = workloadConfig.aspectRatioSource;
            workloadConfig.aspectRatioScale = 1.0f;
            workloadConfig.resolutionScale = { wr64Multiplier, wr64Multiplier };
            ext.sharedResources->resolutionScale = workloadConfig.resolutionScale;
        }
"""

XR_RATE_ANCHOR = """        ext.sharedResources->targetRate = workloadConfig.targetRate;
"""

XR_RATE_REPLACEMENT = """        // wr64: a headset that sets the pace sets the frame rate too, past the
        // window's refresh rate that caps every other choice above.
        const uint32_t wr64XrRate = wr64XrRefreshRate.load(std::memory_order_relaxed);
        if (wr64XrRate > 0) {
            workloadConfig.targetRate = wr64XrRate;
        }

        ext.sharedResources->targetRate = workloadConfig.targetRate;
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
    patch(PROJ_PROCESSOR, LENS_ANCHOR, LENS_REPLACEMENT, "headset (lens)")
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
    patch(VI_RENDERER, VI_GAMMA_ANCHOR, VI_GAMMA_REPLACEMENT, "headset (gamma)")
    patch(VI_RENDERER, VI_FILL_ANCHOR, VI_FILL_REPLACEMENT, "headset (fill)")
    patch(PRESENT_QUEUE, PRESENT_EXTERN_ANCHOR, PRESENT_EXTERN_REPLACEMENT, "stereo (present switch)")
    patch(PRESENT_QUEUE, PRESENT_DRAW_ANCHOR, PRESENT_DRAW_REPLACEMENT, "stereo (present halves)")
    patch(PRESENT_QUEUE, XR_INCLUDE_ANCHOR, XR_INCLUDE_REPLACEMENT, "headset (include)")
    patch(PRESENT_QUEUE, XR_DECL_ANCHOR, XR_DECL_REPLACEMENT, "headset (output)")
    patch(PRESENT_QUEUE, XR_BEGIN_ANCHOR, XR_BEGIN_REPLACEMENT, "headset (frame begun)")
    patch(PRESENT_QUEUE, XR_DRAW_ANCHOR, XR_DRAW_REPLACEMENT, "headset (eye images drawn)")
    patch(PRESENT_QUEUE, XR_END_ANCHOR, XR_END_REPLACEMENT, "headset (frame ended)")
    patch(PRESENT_QUEUE, XR_SLEEP_ANCHOR, XR_SLEEP_REPLACEMENT, "headset (no window pacing)")
    patch(PRESENT_QUEUE, XR_WAIT_ANCHOR, XR_WAIT_REPLACEMENT, "headset (no window wait)")
    patch(PRESENT_HEADER, XR_HEADER_INCLUDE_ANCHOR, XR_HEADER_INCLUDE_REPLACEMENT, "headset (header include)")
    patch(PRESENT_HEADER, XR_MEMBERS_ANCHOR, XR_MEMBERS_REPLACEMENT, "headset (wrapped images)")
    patch(WORKLOAD_QUEUE, XR_RATE_EXTERN_ANCHOR, XR_RATE_EXTERN_REPLACEMENT, "headset (rate switch)")
    patch(WORKLOAD_QUEUE, XR_RATE_ANCHOR, XR_RATE_REPLACEMENT, "headset (frame rate)")
    patch(WORKLOAD_QUEUE, XR_SIZE_ANCHOR, XR_SIZE_REPLACEMENT, "headset (eye size)")
    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()

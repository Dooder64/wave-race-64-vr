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
                const float offsetX = wr64ViewOffsetX.load(std::memory_order_relaxed);
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

                    // wr64: side-by-side stereo draws into each half of the
                    // window. Both halves show the same picture until each eye
                    // is rendered on its own.
                    if (wr64StereoMode.load(std::memory_order_relaxed) != 0) {
                        const float halfWidth = float(ext.swapChain->getWidth()) / 2.0f;
                        renderParams.regionWidth = halfWidth;
                        renderParams.regionX = 0.0f;
                        viRenderer->render(renderParams);
                        renderParams.regionX = halfWidth;
                        viRenderer->render(renderParams);
                    }
                    else {
                        viRenderer->render(renderParams);
                    }
                }
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
    for target in (PROJ_PROCESSOR, VI_HEADER, VI_RENDERER, PRESENT_QUEUE):
        if not target.exists():
            sys.exit(f"missing {target}. Run: git submodule update --init --recursive")

    patch(PROJ_PROCESSOR, SETTER_ANCHOR, SETTER_REPLACEMENT, "view offset (setter)")
    patch(PROJ_PROCESSOR, OFFSET_ANCHOR, OFFSET_REPLACEMENT, "view offset (camera)")
    patch(PROJ_PROCESSOR, STEREO_ANCHOR, STEREO_REPLACEMENT, "stereo (switch)")
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

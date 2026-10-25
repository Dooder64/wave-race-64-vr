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

Scripted and idempotent because it patches a submodule: a submodule update
would otherwise revert it silently.

Run from the repository root, after patch_rt64.py:
    python tools/patch_rt64_vr.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROJ_PROCESSOR = REPO / "lib" / "RT64" / "src" / "render" / "rt64_projection_processor.cpp"

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
    if not PROJ_PROCESSOR.exists():
        sys.exit(f"missing {PROJ_PROCESSOR}. Run: git submodule update --init --recursive")

    patch(PROJ_PROCESSOR, SETTER_ANCHOR, SETTER_REPLACEMENT, "view offset (setter)")
    patch(PROJ_PROCESSOR, OFFSET_ANCHOR, OFFSET_REPLACEMENT, "view offset (camera)")
    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()

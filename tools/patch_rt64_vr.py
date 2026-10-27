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

Head tracking goes between the turn that centres the game's frame and the
lens: the port publishes, each headset frame, a transform per eye from the
seated space into that eye (RT64_SetXrHead), with a tag naming the pose. The
workload queue takes one snapshot of it per displayed frame, for both eyes,
and the tag travels with the frame to the present queue, which hands it back
when the frame is submitted -- so the headset is told the pose each frame
was really rendered with, and its compositor can correct for the head's
movement since.

What a frame draws flat -- a race's HUD, the title's lettering over its 3D
backdrop, a menu's labels -- cannot be stretched over each eye's view like
the rest: the eyes' lenses look different ways, and the two copies would
never line up. In a frame that put a scene through the lens, everything
drawn flat (orthographic projections, a menu's 3D laid out in a box of its
own, and screen rectangles with their scissors) is moved onto a panel of the
frame's shape (RT64_SetXrPanel), hung straight ahead of the seated position
-- where the game's camera looks -- and placed in each eye as that eye sees
it, so the two agree and the panel stays put as the head turns; or, if the
port asks, kept in front of the eyes. A frame that put nothing
through the lens -- a menu with nothing 3D behind it -- is not shown to the
eyes at all: the workload queue notes which frames these are, and the
present queue draws such a frame, as the left eye was drawn, onto a third
image the port supplies, which it hangs in front of the player as a flat
screen; the end callback says which was drawn.

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
FRAMEBUFFER_RENDERER = RT64 / "render" / "rt64_framebuffer_renderer.cpp"

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

                // The head: where this eye is, and which way it faces, relative to
                // the seated position, as the frame's snapshot holds it. Straight
                // ahead for the head is now the middle of the game's frame.
                if (wr64HeadActive) {
                    const float *e = wr64HeadEye[wr64EyeIndex];
                    const hlslpp::float4x4 head(
                        e[0], e[1], e[2], e[3],
                        e[4], e[5], e[6], e[7],
                        e[8], e[9], e[10], e[11],
                        e[12], e[13], e[14], e[15]);
                    viewMatrix = hlslpp::mul(hlslpp::float4x4(viewMatrix), head);
                    prevViewTransform = hlslpp::mul(hlslpp::float4x4(prevViewTransform), head);
                }

                projMatrix = wr64EyeLens(projMatrix, wr64EyeIndex);
                // wr64: in the HUD's own passes the scene is left out: nothing
                // seen through the lens reaches the picture.
                if (wr64PanelPass != 0) {
                    for (int row = 0; row < 4; row++) {
                        for (int column = 0; column < 4; column++) {
                            projMatrix[row][column] = 0.0f;
                        }
                    }
                }

                prevProjTransform = projMatrix;
                wr64LensUsed = true;
            }
            else if (wr64XrLensActive.load(std::memory_order_relaxed) &&
                ((proj.type == Projection::Type::Perspective) || (proj.type == Projection::Type::Orthographic))) {
                // wr64: drawn flat; moved onto the panel by process() once the
                // frame is known to have put a scene through the lens.
                wr64FlatTransforms.push_back({ uint32_t(sceneProj.workloadIndex), uint32_t(proj.transformsIndex),
                    (proj.type == Projection::Type::Perspective) && (curProjGroup.aspectMode == G_EX_ASPECT_ADJUST),
                    &drawData, float(fbPair.colorImage.width) });
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

    // wr64: whether the frame being drawn put a scene through a headset's lens.
    // Cleared by the workload queue before each displayed frame's first eye,
    // set as its projections are processed, and read by the framebuffer
    // renderer and the workload queue, all on the render thread.
    bool wr64LensUsed = false;

    // wr64: the projections of the frame being drawn that were not put through
    // the lens -- its flat elements, and 3D that is not the scene -- by
    // workload and transform, and whether each is framed 3D, which the port
    // marks with G_EX_ASPECT_ADJUST (a menu's model in a box of its own), for
    // process() to move onto the panel.
    struct Wr64FlatTransform {
        uint32_t workload;
        uint32_t transform;
        bool framed;
        const void *drawData;
        float frameWidth;
    };
    static std::vector<Wr64FlatTransform> wr64FlatTransforms;

    // wr64: the panel a frame's flat elements are drawn on inside a headset's
    // eyes, set by the port: how far ahead and how wide, and how far apart
    // the eyes are, in the units of the head's transforms (game units). It is
    // the frame's 4:3. It hangs straight ahead of the seated position, where
    // the game's camera looks, and stays there as the head turns -- the flat
    // picture where the game framed it, with the 3D scene around it to look
    // at -- unless followHead is set, when it stays in front of the eyes.
    static std::atomic<float> wr64PanelDistance{ 66.0f };
    static std::atomic<float> wr64PanelWidth{ 53.0f };
    static std::atomic<float> wr64PanelSeparation{ 2.0f };
    static std::atomic<bool> wr64PanelFollowsHead{ false };

    extern "C" void RT64_SetXrPanel(float distance, float width, float separation, int followHead) {
        wr64PanelDistance.store(distance, std::memory_order_relaxed);
        wr64PanelWidth.store(width, std::memory_order_relaxed);
        wr64PanelSeparation.store(separation, std::memory_order_relaxed);
        wr64PanelFollowsHead.store(followHead != 0, std::memory_order_relaxed);
    }

    // wr64: where the panel hangs in the seated space when it does not follow
    // the head -- its middle, and its right and up directions -- set by the
    // port each headset frame, which lets it follow the head softly there;
    // straight ahead of the seated position until it is set.
    static std::atomic<float> wr64PanelPose[9];
    static std::atomic<bool> wr64PanelPosed{ false };

    extern "C" void RT64_SetXrPanelPose(const float *centre, const float *right, const float *up) {
        for (int i = 0; i < 3; i++) {
            wr64PanelPose[i].store(centre[i], std::memory_order_relaxed);
            wr64PanelPose[3 + i].store(right[i], std::memory_order_relaxed);
            wr64PanelPose[6 + i].store(up[i], std::memory_order_relaxed);
        }

        wr64PanelPosed.store(true, std::memory_order_relaxed);
    }

    // wr64: the HUD on a layer of its own, set by the port: the eyes are drawn
    // without the frame's flat elements, and the workload queue draws those
    // alone twice more -- over black (wr64PanelPass 1) and over white (2), the
    // scene seen through the lens left out -- for the present queue to make into
    // one see-through image the headset holds in front of the eyes itself.
    std::atomic<bool> wr64PanelLayer{ false };
    int wr64PanelPass = 0;

    extern "C" void RT64_SetXrPanelLayer(int on) {
        wr64PanelLayer.store(on != 0, std::memory_order_relaxed);
    }

    // wr64: the head's transform for an eye, from the seated space into the
    // eye's, as the frame being drawn took it (below, with the head).
    bool wr64HeadForEye(int eye, float out[16]);

    // wr64: where framed 3D -- a model a menu turns in a box of its own, like
    // the course card's track -- goes on the panel, set by the port: scaled
    // about the frame's middle, then moved, in the frame's own -1 to 1. Left
    // where the game put it, it stays in its box, which the box's own
    // viewport clips it to; the port can move it aside instead.
    static std::atomic<float> wr64FramedScale{ 1.0f };
    static std::atomic<float> wr64FramedX{ 0.0f };
    static std::atomic<float> wr64FramedY{ 0.0f };

    extern "C" void RT64_SetXrFramed(float scale, float x, float y) {
        wr64FramedScale.store(scale, std::memory_order_relaxed);
        wr64FramedX.store(x, std::memory_order_relaxed);
        wr64FramedY.store(y, std::memory_order_relaxed);
    }

    bool wr64PanelTransform(float out[4]);

    // wr64: for framed 3D, what its frame's -1 to 1 becomes in the eye's
    // picture -- its placement, then the panel -- as wr64PanelTransform
    // gives it; false for anything else. The framebuffer renderer moves the
    // viewport's clip with it.
    bool wr64FramedTransform(const void *drawData, uint32_t transform, float out[4]) {
        float panel[4];
        if (!wr64PanelTransform(panel)) {
            return false;
        }

        for (const Wr64FlatTransform &flat : wr64FlatTransforms) {
            if (flat.framed && (flat.drawData == drawData) && (flat.transform == transform)) {
                const float scale = wr64FramedScale.load(std::memory_order_relaxed);
                out[0] = panel[0] * scale;
                out[1] = panel[0] * wr64FramedX.load(std::memory_order_relaxed) + panel[1];
                out[2] = panel[2] * scale;
                out[3] = panel[2] * wr64FramedY.load(std::memory_order_relaxed) + panel[3];
                return true;
            }
        }

        return false;
    }

    // wr64: where the panel lies in the picture of the eye being drawn: a flat
    // element at x and y in the frame (-1 to 1, up positive) belongs at
    // x * out[0] + out[1] and y * out[2] + out[3] there. Each eye sees the
    // panel's points in the directions they lie from it, through its own
    // lens, so the two agree on where the panel is. Hung in the seated space,
    // the panel is found from each eye's head transform: its middle in the
    // eye's space gives where, and how its edges move from there gives how
    // big -- the panel's picture straightened, as a scale and an offset are
    // all a screen rectangle can take, which keeps it level and square-on
    // whichever way the head turns or tilts. Once it is behind the eye, or
    // beside it, it is moved out of the picture. False when there is no
    // panel: no headset lens, or a frame that put no scene through it, which
    // the port shows on its flat screen.
    bool wr64PanelTransform(float out[4]) {
        if (!wr64XrLensActive.load(std::memory_order_relaxed) || !wr64LensUsed) {
            return false;
        }

        // On a layer of its own, the HUD is drawn where the frame has it in its
        // own passes, and out of the picture in the eyes'.
        if (wr64PanelLayer.load(std::memory_order_relaxed)) {
            out[0] = (wr64PanelPass != 0) ? 1.0f : 0.0f;
            out[1] = (wr64PanelPass != 0) ? 0.0f : 4.0f;
            out[2] = out[0];
            out[3] = out[1];
            return true;
        }

        const float tanLeft = wr64XrLensTangents[wr64EyeIndex][0].load(std::memory_order_relaxed);
        const float tanRight = wr64XrLensTangents[wr64EyeIndex][1].load(std::memory_order_relaxed);
        const float tanUp = wr64XrLensTangents[wr64EyeIndex][2].load(std::memory_order_relaxed);
        const float tanDown = wr64XrLensTangents[wr64EyeIndex][3].load(std::memory_order_relaxed);
        const float distance = wr64PanelDistance.load(std::memory_order_relaxed);
        const float width = wr64PanelWidth.load(std::memory_order_relaxed);
        if ((tanRight <= tanLeft) || (tanUp <= tanDown) || (distance <= 0.0f) || (width <= 0.0f)) {
            return false;
        }

        const float height = width * 0.75f;
        float head[16];
        if (!wr64PanelFollowsHead.load(std::memory_order_relaxed) && wr64HeadForEye(wr64EyeIndex, head)) {
            // Row vectors: a point p of the seated space is p * head in the
            // eye's. The panel's middle, and its half-width and half-height
            // as directions -- where the port hangs it, or straight ahead.
            float pc[3] = { 0.0f, 0.0f, -distance }, pr[3] = { 1.0f, 0.0f, 0.0f }, pu[3] = { 0.0f, 1.0f, 0.0f };
            if (wr64PanelPosed.load(std::memory_order_relaxed)) {
                for (int i = 0; i < 3; i++) {
                    pc[i] = wr64PanelPose[i].load(std::memory_order_relaxed);
                    pr[i] = wr64PanelPose[3 + i].load(std::memory_order_relaxed);
                    pu[i] = wr64PanelPose[6 + i].load(std::memory_order_relaxed);
                }
            }

            float c[3], u[3], v[3];
            for (int j = 0; j < 3; j++) {
                c[j] = pc[0] * head[j] + pc[1] * head[4 + j] + pc[2] * head[8 + j] + head[12 + j];
                u[j] = 0.5f * width * (pr[0] * head[j] + pr[1] * head[4 + j] + pr[2] * head[8 + j]);
                v[j] = 0.5f * height * (pu[0] * head[j] + pu[1] * head[4 + j] + pu[2] * head[8 + j]);
            }
            const float depth = -c[2];
            if (depth < 0.1f * distance) {
                out[0] = 0.0f; out[1] = 4.0f; out[2] = 0.0f; out[3] = 4.0f;
                return true;
            }

            // The tangents of the middle, and how fast they change towards
            // the right and top edges.
            const float tx = c[0] / depth, ty = c[1] / depth;
            const float dtx = (u[0] * depth + c[0] * u[2]) / (depth * depth);
            const float dty = (v[1] * depth + c[1] * v[2]) / (depth * depth);
            out[0] = 2.0f * dtx / (tanRight - tanLeft);
            out[1] = (2.0f * tx - (tanRight + tanLeft)) / (tanRight - tanLeft);
            out[2] = 2.0f * dty / (tanUp - tanDown);
            out[3] = (2.0f * ty - (tanUp + tanDown)) / (tanUp - tanDown);
            return true;
        }

        // Following the head: straight ahead of the eyes, each eye half the
        // separation to its side.
        const float eyeX = ((wr64EyeIndex == 0) ? -0.5f : 0.5f) * wr64PanelSeparation.load(std::memory_order_relaxed);
        out[0] = (width / distance) / (tanRight - tanLeft);
        out[1] = (-2.0f * eyeX / distance - (tanRight + tanLeft)) / (tanRight - tanLeft);
        out[2] = (height / distance) / (tanUp - tanDown);
        out[3] = -(tanUp + tanDown) / (tanUp - tanDown);
        return true;
    }

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

# --- the head ---------------------------------------------------------------

HEAD_ANCHOR = """    // ProjectionProcessor

    ProjectionProcessor::ProjectionProcessor() { }
"""

HEAD_REPLACEMENT = """    // wr64: the head, in a headset: for each eye a transform from the seated
    // space into that eye's own, in the game's units, and a tag naming the
    // pose it came from. The port publishes the latest each headset frame.
    // The workload queue takes a snapshot of it before rendering each frame,
    // so both eyes of a frame see the same moment, and reports the snapshot's
    // tag with the frame, so the port can hand the headset the pose the frame
    // was rendered with.
    static std::mutex wr64HeadMutex;
    static bool wr64HeadPublishedActive = false;
    static float wr64HeadPublished[2][16];
    static uint64_t wr64HeadPublishedTag = 0;

    // The snapshot, on the render thread.
    static bool wr64HeadActive = false;
    static float wr64HeadEye[2][16];

    extern "C" void RT64_SetXrHead(int active, const float *eyes, uint64_t tag) {
        std::scoped_lock lock(wr64HeadMutex);
        wr64HeadPublishedActive = (active != 0) && (eyes != nullptr);
        if (wr64HeadPublishedActive) {
            memcpy(wr64HeadPublished, eyes, sizeof(wr64HeadPublished));
        }

        wr64HeadPublishedTag = wr64HeadPublishedActive ? tag : 0;
    }

    uint64_t wr64SnapshotHead() {
        std::scoped_lock lock(wr64HeadMutex);
        wr64HeadActive = wr64HeadPublishedActive;
        memcpy(wr64HeadEye, wr64HeadPublished, sizeof(wr64HeadEye));
        return wr64HeadPublishedTag;
    }

    bool wr64HeadForEye(int eye, float out[16]) {
        if (!wr64HeadActive || (eye < 0) || (eye > 1)) {
            return false;
        }

        memcpy(out, wr64HeadEye[eye], sizeof(wr64HeadEye[eye]));
        return true;
    }

    // ProjectionProcessor

    ProjectionProcessor::ProjectionProcessor() { }
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

        // wr64: the HUD's own passes of each displayed frame, over black and over
        // white, in the same order (rt64_projection_processor.cpp).
        std::vector<std::unique_ptr<RenderTarget>> wr64PanelTargets[2];
"""

HEAD_TAGS_ANCHOR = """        InterpolatedFrameCounters interpolatedFrames[2];
"""

HEAD_TAGS_REPLACEMENT = """        // wr64: the head pose each displayed frame was rendered with, as the
        // port's tag for it, in the same order as wr64StereoTargets. Fixed in
        // size, so the present queue can read it while the workload queue
        // writes, with no reallocation between them.
        std::array<uint64_t, 16> wr64HeadTags = {};

        // wr64: whether each displayed frame is for a headset's flat screen --
        // drawn without the headset's lens -- in the same order.
        std::array<bool, 16> wr64ScreenFrames = {};

        // wr64: whether each displayed frame's HUD was drawn on its own, in the
        // same order.
        std::array<bool, 16> wr64PanelFrames = {};

        InterpolatedFrameCounters interpolatedFrames[2];
"""

HEAD_EXTERN_ANCHOR = """    // WorkloadQueue

    WorkloadQueue::WorkloadQueue() {
"""

HEAD_EXTERN_REPLACEMENT = """    // wr64: see rt64_projection_processor.cpp.
    uint64_t wr64SnapshotHead();

    // WorkloadQueue

    WorkloadQueue::WorkloadQueue() {
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
    extern bool wr64LensUsed;
    extern std::atomic<bool> wr64PanelLayer;
    extern int wr64PanelPass;
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
            ext.sharedResources->wr64PanelTargets[0].clear();
            ext.sharedResources->wr64PanelTargets[1].clear();
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

                // wr64: and the HUD on its own, when the port asks for it.
                const bool wr64Panel = wr64Stereo && wr64PanelLayer.load(std::memory_order_relaxed);
                if (wr64Panel) {
                    const bool panelHDR = ext.sharedResources->renderTargetManager.usesHDR;
                    for (auto &panelTargets : ext.sharedResources->wr64PanelTargets) {
                        while (panelTargets.size() < displayFrames) {
                            panelTargets.emplace_back(std::make_unique<RenderTarget>(interpolationTargetKey.address, Framebuffer::Type::Color, RenderMultisampling(), panelHDR));
                        }
                    }
                }
"""

WORKLOAD_RENDER_ANCHOR = """                    threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                        interpolationTargetKey, interpolationTargetFbPairIndex, overrideTarget, overrideModifier, velocityUploaderUsed, uploadExtras, tileInterpolationUsed, lookAtInterpolationUsed);
"""

WORKLOAD_RENDER_REPLACEMENT = """                    // wr64: a headset's head, as it is now, for both eyes of this
                    // frame; its tag goes with the frame to the present queue,
                    // indexed like the right eye's target.
                    const uint64_t wr64HeadTag = wr64SnapshotHead();
                    if (framesRendered < ext.sharedResources->wr64HeadTags.size()) {
                        ext.sharedResources->wr64HeadTags[framesRendered] = wr64HeadTag;
                    }

                    // wr64: in stereo this is the left eye, moved left by half the
                    // separation.
                    wr64EyeIndex = 0;
                    wr64EyeOffsetX = wr64Stereo ? (-0.5f * wr64Separation) : 0.0f;
                    wr64LensUsed = false;
                    threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                        interpolationTargetKey, interpolationTargetFbPairIndex, overrideTarget, overrideModifier, velocityUploaderUsed, uploadExtras, tileInterpolationUsed, lookAtInterpolationUsed);

                    // wr64: a frame no scene of which went through a headset's
                    // lens -- a menu with nothing 3D behind it -- is shown on its
                    // flat screen instead of to its eyes.
                    if (framesRendered < ext.sharedResources->wr64ScreenFrames.size()) {
                        ext.sharedResources->wr64ScreenFrames[framesRendered] = !wr64LensUsed;
                    }

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

                    // wr64: and the HUD on its own, twice -- over black, then over
                    // white -- once the eyes have shown the frame put a scene
                    // through the lens, into targets of its own.
                    bool wr64PanelDrawn = false;
                    auto &wr64PanelTargets = ext.sharedResources->wr64PanelTargets;
                    if (wr64Panel && wr64LensUsed && (framesRendered < wr64PanelTargets[0].size()) && (framesRendered < wr64PanelTargets[1].size())) {
                        for (int pass = 0; pass < 2; pass++) {
                            wr64EyeIndex = 0;
                            wr64EyeOffsetX = 0.0f;
                            wr64PanelPass = pass + 1;
                            threadRenderFrame(curFrame, prevFrame, workloadConfig, workload.debuggerRenderer, workload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                                interpolationTargetKey, interpolationTargetFbPairIndex, wr64PanelTargets[pass][framesRendered].get(), wr64StereoModifierBase + 0x100 * (pass + 1) + framesRendered,
                                velocityUploaderUsed, false, tileInterpolationUsed, lookAtInterpolationUsed);
                        }

                        wr64PanelPass = 0;
                        wr64PanelDrawn = true;
                    }

                    if (framesRendered < ext.sharedResources->wr64PanelFrames.size()) {
                        ext.sharedResources->wr64PanelFrames[framesRendered] = wr64PanelDrawn;
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
    // want drawn, and 1 for a frame whose images are to be drawn; every
    // frame begun is handed back through end -- with 0 if nothing was drawn,
    // 1 if the eyes were and 2 if the flat screen was -- and the tag of
    // the head pose the frame was rendered with (see RT64_SetXrHead in
    // rt64_projection_processor.cpp). A frame no scene of which went through
    // the headset's lens -- a menu with nothing 3D behind it -- is drawn as it
    // is onto the screen's image, for the headset to hang in front of the
    // player. writeRaw says the
    // images take the VI's output as it is; otherwise they encode to sRGB as
    // they are written, and are given it linearised.
    struct RT64_XrEye {
        void *resource;
        uint32_t width;
        uint32_t height;
    };

    struct RT64_XrFrame {
        RT64_XrEye eyes[2];
        RT64_XrEye screen;
        int writeRaw;
    };

    typedef int (*RT64_XrBeginFrame)(void *user, RT64_XrFrame *frame);
    typedef void (*RT64_XrEndFrame)(void *user, int rendered, uint64_t headTag);

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
            int wr64XrRendered = 0;

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
                    const auto &wr64ScreenFrames = ext.sharedResources->wr64ScreenFrames;
                    const bool wr64Screen = (wr64XrFrame.screen.resource != nullptr) && (uint32_t(i) < wr64ScreenFrames.size()) && wr64ScreenFrames[i];
                    for (uint32_t eye = (wr64Screen ? 2 : 0); eye < (wr64Screen ? 3 : 2); eye++) {
                        const RT64_XrEye &xrEye = (eye < 2) ? wr64XrFrame.eyes[eye] : wr64XrFrame.screen;
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
                            // The screen's image is also written by the HUD's combination.
                            xrTexture->desc.flags = (eye == 2) ? (RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::UNORDERED_ACCESS) : RenderTextureFlag::RENDER_TARGET;
                            const RenderTexture *xrAttachment = xrTexture.get();
                            xrTarget.framebuffer = ext.device->createFramebuffer(RenderFramebufferDesc(&xrAttachment, 1));
                            xrTarget.texture = std::move(xrTexture);
                        }

                        D3D12Texture *xrTexture = static_cast<D3D12Texture *>(xrTarget.texture.get());
                        xrTexture->resourceStates = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        xrTexture->layout = RenderTextureLayout::COLOR_WRITE;
                        commandList->setFramebuffer(xrTarget.framebuffer.get());
                        commandList->clearColor();
                        // The screen shows the frame as the left eye was drawn.
                        const VIRenderer::RenderParams &xrSource = wr64EyeParams[(eye < 2) ? eye : 0];
                        if (xrSource.texture != nullptr) {
                            VIRenderer::RenderParams xrParams = xrSource;
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

                    // wr64: the HUD, drawn on its own over black and over white by
                    // the workload queue, made into one see-through image on the
                    // screen's, for the headset to hold in front of the eyes. Each
                    // pass is first drawn at the screen's size the way the eyes
                    // are; what differs between the two is how see-through it is.
                    const auto &wr64PanelFrames = ext.sharedResources->wr64PanelFrames;
                    const RT64_XrEye &xrScreen = wr64XrFrame.screen;
                    bool wr64Panel = !wr64Screen && (xrScreen.resource != nullptr) && wr64XrFrame.writeRaw &&
                        (uint32_t(i) < wr64PanelFrames.size()) && wr64PanelFrames[i] && (wr64EyeParams[0].texture != nullptr);
                    Wr64XrTarget *screenTarget = nullptr;
                    if (wr64Panel) {
                        auto found = wr64XrTargets.find(xrScreen.resource);
                        screenTarget = (found != wr64XrTargets.end()) ? &found->second : nullptr;
                        if (screenTarget == nullptr) {
                            Wr64XrTarget &made = wr64XrTargets[xrScreen.resource];
                            auto screenTexture = std::make_unique<D3D12Texture>();
                            screenTexture->d3d = static_cast<ID3D12Resource *>(xrScreen.resource);
                            screenTexture->device = static_cast<D3D12Device *>(ext.device);
                            screenTexture->desc.dimension = RenderTextureDimension::TEXTURE_2D;
                            screenTexture->desc.width = xrScreen.width;
                            screenTexture->desc.height = xrScreen.height;
                            screenTexture->desc.depth = 1;
                            screenTexture->desc.mipLevels = 1;
                            screenTexture->desc.arraySize = 1;
                            screenTexture->desc.format = RenderFormat::R8G8B8A8_UNORM;
                            screenTexture->desc.flags = RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::UNORDERED_ACCESS;
                            const RenderTexture *screenAttachment = screenTexture.get();
                            made.framebuffer = ext.device->createFramebuffer(RenderFramebufferDesc(&screenAttachment, 1));
                            made.texture = std::move(screenTexture);
                            screenTarget = &made;
                        }
                    }

                    if (wr64Panel && ((wr64PanelImageWidth != xrScreen.width) || (wr64PanelImageHeight != xrScreen.height))) {
                        for (int pass = 0; pass < 2; pass++) {
                            wr64PanelFramebuffers[pass].reset();
                            wr64PanelImages[pass] = ext.device->createTexture(RenderTextureDesc::ColorTarget(xrScreen.width, xrScreen.height, RenderFormat::B8G8R8A8_UNORM));
                            const RenderTexture *panelAttachment = wr64PanelImages[pass].get();
                            wr64PanelFramebuffers[pass] = ext.device->createFramebuffer(RenderFramebufferDesc(&panelAttachment, 1));
                        }

                        wr64PanelCombined = ext.device->createTexture(RenderTextureDesc::Texture2D(xrScreen.width, xrScreen.height, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));

                        wr64PanelImageWidth = xrScreen.width;
                        wr64PanelImageHeight = xrScreen.height;
                    }

                    for (int pass = 0; wr64Panel && (pass < 2); pass++) {
                        auto &panelTargets = ext.sharedResources->wr64PanelTargets[pass];
                        RenderTarget *panelTarget = (uint32_t(i) < panelTargets.size()) ? panelTargets[i].get() : nullptr;
                        if ((panelTarget == nullptr) || (panelTarget->width == 0)) {
                            wr64Panel = false;
                            break;
                        }

                        VIRenderer::RenderParams panelParams = wr64EyeParams[0];
                        panelParams.textureFormat = panelTarget->format;
                        panelParams.resolutionScale = panelTarget->resolutionScale;
                        panelParams.downsamplingScale = 1;
                        if (panelTarget->downsampleMultiplier > 1) {
                            panelTarget->downsampleTarget(ext.presentGraphicsWorker, ext.shaderLibrary);
                            panelParams.texture = panelTarget->downsampledTexture.get();
                            panelParams.textureWidth = panelTarget->width / panelTarget->downsampleMultiplier;
                            panelParams.textureHeight = panelTarget->height / panelTarget->downsampleMultiplier;
                            panelParams.downsamplingScale = panelTarget->downsampleMultiplier;
                        }
                        else {
                            panelTarget->resolveTarget(ext.presentGraphicsWorker, ext.shaderLibrary);
                            panelParams.texture = panelTarget->getResolvedTexture();
                            panelParams.textureWidth = panelTarget->width;
                            panelParams.textureHeight = panelTarget->height;
                        }

                        panelParams.regionX = 0.0f;
                        panelParams.regionWidth = float(xrScreen.width);
                        panelParams.regionHeight = float(xrScreen.height);
                        panelParams.gammaScale = 1.0f;
                        panelParams.fillRegion = true;
                        commandList->barriers(RenderBarrierStage::GRAPHICS, {
                            RenderTextureBarrier(panelParams.texture, RenderTextureLayout::SHADER_READ),
                            RenderTextureBarrier(wr64PanelImages[pass].get(), RenderTextureLayout::COLOR_WRITE)
                        });
                        commandList->setFramebuffer(wr64PanelFramebuffers[pass].get());
                        commandList->clearColor();
                        if (wr64PanelViRenderers[pass] == nullptr) {
                            wr64PanelViRenderers[pass] = std::make_unique<VIRenderer>();
                        }

                        wr64PanelViRenderers[pass]->render(panelParams);
                    }

                    if (wr64Panel && (screenTarget != nullptr)) {
                        D3D12Texture *screenTexture = static_cast<D3D12Texture *>(screenTarget->texture.get());
                        screenTexture->resourceStates = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        screenTexture->layout = RenderTextureLayout::COLOR_WRITE;
                        commandList->setFramebuffer(nullptr);
                        commandList->barriers(RenderBarrierStage::COMPUTE, {
                            RenderTextureBarrier(wr64PanelImages[0].get(), RenderTextureLayout::SHADER_READ),
                            RenderTextureBarrier(wr64PanelImages[1].get(), RenderTextureLayout::SHADER_READ),
                            RenderTextureBarrier(wr64PanelCombined.get(), RenderTextureLayout::GENERAL)
                        });
                        if (wr64PanelCombineSet == nullptr) {
                            wr64PanelCombineSet = std::make_unique<Wr64PanelCombineDescriptorSet>(ext.device);
                        }

                        wr64PanelCombineSet->setTexture(wr64PanelCombineSet->gBlack, wr64PanelImages[0].get(), RenderTextureLayout::SHADER_READ);
                        wr64PanelCombineSet->setTexture(wr64PanelCombineSet->gWhite, wr64PanelImages[1].get(), RenderTextureLayout::SHADER_READ);
                        wr64PanelCombineSet->setTexture(wr64PanelCombineSet->gOutput, wr64PanelCombined.get(), RenderTextureLayout::GENERAL);
                        const uint32_t combineSize[2] = { xrScreen.width, xrScreen.height };
                        commandList->setPipeline(ext.shaderLibrary->wr64PanelCombine.pipeline.get());
                        commandList->setComputePipelineLayout(ext.shaderLibrary->wr64PanelCombine.pipelineLayout.get());
                        commandList->setComputePushConstants(0, combineSize);
                        commandList->setComputeDescriptorSet(wr64PanelCombineSet->get(), 0);
                        commandList->dispatch((xrScreen.width + 7) / 8, (xrScreen.height + 7) / 8, 1);
                        // The headset's image takes the result by a copy: written by a
                        // compute shader directly, it lost the device.
                        commandList->barriers(RenderBarrierStage::COPY, {
                            RenderTextureBarrier(wr64PanelCombined.get(), RenderTextureLayout::COPY_SOURCE),
                            RenderTextureBarrier(screenTexture, RenderTextureLayout::COPY_DEST)
                        });
                        commandList->copyTexture(screenTexture, wr64PanelCombined.get());
                        commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(screenTexture, RenderTextureLayout::COLOR_WRITE));
                    }

                    commandList->setFramebuffer(swapChainFramebuffer);
                    wr64XrRendered = wr64Screen ? 2 : (wr64Panel ? 3 : 1);
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
                const auto &wr64HeadTags = ext.sharedResources->wr64HeadTags;
                const uint64_t wr64HeadTag = (uint32_t(i) < wr64HeadTags.size()) ? wr64HeadTags[i] : 0;
                wr64Xr.end(wr64Xr.user, wr64XrRendered, wr64HeadTag);
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
        // runtime's resource, and a VI renderer for each eye and the screen.
        struct Wr64XrTarget {
            std::unique_ptr<RenderTexture> texture;
            std::unique_ptr<RenderFramebuffer> framebuffer;
        };
        std::unordered_map<void *, Wr64XrTarget> wr64XrTargets;
        std::unique_ptr<VIRenderer> wr64XrViRenderers[3];

        // wr64: the HUD's two passes at the screen's size, each with its own VI
        // renderer to draw it there, and what combines them.
        std::unique_ptr<RenderTexture> wr64PanelImages[2];
        std::unique_ptr<RenderFramebuffer> wr64PanelFramebuffers[2];
        std::unique_ptr<VIRenderer> wr64PanelViRenderers[2];
        std::unique_ptr<RenderTexture> wr64PanelCombined;
        uint32_t wr64PanelImageWidth = 0;
        uint32_t wr64PanelImageHeight = 0;
        std::unique_ptr<Wr64PanelCombineDescriptorSet> wr64PanelCombineSet;
"""

XR_HEADER_INCLUDE_ANCHOR = """#include "rt64_shared_queue_resources.h"
"""

XR_HEADER_INCLUDE_REPLACEMENT = """#include "rt64_shared_queue_resources.h"

#include <unordered_map> // wr64: the headset's wrapped eye images.

#include "render/rt64_descriptor_sets.h" // wr64: the HUD's passes combined.
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



# --- a frame's flat elements onto a panel in a headset's eyes ----------------

PANEL_PROCESS_ANCHOR = """        for (size_t s = 0; s < p.curFrame->perspectiveScenes.size(); s++) {
            processScene(p, p.curFrame->perspectiveScenes[s], s);
        }

        for (size_t s = 0; s < p.curFrame->orthographicScenes.size(); s++) {
            processScene(p, p.curFrame->orthographicScenes[s], s);
        }
    }
"""

PANEL_PROCESS_REPLACEMENT = """        wr64FlatTransforms.clear();

        for (size_t s = 0; s < p.curFrame->perspectiveScenes.size(); s++) {
            processScene(p, p.curFrame->perspectiveScenes[s], s);
        }

        for (size_t s = 0; s < p.curFrame->orthographicScenes.size(); s++) {
            processScene(p, p.curFrame->orthographicScenes[s], s);
        }

        // wr64: in a frame that put a scene through a headset's lens, what it
        // drew flat goes onto the panel (wr64PanelTransform): after its own
        // projection, a scale and offset of the picture, w kept so a menu's
        // 3D keeps its depth among itself. Framed 3D is placed
        // (RT64_SetXrFramed) and its depth squeezed into the nearest
        // hundredth, in front of the scene behind the panel: left where it
        // was it is tested against that scene's depth, from another camera
        // entirely, and flickers in and out of it. It is drawn through a
        // viewport of its own -- its box -- which is applied after this, so
        // it is moved within that viewport's -1 to 1 to where the box lands:
        // with the box's middle at c and half-size h in the frame's -1 to 1,
        // a point at x in the viewport is at c + h x in the frame, belongs at
        // f = panel(c + s h x + o) -- scaled by s about the box's middle and
        // moved by o -- and so at (f - c) / h in the viewport. The frame is
        // taken to be the game's 4:3.
        float wr64Panel[4];
        if (wr64PanelTransform(wr64Panel)) {
            const hlslpp::float4x4 onto(
                wr64Panel[0], 0.0f, 0.0f, 0.0f,
                0.0f, wr64Panel[2], 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                wr64Panel[1], wr64Panel[3], 0.0f, 1.0f);
            const float framedDepth = 0.01f;
            for (const Wr64FlatTransform &flat : wr64FlatTransforms) {
                const uint32_t w = flat.workload, t = flat.transform;
                DrawData &drawData = p.workloadQueue->workloads[w].drawData;
                hlslpp::float4x4 placed = onto;
                float framed[4];
                if (flat.framed && (flat.frameWidth > 0.0f) && wr64FramedTransform(&drawData, t, framed)) {
                    const interop::RSPViewport &viewport = drawData.rspViewports[t];
                    const float halfWidth = 0.5f * flat.frameWidth, halfHeight = 0.375f * flat.frameWidth;
                    const float cx = (viewport.translate.x - halfWidth) / halfWidth, hx = viewport.scale.x / halfWidth;
                    const float cy = (halfHeight - viewport.translate.y) / halfHeight, hy = viewport.scale.y / halfHeight;
                    if ((hx != 0.0f) && (hy != 0.0f)) {
                        placed = hlslpp::float4x4(
                            framed[0], 0.0f, 0.0f, 0.0f,
                            0.0f, framed[2], 0.0f, 0.0f,
                            0.0f, 0.0f, framedDepth, 0.0f,
                            (wr64Panel[0] * cx + framed[1] - cx) / hx, (wr64Panel[2] * cy + framed[3] - cy) / hy, framedDepth - 1.0f, 1.0f);
                    }
                }

                drawData.modProjTransforms[t] = hlslpp::mul(hlslpp::float4x4(drawData.modProjTransforms[t]), placed);
                drawData.modViewProjTransforms[t] = hlslpp::mul(hlslpp::float4x4(drawData.modViewProjTransforms[t]), placed);
                drawData.prevProjTransforms[t] = hlslpp::mul(hlslpp::float4x4(drawData.prevProjTransforms[t]), placed);
                drawData.prevViewProjTransforms[t] = hlslpp::mul(hlslpp::float4x4(drawData.prevViewProjTransforms[t]), placed);
            }
        }
    }
"""

PANEL_RECT_ANCHOR = """                            triangles.screenOffset.y = halfPixelOffset.y + (halfViewportSize.y - (viewportRect.y + viewportRect.height / 2.0f)) / halfViewportSize.y;
"""

PANEL_RECT_REPLACEMENT = """                            triangles.screenOffset.y = halfPixelOffset.y + (halfViewportSize.y - (viewportRect.y + viewportRect.height / 2.0f)) / halfViewportSize.y;

                            // wr64: onto the panel in a headset's eye, with the
                            // frame's other flat elements (rt64_projection_processor.cpp).
                            bool wr64PanelTransform(float out[4]);
                            float wr64Panel[4];
                            if (wr64PanelTransform(wr64Panel)) {
                                triangles.screenScale.x *= wr64Panel[0];
                                triangles.screenScale.y *= wr64Panel[2];
                                triangles.screenOffset.x = triangles.screenOffset.x * wr64Panel[0] + wr64Panel[1];
                                triangles.screenOffset.y = triangles.screenOffset.y * wr64Panel[2] + wr64Panel[3];
                            }
"""

PANEL_SCISSOR_ANCHOR = """                        triangles.scissor = convertFixedRect(call.callDesc.scissorRect, p.resolutionScale, p.fbWidth, invRatioScale, extOriginPercentage, int32_t(horizontalMisalignment), call.callDesc.scissorLeftOrigin, call.callDesc.scissorRightOrigin);
"""

PANEL_SCISSOR_REPLACEMENT = """                        triangles.scissor = convertFixedRect(call.callDesc.scissorRect, p.resolutionScale, p.fbWidth, invRatioScale, extOriginPercentage, int32_t(horizontalMisalignment), call.callDesc.scissorLeftOrigin, call.callDesc.scissorRightOrigin);

                        // wr64: a flat element's scissor goes onto the panel with it, and so
                        // does framed 3D's, which is what keeps it in its box -- the box
                        // where the panel has it, while the model may be moved within it
                        // (RT64_SetXrFramed).
                        {
                            bool wr64PanelTransform(float out[4]);
                            bool wr64FramedTransform(const void *drawData, uint32_t transform, float out[4]);
                            float wr64Panel[4];
                            const bool wr64Flat = (proj.type == Projection::Type::Rectangle) || (proj.type == Projection::Type::Orthographic);
                            if ((wr64Flat && wr64PanelTransform(wr64Panel)) ||
                                ((proj.type == Projection::Type::Perspective) && wr64FramedTransform(&drawData, proj.transformsIndex, wr64Panel) &&
                                 wr64PanelTransform(wr64Panel))) {
                                auto across = [&](float px) { return ((px - halfViewportSize.x) / halfViewportSize.x * wr64Panel[0] + wr64Panel[1]) * halfViewportSize.x + halfViewportSize.x; };
                                auto down = [&](float py) { return halfViewportSize.y - ((halfViewportSize.y - py) / halfViewportSize.y * wr64Panel[2] + wr64Panel[3]) * halfViewportSize.y; };
                                triangles.scissor.left = int32_t(std::floor(across(float(triangles.scissor.left))));
                                triangles.scissor.right = int32_t(std::ceil(across(float(triangles.scissor.right))));
                                triangles.scissor.top = int32_t(std::floor(down(float(triangles.scissor.top))));
                                triangles.scissor.bottom = int32_t(std::ceil(down(float(triangles.scissor.bottom))));
                            }
                        }
"""


PANEL_FRAMED_CLIP_ANCHOR = """                viewportClip = convertViewportRect(viewport.rect(viewportClipRatios), p.resolutionScale, p.fbWidth, projInvRatioScale, extOriginPercentage, 0.0f, viewportOrigin, viewportOrigin);
"""

PANEL_FRAMED_CLIP_REPLACEMENT = """                viewportClip = convertViewportRect(viewport.rect(viewportClipRatios), p.resolutionScale, p.fbWidth, projInvRatioScale, extOriginPercentage, 0.0f, viewportOrigin, viewportOrigin);

                // wr64: framed 3D in a headset's eye is clipped by its scissor alone,
                // moved onto the panel with it below -- the box the game draws it
                // in. Its viewport is the frame's size, only off-centre, and moved
                // with it cut the box short (rt64_projection_processor.cpp).
                bool wr64FramedTransform(const void *drawData, uint32_t transform, float out[4]);
                float wr64Framed[4];
                if (wr64FramedTransform(&drawData, proj.transformsIndex, wr64Framed)) {
                    viewportClip = framebuffer.viewport;
                }
"""


# --- the HUD on a layer of its own (RT64_SetXrPanelLayer) -------------------
#
# In the HUD's own passes (wr64PanelPass, rt64_projection_processor.cpp) the
# frame's clear is black in the first and white in the second; the present
# queue combines the two with this shader.

DESCRIPTOR_SETS = RT64 / "render" / "rt64_descriptor_sets.h"
SHADER_LIBRARY_HEADER = RT64 / "render" / "rt64_shader_library.h"
SHADER_LIBRARY = RT64 / "render" / "rt64_shader_library.cpp"
RT64_CMAKE = REPO / "lib" / "RT64" / "CMakeLists.txt"
PANEL_SHADER = RT64 / "shaders" / "Wr64PanelCombineCS.hlsl"

PANEL_CLEAR_ANCHOR = """                    clearRect.rect = convertFixedRect(call.callDesc.rect, p.resolutionScale, p.fbWidth, invRatioScale, extOriginPercentage, horizontalMisalignment, call.callDesc.rectLeftOrigin, call.callDesc.rectRightOrigin);
"""

PANEL_CLEAR_REPLACEMENT = """                    clearRect.rect = convertFixedRect(call.callDesc.rect, p.resolutionScale, p.fbWidth, invRatioScale, extOriginPercentage, horizontalMisalignment, call.callDesc.rectLeftOrigin, call.callDesc.rectRightOrigin);

                    // wr64: in the HUD's own passes (rt64_projection_processor.cpp),
                    // the frame's clear -- a fill most of the frame wide and tall --
                    // is black in the first and white in the second, which is what
                    // the two are told apart by. Any other fill of the picture is a
                    // flat element, and goes onto the panel with the rest of them
                    // (wr64PanelTransform) -- out of the eyes when the panel is a
                    // layer of its own, which leaves it empty. The frame's depth
                    // clear, through a headset's lens, clears the whole eye: the
                    // game clears only inside its border, and the wider eye sees
                    // past it, where old depth hid the water in bands at the top
                    // and bottom.
                    {
                        extern int wr64PanelPass;
                        extern bool wr64LensUsed;
                        bool wr64PanelTransform(float out[4]);
                        const int32_t clearWidth = call.callDesc.rect.lrx - call.callDesc.rect.ulx;
                        const int32_t clearHeight = call.callDesc.rect.lry - call.callDesc.rect.uly;
                        const bool frameClear = (clearWidth >= int32_t(p.fbWidth) * 3) && (clearHeight * 5 >= int32_t(p.fbHeight) * 12);
                        float wr64Panel[4];
                        if ((wr64PanelPass != 0) && (p.fbStorage->colorTarget != nullptr) && frameClear) {
                            const float level = (wr64PanelPass == 2) ? 1.0f : 0.0f;
                            clearRect.color = RenderColor(level, level, level, 1.0f);
                        }
                        else if ((p.fbStorage->colorTarget == nullptr) && frameClear && wr64LensUsed) {
                            clearRect.rect.left = 0;
                            clearRect.rect.top = 0;
                            clearRect.rect.right = int32_t(p.fbStorage->depthTarget->width);
                            clearRect.rect.bottom = int32_t(p.fbStorage->depthTarget->height);
                        }
                        else if ((p.fbStorage->colorTarget != nullptr) && !frameClear && wr64PanelTransform(wr64Panel)) {
                            const float halfWidth = framebuffer.viewport.width / 2.0f, halfHeight = framebuffer.viewport.height / 2.0f;
                            auto across = [&](int32_t px) { return int32_t(std::lround(((px - halfWidth) / halfWidth * wr64Panel[0] + wr64Panel[1]) * halfWidth + halfWidth)); };
                            auto down = [&](int32_t py) { return int32_t(std::lround(halfHeight - ((halfHeight - py) / halfHeight * wr64Panel[2] + wr64Panel[3]) * halfHeight)); };
                            const int32_t width = int32_t(framebuffer.viewport.width), height = int32_t(framebuffer.viewport.height);
                            clearRect.rect.left = std::clamp(across(clearRect.rect.left), 0, width);
                            clearRect.rect.right = std::clamp(across(clearRect.rect.right), 0, width);
                            clearRect.rect.top = std::clamp(down(clearRect.rect.top), 0, height);
                            clearRect.rect.bottom = std::clamp(down(clearRect.rect.bottom), 0, height);
                        }
                    }
"""

PANEL_SET_ANCHOR = """    struct FramebufferClearChangesDescriptorSet : RenderDescriptorSetBase {
"""

PANEL_SET_REPLACEMENT = """    // wr64: the HUD's two passes, over black and over white, made into one
    // see-through image (rt64_present_queue.cpp).
    struct Wr64PanelCombineDescriptorSet : RenderDescriptorSetBase {
        uint32_t gBlack;
        uint32_t gWhite;
        uint32_t gOutput;

        Wr64PanelCombineDescriptorSet(RenderDevice *device = nullptr) {
            builder.begin();
            gBlack = builder.addTexture(1);
            gWhite = builder.addTexture(2);
            gOutput = builder.addReadWriteTexture(3);
            builder.end();

            if (device != nullptr) {
                create(device);
            }
        }
    };

    struct FramebufferClearChangesDescriptorSet : RenderDescriptorSetBase {
"""

PANEL_RECORD_ANCHOR = """        ShaderRecord boxFilter;
"""

PANEL_RECORD_REPLACEMENT = """        ShaderRecord boxFilter;
        ShaderRecord wr64PanelCombine; // wr64: see rt64_present_queue.cpp.
"""

PANEL_SPIRV_ANCHOR = """#include "shaders/BoxFilterCS.hlsl.spirv.h"
"""

PANEL_SPIRV_REPLACEMENT = """#include "shaders/BoxFilterCS.hlsl.spirv.h"
#include "shaders/Wr64PanelCombineCS.hlsl.spirv.h"
"""

PANEL_DXIL_ANCHOR = """#   include "shaders/BoxFilterCS.hlsl.dxil.h"
"""

PANEL_DXIL_REPLACEMENT = """#   include "shaders/BoxFilterCS.hlsl.dxil.h"
#   include "shaders/Wr64PanelCombineCS.hlsl.dxil.h"
"""

PANEL_METAL_ANCHOR = """#   include "shaders/BoxFilterCS.hlsl.metal.h"
"""

PANEL_METAL_REPLACEMENT = """#   include "shaders/BoxFilterCS.hlsl.metal.h"
#   include "shaders/Wr64PanelCombineCS.hlsl.metal.h"
"""

PANEL_PIPELINE_ANCHOR = """        // Box filter.
        {
"""

PANEL_PIPELINE_REPLACEMENT = """        // wr64: the HUD's two passes combined (rt64_present_queue.cpp).
        {
            Wr64PanelCombineDescriptorSet descriptorSet;
            layoutBuilder.begin();
            layoutBuilder.addPushConstant(0, 0, sizeof(uint32_t) * 2, RenderShaderStageFlag::COMPUTE);
            layoutBuilder.addDescriptorSet(descriptorSet);
            layoutBuilder.end();
            wr64PanelCombine.pipelineLayout = layoutBuilder.create(device);

            std::unique_ptr<RenderShader> computeShader = device->createShader(CREATE_SHADER_INPUTS(Wr64PanelCombineCSBlobDXIL, Wr64PanelCombineCSBlobSPIRV, Wr64PanelCombineCSBlobMSL, "CSMain", shaderFormat));
            RenderComputePipelineDesc pipelineDesc(wr64PanelCombine.pipelineLayout.get(), computeShader.get(), 8, 8, 1);
            wr64PanelCombine.pipeline = device->createComputePipeline(pipelineDesc);
        }

        // Box filter.
        {
"""

PANEL_CMAKE_ANCHOR = """build_compute_shader(rt64 "src/shaders/FbChangesClearCS.hlsl")
"""

PANEL_CMAKE_REPLACEMENT = """build_compute_shader(rt64 "src/shaders/FbChangesClearCS.hlsl")
build_compute_shader(rt64 "src/shaders/Wr64PanelCombineCS.hlsl")
"""

PANEL_SHADER_SOURCE = """//
// wr64: the HUD, drawn twice -- over black and over white -- made into one
// see-through image for a headset to hold in front of the eyes. Where the two
// agree the HUD is solid; where they differ completely there is none of it;
// in between, it lets through as much as they differ. Over black is the HUD's
// colour already multiplied by how solid it is, which is what the headset
// takes. Written for tools/patch_rt64_vr.py.
//

#define BLOCK_SIZE 8

struct Wr64PanelCombineCB {
    uint2 Resolution;
};

[[vk::push_constant]] ConstantBuffer<Wr64PanelCombineCB> gConstants : register(b0);
Texture2D<float4> gBlack : register(t1);
Texture2D<float4> gWhite : register(t2);
RWTexture2D<float4> gOutput : register(u3);

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void CSMain(uint2 coord : SV_DispatchThreadID) {
    if (any(coord >= gConstants.Resolution)) {
        return;
    }

    const float3 black = gBlack.Load(int3(coord, 0)).rgb;
    const float3 white = gWhite.Load(int3(coord, 0)).rgb;
    const float3 through = saturate(white - black);
    const float alpha = 1.0f - max(max(through.r, through.g), through.b);
    gOutput[coord] = float4(min(black, alpha), alpha);
}
"""


# The HUD's passes start each picture black, then white, where the frame first
# draws to it.
PANEL_FRAME_CLEAR_ANCHOR = """        bool depthState = false;
        worker->commandList->setFramebuffer(targetDrawCall.fbStorage->colorDepthWrite.get());
"""

PANEL_FRAME_CLEAR_REPLACEMENT = """        bool depthState = false;
        worker->commandList->setFramebuffer(targetDrawCall.fbStorage->colorDepthWrite.get());

        // wr64: in the HUD's own passes (rt64_projection_processor.cpp) the picture
        // starts black in the first and white in the second -- the game itself
        // seldom clears it, its sky covering everything -- which is what the two
        // are told apart by. Only where the frame first draws to it: the game
        // may draw elsewhere mid-frame (the course card clears a depth buffer)
        // and come back, and what it drew before must stay.
        {
            extern int wr64PanelPass;
            bool wr64FirstUse = true;
            for (uint32_t i = 0; wr64FirstUse && (i < framebufferIndex); i++) {
                wr64FirstUse = (framebufferVector[i].renderTargetDrawCall.fbStorage->colorTarget != colorTarget);
            }

            if ((wr64PanelPass != 0) && (colorTarget != nullptr) && wr64FirstUse) {
                const float level = (wr64PanelPass == 2) ? 1.0f : 0.0f;
                worker->commandList->clearColor(0, RenderColor(level, level, level, 1.0f));
            }
        }

"""

# A fill moved off the picture has nothing left to clear.
PANEL_FILL_GUARD_ANCHOR = """            case InstanceDrawCall::Type::FillRect: {
                const auto &clearRect = drawCall.clearRect;
"""

PANEL_FILL_GUARD_REPLACEMENT = """            case InstanceDrawCall::Type::FillRect: {
                const auto &clearRect = drawCall.clearRect;
                // wr64: a fill moved off the picture onto the panel (in the eyes,
                // when the panel is a layer of its own) has nothing left to clear.
                if ((clearRect.rect.right <= clearRect.rect.left) || (clearRect.rect.bottom <= clearRect.rect.top)) {
                    break;
                }

"""

def ensure_file(target, content, name):
    if target.exists() and target.read_text() == content:
        print(f"  {target.name}: {name} already in place")
        return
    target.write_text(content)
    print(f"  {target.name}: {name} written")


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
                   WORKLOAD_QUEUE, SHARED_RESOURCES, FRAMEBUFFER_RENDERER):
        if not target.exists():
            sys.exit(f"missing {target}. Run: git submodule update --init --recursive")

    patch(PROJ_PROCESSOR, SETTER_ANCHOR, SETTER_REPLACEMENT, "view offset (setter)")
    patch(PROJ_PROCESSOR, OFFSET_ANCHOR, OFFSET_REPLACEMENT, "view offset (camera)")
    patch(PROJ_PROCESSOR, STEREO_ANCHOR, STEREO_REPLACEMENT, "stereo (switch)")
    patch(PROJ_PROCESSOR, EYE_ANCHOR, EYE_REPLACEMENT, "stereo (eye offset)")
    patch(PROJ_PROCESSOR, LENS_ANCHOR, LENS_REPLACEMENT, "headset (lens)")
    patch(PROJ_PROCESSOR, HEAD_ANCHOR, HEAD_REPLACEMENT, "headset (head)")
    patch(SHARED_RESOURCES, COUNTERS_ANCHOR, COUNTERS_REPLACEMENT, "stereo (frame flag)")
    patch(SHARED_RESOURCES, TARGETS_ANCHOR, TARGETS_REPLACEMENT, "stereo (right eye targets)")
    patch(SHARED_RESOURCES, HEAD_TAGS_ANCHOR, HEAD_TAGS_REPLACEMENT, "headset (head tags)")
    patch(WORKLOAD_QUEUE, WORKLOAD_EXTERN_ANCHOR, WORKLOAD_EXTERN_REPLACEMENT, "stereo (workload switch)")
    patch(WORKLOAD_QUEUE, HEAD_EXTERN_ANCHOR, HEAD_EXTERN_REPLACEMENT, "headset (head snapshot)")
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
    patch(PROJ_PROCESSOR, PANEL_PROCESS_ANCHOR, PANEL_PROCESS_REPLACEMENT, "headset (flat onto the panel)")
    patch(FRAMEBUFFER_RENDERER, PANEL_RECT_ANCHOR, PANEL_RECT_REPLACEMENT, "headset (rectangles onto the panel)")
    patch(FRAMEBUFFER_RENDERER, PANEL_SCISSOR_ANCHOR, PANEL_SCISSOR_REPLACEMENT, "headset (scissors onto the panel)")
    patch(FRAMEBUFFER_RENDERER, PANEL_FRAMED_CLIP_ANCHOR, PANEL_FRAMED_CLIP_REPLACEMENT, "headset (framed viewports onto the panel)")
    patch(FRAMEBUFFER_RENDERER, PANEL_CLEAR_ANCHOR, PANEL_CLEAR_REPLACEMENT, "headset (the HUD's passes cleared black and white)")
    patch(FRAMEBUFFER_RENDERER, PANEL_FRAME_CLEAR_ANCHOR, PANEL_FRAME_CLEAR_REPLACEMENT, "headset (the HUD's pictures started black and white)")
    patch(FRAMEBUFFER_RENDERER, PANEL_FILL_GUARD_ANCHOR, PANEL_FILL_GUARD_REPLACEMENT, "headset (fills moved off the picture)")
    patch(DESCRIPTOR_SETS, PANEL_SET_ANCHOR, PANEL_SET_REPLACEMENT, "headset (HUD combination's descriptors)")
    patch(SHADER_LIBRARY_HEADER, PANEL_RECORD_ANCHOR, PANEL_RECORD_REPLACEMENT, "headset (HUD combination's record)")
    patch(SHADER_LIBRARY, PANEL_SPIRV_ANCHOR, PANEL_SPIRV_REPLACEMENT, "headset (HUD combination, SPIR-V)")
    patch(SHADER_LIBRARY, PANEL_DXIL_ANCHOR, PANEL_DXIL_REPLACEMENT, "headset (HUD combination, DXIL)")
    patch(SHADER_LIBRARY, PANEL_METAL_ANCHOR, PANEL_METAL_REPLACEMENT, "headset (HUD combination, Metal)")
    patch(SHADER_LIBRARY, PANEL_PIPELINE_ANCHOR, PANEL_PIPELINE_REPLACEMENT, "headset (HUD combination's pipeline)")
    patch(RT64_CMAKE, PANEL_CMAKE_ANCHOR, PANEL_CMAKE_REPLACEMENT, "headset (HUD combination built)")
    ensure_file(PANEL_SHADER, PANEL_SHADER_SOURCE, "headset (HUD combination's shader)")
    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()

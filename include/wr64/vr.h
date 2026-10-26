#pragma once

// The VR work's home in the port. For now, a debug control that moves the
// race camera sideways through RT64's view offset (tools/patch_rt64_vr.py);
// the headset will be driven from here as well.

namespace RT64 {
struct Application;
}

namespace wr64 {

// Starts VR mode's OpenXR session on a set-up RT64 application, when WR64_VR
// asks for it, by handing attach() below RT64's native device and queue.
// Called by whichever render context owns the application: the port's own
// (src/renderer.cpp, which defines this), or RecompFrontend's when the
// frontend is built (src/frontend.cpp).
void attach_vr(RT64::Application& app);

}  // namespace wr64

namespace wr64::vr {

// F7: steps the view offset through left, right and back to none.
void cycle_debug_offset();

// F8: steps side-by-side stereo through off, normal and exaggerated eye
// separation.
void cycle_stereo();

// VR mode, switched on by WR64_VR=1 (src/vr_openxr.cpp; Windows only).
bool enabled();

// Creates the OpenXR session on RT64's Direct3D 12 device and the command
// queue of its present thread, once RT64 is set up. Does nothing when VR mode
// is off, and turns it off with a message when the headset cannot be used.
void attach(void* d3d12_device, void* d3d12_queue);

// Ends the session. Called before RT64 shuts down, while the device lives.
void shutdown();

// F6: straight ahead becomes the way the head faces now, from where it is.
void recenter();

}  // namespace wr64::vr

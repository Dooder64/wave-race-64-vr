#pragma once

// The VR work's home in the port. For now, a debug control that moves the
// race camera sideways through RT64's view offset (tools/patch_rt64_vr.py);
// the headset will be driven from here as well.

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

}  // namespace wr64::vr

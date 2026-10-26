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

}  // namespace wr64::vr

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

// First person: a race seen from the player's rider's eyes, facing the way
// the chest faces, the head left out (src/first_person.cpp). On by default in
// VR mode; WR64_VR_FIRST_PERSON=0 starts with it off, =1 turns it on without
// a headset too. F5 switches it.
bool first_person();
void toggle_first_person();
// How much of the rider's lean, pitch and roll first person takes, 0 to 1
// (the settings' VR tab).
void set_first_person_intensity(float intensity);

// The headset's Touch controllers as last read, for the game's input
// (src/callbacks.cpp): live only in VR mode with the session focused. Sticks
// run -1 to 1, up and right positive; triggers and grips 0 to 1.
struct Controllers {
    bool live = false;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    float left_trigger = 0, right_trigger = 0, left_grip = 0, right_grip = 0;
    bool a = false, b = false, menu = false;
};
Controllers controllers();

// Each Touch controller's vibration, 0 to 1, from the port's haptics; held
// until changed.
void set_controller_rumble(float left, float right);

// Whether the game draws objects all round its camera rather than only those
// a flat screen would show: in VR mode, or with WR64_CULL_ALL_ROUND=1.
bool all_round();

// How many times farther than the course's own distances objects are drawn
// when they are drawn all round (WR64_VR_DRAW_DISTANCE, 2 by default).
float draw_distance();

}  // namespace wr64::vr

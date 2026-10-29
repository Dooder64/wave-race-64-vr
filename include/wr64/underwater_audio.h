#pragma once

#include <cstddef>
#include <cstdint>

// Every sound heard as if underwater while the first-person eye is below the
// waves: the music and the effects alike, muffled, washed in a long reverb
// and a little quieter, gliding in and out over a quarter of a second
// (src/underwater_audio.cpp).
namespace wr64::underwater_audio {

// From the first-person view, once a game frame: whether the eye is below the
// waves now. When it stops saying (no first-person view: menus, third
// person), the effect fades out.
void set_underwater(bool under);

// Applied to each buffer of interleaved stereo samples on its way out, after
// the replacement music is mixed in.
void process(int16_t* samples, size_t sample_count, uint32_t sample_rate);

}  // namespace wr64::underwater_audio

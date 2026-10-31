#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace wr64::music {
void initialize(const std::filesystem::path& music_directory,
                const std::filesystem::path& bundled_directory = {});
void set_enabled(bool enabled);
void set_volume(double percent);
void set_replacement_volume(double percent);
void mix(int16_t* samples, size_t sample_count, uint32_t sample_rate);

// Only ready replacements suppress native music; missing tracks retain it.
bool replacement_available(uint8_t sequence);
void sequence_started(unsigned player, uint8_t sequence);
void sequence_stopped(unsigned player);
void sequence_state(unsigned player, uint8_t sequence, float gain, bool paused);
// The course music (sequences 6 to 14) heard in its place until the next is
// set, or -1 for each course's own: the race music chosen, swapped here
// rather than in the game, whose next course's instruments may not fit.
void set_race_sequence(int sequence);
// A course's music heard on its own from its start, in place of whatever
// music is playing, while it is asked for each frame (the Free Roam options'
// MUSIC row, src/game_menus.cpp); -1, or a quarter of a second without
// asking, fades back. Its recording only: nothing is heard without one.
void preview(int sequence);
}

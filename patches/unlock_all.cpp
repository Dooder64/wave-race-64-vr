// Debug: every course and difficulty open, whatever the save holds.
//
// The game keeps what the player has opened in two bytes of the save it
// reads into memory (USA Rev A, physical): the courses -- 0x16 is all of
// them, Glacier Coast and Southern Island too -- and the difficulties, bits
// for Hard, Expert and the dolphin to ride (0x13, all three). With the Debug
// tab's switch on (or WR64_UNLOCK_ALL=1), both are set every frame, so any
// course can be picked in Time Trials and any difficulty in Championship,
// whatever has been won or erased. The game may write them into the save
// when it next saves: switching off leaves what it saved.
//
// The addresses and values are the published GameShark codes for this
// version (801CB28B 0016, 801CB280 0013).

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace wr64 {

namespace {

constexpr uint32_t kOpenCourses = 0x1CB28B, kOpenDifficulties = 0x1CB280;
constexpr uint8_t kAllCourses = 0x16, kAllDifficulties = 0x13;

std::atomic<bool> g_unlock{ std::getenv("WR64_UNLOCK_ALL") != nullptr && std::getenv("WR64_UNLOCK_ALL")[0] == '1' };

}  // namespace

// From the settings' Debug tab.
void set_unlock_all(bool on) {
    g_unlock.store(on || (std::getenv("WR64_UNLOCK_ALL") != nullptr && std::getenv("WR64_UNLOCK_ALL")[0] == '1'),
                   std::memory_order_relaxed);
}

// Each frame, from the display-list rewriter.
void unlock_frame(uint8_t* rdram) {
    if (!g_unlock.load(std::memory_order_relaxed)) return;
    // Bytes of big-endian words, as the runtime keeps them.
    rdram[kOpenCourses ^ 3] = kAllCourses;
    rdram[kOpenDifficulties ^ 3] = kAllDifficulties;
}

}  // namespace wr64

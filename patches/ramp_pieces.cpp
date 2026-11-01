// Room for more ramps.
//
// As a course sets up (func_800744EC), each ramp object's slope is built as
// pieces -- six for Dolphin Park's ramps, five for some courses' -- 64 bytes
// each, into one array, D_801BFD78, counted in D_801C0578 (its pieces
// numbered from the ramp's +0x64). The array has room for 32, and the setup
// stops building a ramp's pieces once the next would leave too few for a
// whole ramp (slti $at, $t8, 0x21): four of Dolphin Park's ramps fit, and a
// fifth gets three of its six -- drawn, but met as a wall.
//
// The array is moved to memory nothing else uses -- above the game's 8 MB
// and below the runtime's heap, which starts at 16 MB (0x81000000) -- with
// room for kRoom pieces, and the cap raised to match. The hooks are in
// recomp/wr64.toml: the array's address as the setup takes it (twice: the
// ramps, and Sunset Bay's extra pieces after them), as the slopes are
// followed for the course's objects (func_8006E0F4) and as the jet ski is
// tested against them (func_8007F700, func_8007FAE4); and the cap.

#include <cstdint>
#include <cstdlib>

namespace {
constexpr uint32_t kPieces = 0x80800000u;
constexpr int32_t kRoom = 256;

// WR64_RAMP_PIECES_ORIGINAL=1: the game's own array and cap, for comparison.
bool original() {
    static const bool on = std::getenv("WR64_RAMP_PIECES_ORIGINAL") != nullptr;
    return on;
}
}

extern "C" int32_t wr64_ramp_pieces(void) { return int32_t(original() ? 0x801BFD78u : kPieces); }
extern "C" int32_t wr64_ramp_piece_room(void) { return original() ? 32 : kRoom; }

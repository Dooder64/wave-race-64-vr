// The LAP message on crossing the line, with the lap the HUD shows.
//
// func_801FAEB8 (codeseg) pops up "LAP n" -- or FINAL LAP -- as a human
// crosses the line, n read from the lap counter at race record +0x0. The HUD's
// LAP at the top shows +0x320 instead: the same number, set together as a
// lap is counted, but +0x320 alone goes back one when the line is crossed the
// wrong way and on again when it is recrossed (code_2FB10, 0x80077C90). In
// play, on a ten-lap Reverse race, the two came apart: LAP 8 popped up on the
// HUD's lap 4, LAP 9 on its lap 5. A ten-lap Reverse race of computer riders
// (WR64_LAP_LOG=1) kept every rider's two in step to the end, so it is
// something a human rider does that parts them; the message is made to agree
// with the HUD the player is reading: for its call, each human's +0x0 reads
// as its +0x320, and is put back after. The race's own counting is not
// touched.
//
// Registered over the generated function in src/overlays.cpp.

#include "recomp.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void func_801FAEB8(uint8_t* rdram, recomp_context* ctx);

namespace wr64 {

void lap_message_hook(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t kRecords = 0x801C2938u, kRecordSize = 0x378, kHumanSlots[2] = { 0x800D48DCu, 0x800D48E0u };
    struct Kept { uint32_t record; int32_t lap; bool swapped; } kept[2] = {};
    const int32_t players = MEM_W(0, int32_t(0x800DAB28u));
    for (int p = 0; p < 2 && p < players; ++p) {
        const int32_t slot = MEM_W(0, int32_t(kHumanSlots[p]));
        if (slot < 0 || slot > 3) continue;
        const uint32_t record = kRecords + uint32_t(slot) * kRecordSize;
        const int32_t lap = MEM_W(0, int32_t(record)), hud = MEM_W(0x320, int32_t(record));
        kept[p] = { record, lap, hud > 0 && hud != lap };
        if (kept[p].swapped) {
            static int logged = 0;
            if (logged++ < 20) std::fprintf(stderr, "[wr64] lap message: counter %d, HUD %d -- showing the HUD's\n", lap, hud);
            MEM_W(0, int32_t(record)) = hud;
        }
    }
    func_801FAEB8(rdram, ctx);
    for (const Kept& k : kept) {
        if (k.swapped) MEM_W(0, int32_t(k.record)) = k.lap;
    }
}

}  // namespace wr64

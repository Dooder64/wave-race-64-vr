// VR: swimming back to the jet ski after a fall, in first person.
//
// func_80053EAC starts a rider's animation: a0 the rider's race slot, a1 the
// animation, a2 the frame to start from, a3 its speed. A fall is animation 23
// (thrown), started when the countdown at +0x1608 of the rider's state runs
// out; after a second or so of it the game starts animation 7 from its first
// frame, the climb back on, having placed the rider by the jet ski's stern --
// and if that start is refused it asks again the next frame, the rider
// sinking out of sight meanwhile and the jet ski drifting to a stop (measured
// with WR64_FP_TRACE). So while the first-person view swims back
// (src/first_person.cpp), the human's climb is refused, and the game starts
// it the frame after the swimmer arrives. The game's other animations, and the
// other riders', go through untouched.
//
// Outside a race nothing is held. WR64_SWIM_LOG=1 reports each start of a
// throw or a climb, and each refusal now and then; WR64_TEST_THROW=<seconds>
// throws the human's rider every so many seconds of riding, by setting that
// countdown to 1 -- in scripted test runs only (WR64_INPUT_SCRIPT), as is
// WR64_TEST_FINISH=<seconds>, which marks the rider finished. X on the
// controllers (F3) throws the human's rider at once, the same way: a debug
// wipe-out, for looking round the seafloors.

#include "recomp.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void func_80053EAC(uint8_t*, recomp_context*);

namespace wr64 {

namespace {

// USA Rev A, physical: the human's race slot, each slot's rider state, and the
// game's state (0x28 a race, 0x07 a time trial).
constexpr uint32_t kHumanSlot = 0x0D48DC;
constexpr uint32_t kRiderState = 0x192690;
constexpr uint32_t kRiderStateStride = 0x1718;
constexpr uint32_t kAnimation = 0xC54, kThrowCountdown = 0x1608;
constexpr uint32_t kGameState = 0x0DAB24;

constexpr int32_t kRiding = 22, kThrown = 23, kClimbing = 7;

std::atomic<bool> g_holding{ false };
std::atomic<bool> g_throw_asked{ false };

int32_t& word(uint8_t* rdram, uint32_t physical) { return *reinterpret_cast<int32_t*>(rdram + physical); }
uint16_t& half(uint8_t* rdram, uint32_t physical) { return *reinterpret_cast<uint16_t*>(rdram + (physical ^ 2)); }

int32_t human_slot(uint8_t* rdram) {
    const int32_t slot = word(rdram, kHumanSlot);
    return slot >= 0 && slot < 4 ? slot : 0;
}

bool racing(uint8_t* rdram) {
    const int32_t state = word(rdram, kGameState);
    return state == 0x28 || state == 0x07;
}

}  // namespace

// X on the controllers, F3: the human's rider thrown off next frame it rides.
void swim_request_throw() {
    g_throw_asked.store(true, std::memory_order_relaxed);
}

// From src/first_person.cpp, each frame of a race in first person.
void swim_set_holding(bool holding) {
    g_holding.store(holding, std::memory_order_relaxed);
}

// func_80053EAC, the start of a rider's animation.
void swim_animation_hook(uint8_t* rdram, recomp_context* ctx) {
    static const bool log = std::getenv("WR64_SWIM_LOG") != nullptr;
    const int32_t animation = int32_t(ctx->r5);
    const bool human = int32_t(ctx->r4) == human_slot(rdram);
    if (human && animation == kClimbing && ctx->r6 == 0 && g_holding.load(std::memory_order_relaxed) && racing(rdram)) {
        static int refused = 0;
        if (log && refused++ % 30 == 0) {
            std::fprintf(stderr, "[wr64-swim] climb held back (%d frames so far)\n", refused);
            std::fflush(stderr);
        }
        return;
    }
    if (log && human && (animation == kClimbing || animation == kThrown)) {
        std::fprintf(stderr, "[wr64-swim] animation %d from frame %d\n", animation, int32_t(ctx->r6));
        std::fflush(stderr);
    }
    func_80053EAC(rdram, ctx);
}

// Each frame, from the display-list rewriter: nothing held outside a race,
// and WR64_TEST_THROW's throws.
void swim_frame(uint8_t* rdram) {
    if (!racing(rdram)) g_holding.store(false, std::memory_order_relaxed);

    // A debug wipe-out asked for: thrown as soon as the rider is riding.
    if (g_throw_asked.load(std::memory_order_relaxed)) {
        const uint32_t state = kRiderState + uint32_t(human_slot(rdram)) * kRiderStateStride;
        if (!racing(rdram)) {
            g_throw_asked.store(false, std::memory_order_relaxed);
        }
        else if (word(rdram, state + kAnimation) == kRiding) {
            half(rdram, state + kThrowCountdown) = 1;
            g_throw_asked.store(false, std::memory_order_relaxed);
            std::fprintf(stderr, "[wr64-swim] wiped out on request\n");
            std::fflush(stderr);
        }
    }

    // WR64_TEST_FINISH=<seconds>, in a scripted test run only: the human's
    // rider marked finished (its race record's +0x2F4, as
    // src/haptics_observation.cpp reads it) after so many seconds of racing,
    // to see the game's finish without finishing.
    static const int finish_at = [] {
        const char* env = std::getenv("WR64_TEST_FINISH");
        return env != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr ? int(std::atof(env) * 30.0) : 0;
    }();
    if (finish_at > 0 && racing(rdram)) {
        static int raced = 0;
        if (++raced == finish_at) {
            word(rdram, 0x1C2938 + uint32_t(human_slot(rdram)) * 0x378 + 0x2F4) = 1;
            std::fprintf(stderr, "[wr64-swim] TEST: the human's rider is marked finished\n");
            std::fflush(stderr);
        }
    }

    // Only in a scripted test run (WR64_INPUT_SCRIPT), so a setting left over
    // in a shell can never throw a player, and said so plainly when on.
    static const int every = [] {
        const char* every_env = std::getenv("WR64_TEST_THROW");
        if (every_env == nullptr) return 0;
        if (std::getenv("WR64_INPUT_SCRIPT") == nullptr) {
            std::fprintf(stderr, "[wr64-swim] WR64_TEST_THROW is set but ignored: it only works with WR64_INPUT_SCRIPT\n");
            std::fflush(stderr);
            return 0;
        }
        const int frames = int(std::atof(every_env) * 30.0);
        std::fprintf(stderr, "[wr64-swim] TEST: the rider is thrown every %s seconds of riding\n", every_env);
        std::fflush(stderr);
        return frames;
    }();
    if (every <= 0 || !racing(rdram)) return;
    static int frames = 0;
    static int next = every;
    const uint32_t state = kRiderState + uint32_t(human_slot(rdram)) * kRiderStateStride;
    if (every > 0 && ++frames >= next && word(rdram, state + kAnimation) == kRiding) {
        next = frames + every;
        half(rdram, state + kThrowCountdown) = 1;
        std::fprintf(stderr, "[wr64-swim] test throw\n");
        std::fflush(stderr);
    }
}

}  // namespace wr64

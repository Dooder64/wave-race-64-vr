// Underwater hearing: while the first-person eye is below the waves, every
// sound -- the game's music and effects, and the replacement music mixed into
// them -- is heard as through water: the high frequencies lost (two one-pole
// low-passes, about 650 Hz), a long, dense reverb (eight parallel combs into
// four all-passes a channel, the Freeverb pattern, its delays scaled to the
// game's sample rate, which it changes between screens), and a little
// quieter. Going under and coming up, the mix glides over a quarter of a
// second, and the reverb's tail rings on into it.
//
// It runs on the audio thread, on each buffer as it is queued
// (src/callbacks.cpp, queue_samples). Whether the eye is under comes from the
// first-person view each game frame (set_underwater); a quarter of a second
// without word counts as above water, so menus and the third-person view are
// heard dry.
//
// WR64_UNDERWATER_AUDIO=0 leaves the sound alone; WR64_UNDERWATER_WET (0 to 1,
// default 0.7) sets how much reverb. WR64_UNDERWATER_CAPTURE=<prefix> writes
// what goes out, after this, to <prefix>-<n>.wav (a new file at each change of
// sample rate), for listening to without a headset.

#include "wr64/underwater_audio.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wr64::underwater_audio {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_under{ false };
std::atomic<int64_t> g_told{ 0 };   // when set_underwater last said so, in Clock ticks

bool off() {
    static const bool value = [] { const char* v = std::getenv("WR64_UNDERWATER_AUDIO"); return v != nullptr && v[0] == '0'; }();
    return value;
}

float wet_level() {
    static const float value = [] {
        const char* v = std::getenv("WR64_UNDERWATER_WET");
        return v != nullptr ? std::clamp(float(std::atof(v)), 0.0f, 1.0f) : 0.7f;
    }();
    return value;
}

// A delay line that feeds back through a low-pass: one of the reverb's combs.
struct Comb {
    std::vector<float> line;
    size_t at = 0;
    float held = 0.0f;
    float run(float in, float feedback, float damp) {
        const float out = line[at];
        held = out * (1.0f - damp) + held * damp;
        line[at] = in + held * feedback;
        at = (at + 1) % line.size();
        return out;
    }
};

// An all-pass: diffuses the combs' echoes into a wash.
struct AllPass {
    std::vector<float> line;
    size_t at = 0;
    float run(float in) {
        const float delayed = line[at];
        const float out = delayed - in;
        line[at] = in + delayed * 0.5f;
        at = (at + 1) % line.size();
        return out;
    }
};

struct Channel {
    std::array<Comb, 8> combs;
    std::array<AllPass, 4> passes;
    float low1 = 0.0f, low2 = 0.0f;
};

struct State {
    uint32_t rate = 0;
    std::array<Channel, 2> channels;
    float mix = 0.0f;
    float low_coefficient = 0.0f;

    void prepare(uint32_t sample_rate) {
        if (rate == sample_rate) return;
        rate = sample_rate;
        // Freeverb's tunings, in samples at 44.1 kHz; the right channel a
        // little longer, for width.
        constexpr int kCombs[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        constexpr int kPasses[4] = { 556, 441, 341, 225 };
        constexpr int kSpread = 23;
        const float scale = float(sample_rate) / 44100.0f;
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < 8; ++i) {
                channels[c].combs[i].line.assign(size_t(std::max(1.0f, (kCombs[i] + c * kSpread) * scale)), 0.0f);
                channels[c].combs[i].at = 0;
                channels[c].combs[i].held = 0.0f;
            }
            for (int i = 0; i < 4; ++i) {
                channels[c].passes[i].line.assign(size_t(std::max(1.0f, (kPasses[i] + c * kSpread) * scale)), 0.0f);
                channels[c].passes[i].at = 0;
            }
            channels[c].low1 = channels[c].low2 = 0.0f;
        }
        // One pole at about 650 Hz.
        low_coefficient = 1.0f - std::exp(-6.2831853f * 650.0f / float(sample_rate));
    }
};

State& state() {
    static State value;
    return value;
}

// The capture: 16-bit stereo WAV files, the header rewritten as they grow.
struct Capture {
    std::FILE* file = nullptr;
    uint32_t rate = 0, bytes = 0, segment = 0;
    void header() {
        const uint32_t words[] = { 36 + bytes, 16, 0x00020001, rate, rate * 4, 0x00100004, bytes };
        std::fseek(file, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, file); std::fwrite(&words[0], 4, 1, file);
        std::fwrite("WAVEfmt ", 1, 8, file); std::fwrite(&words[1], 4, 5, file);
        std::fwrite("data", 1, 4, file); std::fwrite(&words[6], 4, 1, file);
        std::fseek(file, 0, SEEK_END);
    }
    void write(const int16_t* samples, size_t count, uint32_t sample_rate) {
        static const char* prefix = std::getenv("WR64_UNDERWATER_CAPTURE");
        if (prefix == nullptr) return;
        if (sample_rate != rate) {
            if (file != nullptr) { header(); std::fclose(file); }
            rate = sample_rate;
            bytes = 0;
            file = std::fopen((std::string(prefix) + "-" + std::to_string(segment++) + ".wav").c_str(), "wb");
            if (file != nullptr) header();
        }
        if (file == nullptr) return;
        bytes += uint32_t(std::fwrite(samples, sizeof(int16_t), count, file) * sizeof(int16_t));
        header();
    }
};

Capture& capture() {
    static Capture value;
    return value;
}

}  // namespace

void set_underwater(bool under) {
    g_under.store(under, std::memory_order_relaxed);
    g_told.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

void process(int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    if (off() || samples == nullptr || sample_rate == 0) return;
    struct Captured {
        int16_t* samples; size_t count; uint32_t rate;
        ~Captured() { capture().write(samples, count, rate); }
    } captured{ samples, sample_count, sample_rate };
    State& s = state();
    const int64_t told = g_told.load(std::memory_order_relaxed);
    const bool fresh = Clock::now().time_since_epoch().count() - told <
                       std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(250)).count();
    const float target = (fresh && g_under.load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
    // Nothing to do above water once the tail has died away.
    if (target == 0.0f && s.mix <= 0.0f) return;
    s.prepare(sample_rate);

    // The mix moves a quarter of a second's worth per second of sound.
    const float step = 1.0f / (0.25f * float(sample_rate));
    const float wet = wet_level();
    // Large-room settings: a long tail, the highs dying in it first.
    constexpr float kFeedback = 0.88f, kDamp = 0.45f, kInput = 0.012f;
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        s.mix += std::clamp(target - s.mix, -step, step);
        const float m = s.mix;
        for (int c = 0; c < 2; ++c) {
            Channel& ch = s.channels[c];
            const float dry = float(samples[i + c]);
            ch.low1 += (dry - ch.low1) * s.low_coefficient;
            ch.low2 += (ch.low1 - ch.low2) * s.low_coefficient;
            const float muffled = ch.low2;
            float reverb = 0.0f;
            for (Comb& comb : ch.combs) reverb += comb.run(muffled * kInput, kFeedback, kDamp);
            for (AllPass& pass : ch.passes) reverb = pass.run(reverb);
            const float under = 0.8f * (muffled * (1.0f - 0.5f * wet) + reverb * wet * 2.2f);
            const float out = dry + (under - dry) * m;
            samples[i + c] = int16_t(std::clamp(out, -32768.0f, 32767.0f));
        }
    }
}

}  // namespace wr64::underwater_audio

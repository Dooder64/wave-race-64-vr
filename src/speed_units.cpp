// The race HUD's speed in miles an hour.
//
// The game knows only kilometres an hour: the HUD draws the speed as a
// picture for each digit and one of "km/h" after them, rectangles of their
// own in the frame's list. With miles an hour chosen (settings), before a
// race frame's list is rewritten it is scanned for them -- each digit's
// picture is one of a run of ten, 0 to 9 (Time Trials and Championship:
// 16x20, from 0x0103D8D8, 0x500 apart; Stunt Mode: 12x16, from 0x0802C3E8,
// 0x300 apart), the speed's told from the lap's, of the same run, by where
// it is drawn -- the number read off, turned into miles an hour, and as the
// list is rewritten each digit's picture is the new number's instead, right
// aligned, digits it no longer needs left out, and "km/h" becomes "mph".
//
// "mph" is drawn here, in the game's lettering: 24x12, as "km/h" is, a
// pixel wide strokes outlined in dark, each row the brightest yellow to
// orange of the same row of "km/h".

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wr64::speed_units {

namespace {

// The setting; WR64_SPEED_MPH=1 for scripted tests.
std::atomic<bool> g_mph{std::getenv("WR64_SPEED_MPH") != nullptr && std::getenv("WR64_SPEED_MPH")[0] == '1'};

constexpr uint32_t kKilometres = 0x010337D8;                      // "km/h", 24x12 RGBA32
constexpr int kLabelWidth = 24, kLabelHeight = 12;
struct Digits { uint32_t zero, stride; };
constexpr Digits kDigitRuns[] = { { 0x0103D8D8, 0x500 }, { 0x0802C3E8, 0x300 } };
constexpr uint32_t kShared = 0x228E10;
// Where "mph" is drawn: unused by the game in any race (checked in dumps).
constexpr uint32_t kMiles = 0x75F000;

// The digit a texture image is, and its run; or -1.
int digit_of(uint32_t image, int* run) {
    for (int r = 0; r < 2; ++r) {
        const uint32_t from = kDigitRuns[r].zero;
        if (image >= from && image < from + 10 * kDigitRuns[r].stride && (image - from) % kDigitRuns[r].stride == 0) {
            *run = r;
            return int((image - from) / kDigitRuns[r].stride);
        }
    }
    return -1;
}

// The frame's changes: texture images to use instead, by the command's
// physical address, and rectangles left out.
std::unordered_map<uint32_t, uint32_t> g_images;
std::unordered_set<uint32_t> g_dropped;

// "mph": o outline, # filled.
constexpr const char* kMask[kLabelHeight] = {
    "........................",
    "................ooo.....",
    "................o#o.....",
    "................o#o.....",
    ".oooooooooooooooo#oooo..",
    ".o#######oo####oo####o..",
    ".o#oo#oo#oo#oo#oo#oo#o..",
    ".o#oo#oo#oo####oo#oo#o..",
    ".o#oo#oo#oo#ooooo#oo#o..",
    ".o#oo#oo#oo#o...o#oo#o..",
    ".oooooooooooo...oooooo..",
    "........................",
};

// Draws "mph" at kMiles from "km/h" at physical `kilometres`.
void draw_miles(uint8_t* rdram, uint32_t kilometres) {
    const auto pixel = [&](uint32_t at, int x, int y) {
        return *reinterpret_cast<const uint32_t*>(rdram + ((at + uint32_t(y * kLabelWidth + x) * 4) & 0x7FFFFC));
    };
    // Each row's brightest fill, and the outline's colour.
    uint32_t fill[kLabelHeight] = {}, outline = 0x2C0000F4u;
    for (int y = 0; y < kLabelHeight; ++y) {
        int best = -1;
        for (int x = 0; x < kLabelWidth; ++x) {
            const uint32_t v = pixel(kilometres, x, y);
            const int r = int(v >> 24), g = int((v >> 16) & 0xFF);
            if ((v & 0xFF) < 200 || r + g < 200) continue;
            if (r + g > best) { best = r + g; fill[y] = v | 0xFFu; }
        }
    }
    for (int y = kLabelHeight - 2; y >= 0; --y) if (fill[y] == 0) fill[y] = fill[y + 1];
    for (int y = 1; y < kLabelHeight; ++y) if (fill[y] == 0) fill[y] = fill[y - 1];
    for (int y = 0; y < kLabelHeight; ++y) {
        for (int x = 0; x < kLabelWidth; ++x) {
            const char m = kMask[y][x];
            const uint32_t v = m == '#' ? fill[y] : m == 'o' ? outline : 0u;
            *reinterpret_cast<uint32_t*>(rdram + kMiles + uint32_t(y * kLabelWidth + x) * 4) = v;
        }
    }
}

}  // namespace

// From the settings.
void set_mph(bool on) {
    g_mph.store(on || (std::getenv("WR64_SPEED_MPH") != nullptr && std::getenv("WR64_SPEED_MPH")[0] == '1'), std::memory_order_relaxed);
}

// Before a race frame's list (physical `list`) is rewritten: finds its speed
// and readies the changes.
void scan(uint8_t* rdram, uint32_t list) {
    g_images.clear();
    g_dropped.clear();
    if (!g_mph.load(std::memory_order_relaxed)) return;
    struct Slot { uint32_t image_at, rect_at, left; int digit, run; };
    std::vector<Slot> slots;
    // Segment 1, the shared pictures', is set before the race's list, always
    // at 0x228E10 (every course and mode traced); taken from the list if it
    // sets it.
    uint32_t segments[16] = {};
    segments[1] = kShared;
    uint32_t image = 0, image_at = 0, label_at = 0;
    uint32_t at = list & 0x7FFFF8;
    for (int i = 0; i < 40000; ++i, at += 8) {
        const uint32_t w0 = *reinterpret_cast<const uint32_t*>(rdram + at);
        const uint32_t w1 = *reinterpret_cast<const uint32_t*>(rdram + at + 4);
        const uint8_t op = uint8_t(w0 >> 24);
        if (op == 0xB8) break;
        // A segment's base (G_MOVEWORD, its index G_MW_SEGMENT, as the rewriter reads it).
        if (op == 0xDB && (w0 & 0xFF) == 0x06) segments[(w0 >> 10) & 0xF] = w1 & 0xFFFFFF;
        if (op == 0xFD) { image = w1; image_at = at; }
        if (op != 0xE4) continue;
        const uint32_t left = ((w1 >> 12) & 0xFFF) / 4, top = (w1 & 0xFFF) / 4;
        int run = -1;
        const int digit = digit_of(image, &run);
        // The speed's digits: top right of the middle; the lap's is left of it.
        if (digit >= 0 && left >= 158 && top >= 30 && top <= 42) slots.push_back({ image_at, at, left, digit, run });
        if (image == kKilometres && left >= 200) label_at = image_at;
    }
    if (slots.empty()) return;
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.left < b.left; });
    int kilometres = 0;
    for (const Slot& s : slots) kilometres = kilometres * 10 + s.digit;
    int miles = int(std::lround(kilometres * 0.621371));
    // Right aligned: the last slot the ones, and so on; slots left over
    // before the number's first digit go.
    for (int k = int(slots.size()) - 1; k >= 0; --k) {
        const Slot& s = slots[size_t(k)];
        const bool needed = k == int(slots.size()) - 1 || miles > 0;
        if (needed) {
            g_images[s.image_at] = kDigitRuns[s.run].zero + uint32_t(miles % 10) * kDigitRuns[s.run].stride;
        } else {
            g_dropped.insert(s.rect_at);
        }
        miles /= 10;
    }
    if (label_at != 0 && segments[1] != 0) {
        static uint32_t drawn_from = 0;
        const uint32_t kilometres_label = segments[1] + (kKilometres & 0xFFFFFF);
        if (drawn_from != kilometres_label) {
            draw_miles(rdram, kilometres_label);
            drawn_from = kilometres_label;
        }
        g_images[label_at] = 0x80000000u | kMiles;
    }
}

// As the list is rewritten, for the command at physical `at`: the texture
// image to set instead of `image`; and whether a rectangle is left out.
uint32_t image(uint32_t at, uint32_t image) {
    if (g_images.empty()) return image;
    const auto it = g_images.find(at);
    return it == g_images.end() ? image : it->second;
}
bool dropped(uint32_t at) { return !g_dropped.empty() && g_dropped.count(at) != 0; }

}  // namespace wr64::speed_units

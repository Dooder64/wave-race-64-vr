// The mod's pages in the game's own menus: FREE ROAM, the WAVES pages and the
// Free Roam options.
//
// The main menu (game state 0x03, overlay ovl_i0; docs/MENUS.md) is a table
// of pages the game moves through with up, down, A and B. Its first page
// gets a sixth row, FREE ROAM, above OPTIONS: a choice of the game's own
// table (rebuilt at kMainRows, the page pointed at it), ending the menu as
// STUNT MODE does, so a free roam is a Stunt Mode run on any of the nine
// courses, always without buoys or rings (patches/free_roam.cpp). Whichever
// choice ends the menu says whether the run is a free roam.
//
// Then a page of the mod's own wherever the run's last choice is made:
//
//   - Championship: after the difficulty (or WARM UP / START THE RACE), on the
//     main menu, before the choice ends it -- WAVES, once for the whole
//     championship, and MISSES and LAPS: the game's own CHANGE CONDITIONS
//     values for that difficulty (D_801CB308[difficulty]: wave conditions,
//     misses 1-10, laps 3-10: the game's own page allows 1-5 and 3-9), which
//     each race reads as it sets up; drawn with
//     the game's number lettering (func_801E9858, font 2, as that page does).
//     REVERSE, which the game races with 5 and 3 whatever its CHANGE
//     CONDITIONS say, has its own, in the Races tab (patches/free_roam.cpp
//     holds them in its races).
//   - Time Trials, Stunt Mode and 2P VS: after the course (and, but for Stunt
//     Mode, its difficulty) on the course select screen (state 0x14, overlay
//     ovl_i4), where the race would start -- WAVES.
//   - Free Roam: after the course -- its options: WAVES, SKY (and water),
//     MUSIC, FOG and RIDERS (the Dolphin Park riders); on Dolphin Park,
//     LAYOUT first, Stunt Mode's or the championship's Warm Up (started as
//     CONTINUE is chosen, patches/free_roam.cpp). On the MUSIC row the music
//     shown is heard (wr64::music::preview).
//
//   - Options: an eighth row, EXTRAS, before END on the game's OPTIONS
//     screen (game state 0x3C, overlay ovl_i7), opens the mod's settings that
//     belong in the game: SPEED, VIEW, TILT, FEEDBACK, TEXTURES, WATER,
//     STYLE, RIPPLES, SPRAY and UNLOCK ALL. The screen's draw and frame are
//     the mod's while it is up; its other rows do as the game's.
//
// Each ends with CONTINUE (EXTRAS, with END). Up and down move, left and right (or A) change a
// row, B goes back to where the page was opened from, and A on CONTINUE
// carries on as the game would have: on the main menu its own controls are
// run with the cursor put back; on the course select, the race is started
// (func_i4_802C7474(0x28), every way into a race from there). A course
// select page opens by catching that call; the game's "start" chime, played
// straight after it, is left out until CONTINUE plays it.
//
// The values are the settings' Races and Free Roam tabs' (frontend.cpp),
// saved as they change.
//
// Pages are drawn with the game's calls, as the game draws its own: its setup
// list D_106F628, a bar per row (func_80094338), the words in colour style
// 0, 6 for the chosen row (func_801E946C), each word a picture by number
// (func_801E3EE0, font 1), func_801E5554 to end; on the course select, over
// a dark see-through box (func_801E7C58, as the pause menu has).
//
// The game has no pictures for most of the new words. They are made from its
// own lettering: each letter cut from a word that has it (the letter's bright
// core, found at the see-through columns between cores, plus the dark
// outline column either side), laid a column apart as the game's words are.
// Flat letters span rows 1-9 and round ones 0-9 in the words picked; some
// words (HARD, EXPERT, ON) are a row taller. W is an M upside down; X is cut
// from EXPERT with a row taken out of its middle. The letters are read from
// the cartridge's segment 1 (ROM 0xF6090), the pictures written at physical
// kPictures. FREE ROAM, drawn by the game's own main page, has an id of its
// own in the font 1 table; every other made word is drawn through one id
// whose entry is rewritten before each word, the address being read into the
// display list as it is drawn. "The course's own" is the game's NORMAL, and
// the courses' names, ON and OFF are the game's own words.
//
// WR64_GAME_MENUS=0 leaves the menus as the game has them.

#include "recomp.h"
#include "wr64/music.h"
#include "wr64/updates.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" void func_i0_802C5A7C(uint8_t*, recomp_context*);
extern "C" void func_i0_802C6044(uint8_t*, recomp_context*);
extern "C" void func_i0_802C63AC(uint8_t*, recomp_context*);
extern "C" void func_i4_802C5800(uint8_t*, recomp_context*);
extern "C" void func_i7_802C5800(uint8_t*, recomp_context*);
extern "C" void func_i7_802C5A18(uint8_t*, recomp_context*);
extern "C" void func_i4_802C5B78(uint8_t*, recomp_context*);
extern "C" void func_i4_802C6BAC(uint8_t*, recomp_context*);
extern "C" void func_i4_802C7474(uint8_t*, recomp_context*);
extern "C" void func_80094338(uint8_t*, recomp_context*);
extern "C" void func_801E946C(uint8_t*, recomp_context*);
extern "C" void func_801E3EE0(uint8_t*, recomp_context*);
extern "C" void func_801E9858(uint8_t*, recomp_context*);
extern "C" void func_801E5554(uint8_t*, recomp_context*);
extern "C" void func_801E7C58(uint8_t*, recomp_context*);
extern "C" void AudioGeneral_PlaySound(uint8_t*, recomp_context*);

namespace recomp {
void do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes);
}

namespace wr64 {

// frontend.cpp: a Races or Free Roam tab setting (a switch as 0 or 1, a
// list's choice as its number).
uint32_t menu_setting(const char* tab, const char* id);
void set_menu_setting(const char* tab, const char* id, uint32_t to);
// patches/free_roam.cpp.
void set_free_roam(bool on);
bool free_roam_chosen();
void swallow_sound_once(int sound);
void start_warm_up(uint8_t* rdram);
void leave_warm_up(uint8_t* rdram);
// frontend.cpp: the view (chase 0, first person 1) for this display, and the
// first-person tilt (none 0, half 1, full 2).
uint32_t view_setting();
void set_view_setting(uint32_t to);
uint32_t tilt_setting();
void set_tilt_setting(uint32_t to);

namespace {

constexpr uint32_t kGameState = 0x0DAB24, kChosenMode = 0x1CE608;
// The course select's row (D_800DA9D0) and the course of each (D_800DAAD8), from
// which the game sets gCourseID only once it has faded out.
constexpr uint32_t kCourseRow = 0x0DA9D0, kCourseOfRow = 0x0DAAD8;
constexpr int32_t kTimeTrials = 0, kVersus = 1, kChampionship = 4, kStunt = 11;
constexpr uint32_t kButtons = 0x1CE65A, kControllerSize = 0x1C;   // pressed this frame (u16)
constexpr uint32_t kFont1Table = 0x226568;
constexpr uint32_t kBarSetup = 0x0106F628;

// The main menu's state (ovl_i0), physical.
constexpr uint32_t kPage = 0x2C6EE0, kCursor = 0x2C6EE4, kRowsDrawn = 0x2C6F0C, kRowsChosen = 0x2C6F10,
                   kTopRow = 0x2C6BD0, kIdle = 0x2C6F14;
// The page table (0x18 bytes a page) and the first page's choices (0x18
// bytes each), as the overlay loads them.
constexpr uint32_t kPages = 0x2C6E08, kMainChoices = 0x2C6C28;
constexpr int kChoiceSize = 0x18, kGameMainRows = 5;
// The first page's rows: the game's, with FREE ROAM put in before OPTIONS.
constexpr int kStuntRow = 2, kFreeRoamRow = 4, kMainRowCount = 6;

// The course select's (ovl_i4): the controller it reads.
constexpr uint32_t kCourseSelectPad = 0x2C76B0;
constexpr uint32_t kCourseSelectState = 0x14, kStartRace = 0x28;

// The OPTIONS screen's (ovl_i7): its state, the "first frame" flag and the
// state before, the game's row (gCurrentOptionsMenuItem), and the flag its
// frame clears. Its seven rows: each word's x from 0x6D, and the word; the
// mod's EXTRAS goes in before END.
constexpr uint32_t kOptionsState = 0x3C, kFirstFrame = 0x1CE63C, kPrevState = 0x1CE634, kOptionsItem = 0x0DA9D4,
                   kOptionsFlag = 0x0DAB2C;
struct GameRow { int x, word; };
constexpr GameRow kOptionRows[] = { { 0, 0x19 }, { 2, 0x1A }, { -20, 0x1B }, { 29, 0x1C }, { -33, 0x1D }, { -2, 0x1E },
                                    { 35, 0x3F } };
constexpr int kExtrasRow = 6, kOptionsRowCount = 8;
// Its rows: 18 apart from 68 (the game's seven are 19 apart from 73).
constexpr int kOptionsTop = 68, kOptionsStep = 18;

constexpr uint16_t kA = 0x8000, kB = 0x4000, kZ = 0x2000, kStart = 0x1000, kUp = 0x0800, kDown = 0x0400,
                   kLeft = 0x0200, kRight = 0x0100;
constexpr int kSoundMove = 0x10, kSoundChoose = 0x11, kSoundBack = 0x16, kSoundStart = 0x37;
constexpr int kStyleNormal = 0, kStyleChosen = 6;

// Font 1 words the game has (segment 1).
constexpr int kWordNormal = 0x16, kWordOn = 0x37, kWordOff = 0x38, kWordContinue = 0x30, kWordCourses = 0x08,
              kWordEnd = 0x3F;

// Where the new pictures go, and where the cartridge's letters are read to
// once: above the floor's lists and the fish, below the rewriter's scratch
// list at 0x700000.
constexpr uint32_t kPictures = 0x6F0000, kMainRows = 0x6F7000, kLetterCopy = 0x6F8000;
constexpr uint32_t kSegment1Rom = 0xF6090, kCopyFrom = 0x5F000, kCopySize = 0x7000;
constexpr uint32_t kRomBase = 0x10000000;
constexpr int kHeight = 10;

// A letter: the segment-1 word picture it is cut from, that picture's width,
// its core's columns [from, to), and how it is turned into the letter.
enum Shape { kAsIs, kUpsideDown, kRowOut, kTail };
struct Letter { char c; uint32_t picture; int width, from, to; Shape shape; };
constexpr Letter kLetters[] = {
    { 'A', 0x01060AC0, 56, 38, 45, kAsIs },       // NORMAL
    { 'B', 0x0105FDD0, 64, 31, 38, kAsIs },       // PORT BLUE
    { 'C', 0x01061900, 48, 9, 16, kAsIs },        // SCORE
    { 'D', 0x01061F60, 40, 12, 20, kAsIs },       // RIDER
    { 'E', 0x01061DC8, 40, 8, 15, kAsIs },        // LEVEL
    { 'F', 0x01065C08, 24, 10, 16, kAsIs },       // OFF
    { 'G', 0x01060380, 88, 0, 7, kAsIs },         // GLACIER COAST
    { 'H', 0x010606F8, 96, 28, 35, kAsIs },       // SOUTHERN ISLAND
    { 'I', 0x01061F60, 40, 9, 11, kAsIs },        // RIDER
    { 'K', 0x01061C30, 40, 27, 35, kAsIs },       // RANK
    { 'L', 0x01061DC8, 40, 0, 7, kAsIs },         // LEVEL
    { 'M', 0x01060AC0, 56, 27, 36, kAsIs },       // NORMAL
    { 'N', 0x01060AC0, 56, 0, 8, kAsIs },         // NORMAL
    { 'O', 0x01060AC0, 56, 9, 17, kAsIs },        // NORMAL
    { 'P', 0x0105FDD0, 64, 0, 6, kAsIs },         // PORT BLUE
    { 'Q', 0x01060AC0, 56, 9, 17, kTail },        // NORMAL's O, with a tail
    { 'R', 0x01061F60, 40, 0, 8, kAsIs },         // RIDER
    { 'S', 0x01061900, 48, 0, 8, kAsIs },         // SCORE
    { 'T', 0x0105FDD0, 64, 22, 28, kAsIs },       // PORT BLUE
    { 'U', 0x0105FDD0, 64, 45, 51, kAsIs },       // PORT BLUE
    { 'V', 0x01061DC8, 40, 16, 23, kAsIs },       // LEVEL
    { 'W', 0x01060AC0, 56, 27, 36, kUpsideDown }, // NORMAL's M
    { 'X', 0x01060E90, 56, 9, 17, kRowOut },      // EXPERT, a row taller
    { 'Y', 0x0105F130, 80, 29, 35, kAsIs },       // SUNNY BEACH
};
// A space: as wide as PORT BLUE's.
constexpr int kSpace = 2;

// The made words.
enum Word { kFreeRoam, kRiders, kWaves, kSky, kMusic, kFog,
            kStill, kCalm, kGentle, kChoppy, kRough, kStormy, kWild, kExtreme,
            kLayout, kStuntMode, kWarmUp,
            kExtras, kSpeed, kView, kTilt, kFeedback, kTextures, kWater, kStyle, kRipples, kSpray, kUnlockAll,
            kKph, kMph, kChase, kFirstPerson, kNone, kHalf, kFull, kEvents, kOriginal, kHd, kModern, kHigh,
            kClassic, kAqua, kSoft, kStrong, kMisses, kLaps, kUpdateAvailable, kWordCount };
constexpr const char* kTexts[kWordCount] = { "FREE ROAM", "RIDERS", "WAVES", "SKY", "MUSIC", "FOG",
                                             "STILL", "CALM", "GENTLE", "CHOPPY", "ROUGH", "STORMY", "WILD", "EXTREME",
                                             "LAYOUT", "STUNT MODE", "WARM UP",
                                             "EXTRAS", "SPEED", "VIEW", "TILT", "FEEDBACK", "TEXTURES", "WATER", "STYLE",
                                             "RIPPLES", "SPRAY", "UNLOCK ALL",
                                             "KPH", "MPH", "CHASE", "FIRST PERSON", "NONE", "HALF", "FULL", "EVENTS",
                                             "ORIGINAL", "HD", "MODERN", "HIGH", "CLASSIC", "AQUA", "SOFT", "STRONG",
                                             "MISSES", "LAPS", "UPDATE AVAILABLE" };
// FREE ROAM's own id, and the one every other made word is drawn through:
// empty in the font 1 table.
constexpr int kFreeRoamId = 0x28, kScratchId = 0x2B;

struct Picture { uint32_t at = 0; int width = 0, ink = 0; std::vector<uint8_t> pixels; };
Picture g_pictures[kWordCount];
bool g_made = false;

bool enabled() {
    const char* v = std::getenv("WR64_GAME_MENUS");
    return v == nullptr || v[0] != '0';
}

uint32_t phys(gpr address) { return uint32_t(address) & 0x1FFFFFFF; }
void store32(uint8_t* rdram, uint32_t at, uint32_t v) { *reinterpret_cast<uint32_t*>(rdram + at) = v; }
int32_t load32(const uint8_t* rdram, uint32_t at) { return *reinterpret_cast<const int32_t*>(rdram + at); }
uint16_t load16(const uint8_t* rdram, uint32_t at) { return *reinterpret_cast<const uint16_t*>(rdram + (at ^ 2)); }

// The course on the course select's chosen row.
int chosen_course(const uint8_t* rdram) {
    return load32(rdram, kCourseOfRow + uint32_t(std::clamp(load32(rdram, kCourseRow), 0, 8)) * 4);
}

// Calls a game function with up to four register and ten stack arguments,
// on a frame of its own below the caller's; returns v0.
using GameFunction = void (*)(uint8_t*, recomp_context*);
uint32_t call(uint8_t* rdram, recomp_context* ctx, GameFunction function, std::initializer_list<uint32_t> args) {
    const gpr sp = ctx->r29;
    ctx->r29 = sp - 0x40;
    gpr* registers[4] = { &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7 };
    size_t i = 0;
    for (uint32_t a : args) {
        if (i < 4) *registers[i] = gpr(int64_t(int32_t(a)));
        else store32(rdram, phys(ctx->r29) + 0x10 + uint32_t(4 * (i - 4)), a);
        ++i;
    }
    function(rdram, ctx);
    const uint32_t result = uint32_t(ctx->r2);
    ctx->r29 = sp;
    return result;
}

void play(uint8_t* rdram, recomp_context* ctx, int sound) { call(rdram, ctx, AudioGeneral_PlaySound, { uint32_t(sound), 0 }); }

// Cuts the letters from the cartridge and lays out the new words.
void make_pictures(uint8_t* rdram) {
    recomp::do_rom_read(rdram, gpr(int64_t(int32_t(0x80000000u | kLetterCopy))), kRomBase | (kSegment1Rom + kCopyFrom), kCopySize);
    const auto source = [&](uint32_t picture, int width, int x, int y) -> uint8_t {
        if (y < 0 || y >= kHeight) return 0;
        const uint32_t offset = (picture & 0xFFFFFF) - kCopyFrom + uint32_t(y * width + x);
        return offset < kCopySize ? rdram[(kLetterCopy + offset) ^ 3] : 0;
    };
    // The source row each row of a letter is taken from (-1 for none).
    const auto source_row = [](Shape shape, int y) {
        switch (shape) {
            case kUpsideDown: return y == 0 ? -1 : kHeight - y;   // rows 1-9 the other way up
            case kRowOut: return y == 0 ? -1 : y <= 4 ? y - 1 : y;  // rows 0-9 less row 4, down a row
            default: return y;
        }
    };
    uint32_t at = kPictures;
    for (int w = 0; w < kWordCount; ++w) {
        std::vector<uint8_t> canvas(size_t(256 * kHeight), 0);
        int pen = 1;
        for (const char* c = kTexts[w]; *c; ++c) {
            if (*c == ' ') { pen += kSpace; continue; }
            const Letter* letter = nullptr;
            for (const Letter& l : kLetters) if (l.c == *c) letter = &l;
            if (letter == nullptr) { std::fprintf(stderr, "[wr64] game menus: no letter %c\n", *c); continue; }
            for (int sx = std::max(0, letter->from - 1); sx < std::min(letter->width, letter->to + 1); ++sx) {
                const int dx = pen + sx - letter->from;
                if (dx < 0 || dx >= 256) continue;
                const bool edge = sx < letter->from || sx >= letter->to;
                for (int y = 0; y < kHeight; ++y) {
                    const uint8_t v = source(letter->picture, letter->width, sx, source_row(letter->shape, y));
                    // Beside the core only the outline is the letter's; a
                    // bright pixel there is the next letter of its word.
                    if (edge && (v >> 4) > 8) continue;
                    uint8_t& old = canvas[size_t(y * 256 + dx)];
                    // Where letters' outlines meet, the more solid pixel wins.
                    if ((v & 0xF) > (old & 0xF) || ((v & 0xF) == (old & 0xF) && (v >> 4) > (old >> 4))) old = v;
                }
            }
            if (letter->shape == kTail) {
                // A stroke out of the O's foot, to the right: bright, with
                // the dark outline below and beside it.
                const int w = letter->to - letter->from;
                const int tail[][3] = { { w - 3, 7, 0xFF }, { w - 2, 8, 0xFF }, { w - 1, 9, 0xFF },
                                        { w - 2, 7, 0x0F }, { w - 1, 8, 0x0F }, { w, 9, 0x0F },
                                        { w - 3, 8, 0x0F }, { w - 2, 9, 0x0F }, { w, 8, 0x0F } };
                for (const auto& t : tail) {
                    uint8_t& at = canvas[size_t(t[1] * 256 + pen + t[0])];
                    if ((t[2] & 0xF0) != 0 || (at & 0xF0) < 0x90) at = uint8_t(t[2]);
                }
            }
            pen += letter->to - letter->from + 1;
        }
        Picture& p = g_pictures[w];
        p.ink = pen + 1;
        p.width = (p.ink + 7) / 8 * 8;
        p.at = at;
        p.pixels.resize(size_t(p.width * kHeight));
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < p.width; ++x) p.pixels[size_t(y * p.width + x)] = canvas[size_t(y * 256 + x)];
        at += uint32_t(p.pixels.size() + 7) / 8 * 8;
    }
    g_made = true;
    std::fprintf(stderr, "[wr64] game menus: %d words made, %u bytes\n", int(kWordCount), at - kPictures);
}

// Puts the pictures in RDRAM and FREE ROAM's entry in the font 1 table;
// every frame a menu is up, since the table's segment is loaded again now
// and then.
void place_pictures(uint8_t* rdram) {
    if (!g_made) make_pictures(rdram);
    for (int w = 0; w < kWordCount; ++w) {
        const Picture& p = g_pictures[w];
        for (size_t i = 0; i < p.pixels.size(); ++i) rdram[(p.at + uint32_t(i)) ^ 3] = p.pixels[i];
    }
    store32(rdram, kFont1Table + uint32_t(kFreeRoamId) * 8, 0x80000000u | g_pictures[kFreeRoam].at);
    store32(rdram, kFont1Table + uint32_t(kFreeRoamId) * 8 + 4, uint32_t(g_pictures[kFreeRoam].width));
}

// A word to draw: the game's own font 1 id, one of the made words, or a
// number in the game's number lettering.
struct Text { bool made; int id; int number = -1; };
constexpr Text game(int id) { return { false, id }; }
constexpr Text made(Word w) { return { true, int(w) }; }
constexpr Text number(int n) { return { false, 0, n }; }

// The title's version line's steps: a letter or digit, an I, a space, a
// full stop or colon (font 1 at six tenths size).
constexpr int kTitleAdvance = 6, kTitleNarrow = 3, kTitleSpace = 4, kTitleStop = 3;

// Where a number's text is written for the game to draw: past the pictures.
constexpr uint32_t kNumberText = 0x6F6F00;
// The number lettering's advance (font 2's digits, measured from frames).
constexpr int kDigitWidth = 9;

int width_of(const uint8_t* rdram, Text t) {
    if (t.number >= 0) return kDigitWidth * (t.number >= 10 ? 2 : 1);
    return t.made ? g_pictures[t.id].ink : load32(rdram, kFont1Table + uint32_t(t.id) * 8 + 4);
}

uint32_t draw_text(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, Text t, int x, int y) {
    if (t.number >= 0) {
        char text[8];
        std::snprintf(text, sizeof text, "%d", t.number);
        for (size_t i = 0; i <= std::strlen(text); ++i) rdram[(kNumberText + uint32_t(i)) ^ 3] = uint8_t(text[i]);
        // Its scale and spacing, as the game's own callers set them first.
        store32(rdram, 0x226E24, 0x3F800000u);
        *reinterpret_cast<int16_t*>(rdram + (0x226E28 ^ 2)) = 0;
        return call(rdram, ctx, func_801E9858, { gfx, 2, 0x80000000u | kNumberText, uint32_t(x), uint32_t(y - 3) });
    }
    int id = t.id;
    if (t.made) {
        const Picture& p = g_pictures[t.id];
        store32(rdram, kFont1Table + uint32_t(kScratchId) * 8, 0x80000000u | p.at);
        store32(rdram, kFont1Table + uint32_t(kScratchId) * 8 + 4, uint32_t(p.width));
        id = kScratchId;
    }
    return call(rdram, ctx, func_801E3EE0, { gfx, 1, uint32_t(id), uint32_t(x), uint32_t(y), 0 });
}

// The first page's choices: the game's five, with FREE ROAM put in before
// OPTIONS -- a copy of STUNT MODE's, with its own word, centred as the
// game centres its words (the page's words start at x 98, its bars are
// centred on 160). Written, and the page pointed at them, every frame the
// main menu is up, since the overlay is loaded afresh with the title.
void place_main_rows(uint8_t* rdram) {
    uint32_t to = kMainRows;
    for (int row = 0; row < kGameMainRows; ++row) {
        if (row == kFreeRoamRow) {
            const uint32_t stunt = kMainChoices + uint32_t(kStuntRow * kChoiceSize);
            for (int i = 0; i < kChoiceSize; i += 4) store32(rdram, to + uint32_t(i), uint32_t(load32(rdram, stunt + uint32_t(i))));
            store32(rdram, to + 0x08, uint32_t(kFreeRoamId));
            store32(rdram, to + 0x0C, uint32_t(160 - 98 - g_pictures[kFreeRoam].ink / 2));
            to += kChoiceSize;
        }
        const uint32_t from = kMainChoices + uint32_t(row * kChoiceSize);
        for (int i = 0; i < kChoiceSize; i += 4) store32(rdram, to + uint32_t(i), uint32_t(load32(rdram, from + uint32_t(i))));
        to += kChoiceSize;
    }
    store32(rdram, kPages + 0x00, 0x80000000u | kMainRows);
    store32(rdram, kPages + 0x04, kMainRowCount);
    store32(rdram, kPages + 0x08, kMainRowCount);
}

// ---------------------------------------------------------------- options --

// A row's setting: its label, where it is kept, and the word for each of its
// values in order; or, kept elsewhere, how it is read and changed.
struct Option {
    Word label; const char* tab; const char* id; std::vector<Text> values;
    uint32_t (*get)() = nullptr;
    void (*set)(uint32_t) = nullptr;
};
uint32_t value_of(const Option& o) {
    const uint32_t v = o.get ? o.get() : menu_setting(o.tab, o.id);
    return std::min<uint32_t>(v, uint32_t(o.values.size() - 1));
}
void change(const Option& o, uint32_t to) {
    if (o.set) o.set(to);
    else set_menu_setting(o.tab, o.id, to);
}

const std::vector<Text> kWaveValues = { game(kWordNormal), made(kStill), made(kCalm), made(kGentle), made(kChoppy),
                                        made(kRough), made(kStormy), made(kWild), made(kExtreme) };
const std::vector<Text> kCourseValues = { game(kWordNormal), game(kWordCourses + 0), game(kWordCourses + 1),
                                          game(kWordCourses + 2), game(kWordCourses + 3), game(kWordCourses + 4),
                                          game(kWordCourses + 5), game(kWordCourses + 6), game(kWordCourses + 7),
                                          game(kWordCourses + 8) };
const std::vector<Text> kSwitchValues = { game(kWordOff), game(kWordOn) };

// The game's CHANGE CONDITIONS for the Championship's difficulty (see the top),
// for the WAVES page's MISSES and LAPS; set as the page opens.
uint8_t* g_ram = nullptr;
int g_difficulty = 0;
constexpr uint32_t kConditions = 0x1CB308;   // [difficulty][wave conditions, misses, laps], words
uint32_t condition(int which, int least) {
    const int32_t v = load32(g_ram, kConditions + uint32_t(g_difficulty * 12 + which * 4));
    return uint32_t(std::max(0, v - least));
}
void set_condition(int which, int least, uint32_t to) { store32(g_ram, kConditions + uint32_t(g_difficulty * 12 + which * 4), to + uint32_t(least)); }
uint32_t misses_setting() { return condition(1, 1); }
void set_misses_setting(uint32_t to) { set_condition(1, 1, to); }
uint32_t laps_setting() { return condition(2, 3); }
void set_laps_setting(uint32_t to) { set_condition(2, 3, to); }
std::vector<Text> numbers(int from, int to) {
    std::vector<Text> v;
    for (int n = from; n <= to; ++n) v.push_back(number(n));
    return v;
}
bool g_reverse = false;   // the Championship chosen is REVERSE

const std::vector<Option> kChampionshipOptions = {
    { kWaves, "races", "race_waves", kWaveValues },
    { kMisses, nullptr, nullptr, numbers(1, 10), misses_setting, set_misses_setting },
    { kLaps, nullptr, nullptr, numbers(3, 10), laps_setting, set_laps_setting },
};

const std::vector<Option> kReverseOptions = {
    { kWaves, "races", "race_waves", kWaveValues },
    { kMisses, "races", "reverse_misses", numbers(1, 10) },
    { kLaps, "races", "reverse_laps", numbers(3, 10) },
};

const std::vector<Option> kRaceOptions = {
    { kWaves, "races", "race_waves", kWaveValues },
};
const std::vector<Option> kFreeRoamOptions = {
    { kWaves, "free_roam", "free_roam_waves", kWaveValues },
    { kSky, "free_roam", "free_roam_look", kCourseValues },
    { kMusic, "free_roam", "free_roam_music", kCourseValues },
    { kFog, "free_roam", "free_roam_fog", kSwitchValues },
    { kRiders, "free_roam", "dolphin_park_riders", kSwitchValues },
};
// On Dolphin Park, its layout first: Stunt Mode's or the Warm Up's.
const std::vector<Option> kDolphinParkOptions = [] {
    std::vector<Option> options = { { kLayout, "free_roam", "dolphin_park_layout", { made(kStuntMode), made(kWarmUp) } } };
    options.insert(options.end(), kFreeRoamOptions.begin(), kFreeRoamOptions.end());
    return options;
}();

// EXTRAS, on the OPTIONS screen: the settings that change the game itself.
// (Display, window, controls and volumes stay in the settings menu.)
const std::vector<Option> kExtrasOptions = {
    { kSpeed, "graphics", "speed_units", { made(kKph), made(kMph) } },
    { kView, nullptr, nullptr, { made(kChase), made(kFirstPerson) }, view_setting, set_view_setting },
    { kTilt, nullptr, nullptr, { made(kNone), made(kHalf), made(kFull) }, tilt_setting, set_tilt_setting },
    { kFeedback, "haptics", "haptics_mode", { game(kWordOff), made(kEvents), made(kFull) } },
    { kTextures, "graphics", "texture_quality", { made(kOriginal), made(kHd) } },
    { kWater, "graphics", "water_quality", { made(kOriginal), made(kModern), made(kHigh) } },
    { kStyle, "water", "water_style", { made(kModern), made(kClassic), made(kAqua) } },
    { kRipples, "water", "water_ripples", { made(kSoft), game(kWordNormal), made(kStrong) } },
    { kSpray, "water", "water_spray", kSwitchValues },
    { kUnlockAll, "debug", "debug_unlock_all", kSwitchValues },
};

// ------------------------------------------------------------------ pages --

enum class Page { None, ChampionshipWaves, CourseWaves, FreeRoam, Extras };
Page g_page = Page::None;
int g_row = 0;
int g_saved_cursor = 0;      // the main menu's row the page was opened from
bool g_starting = false;     // CONTINUE is starting the race
bool g_dolphin_park = false; // the course select's page is for Dolphin Park

const std::vector<Option>& options_of(Page page) {
    if (page == Page::Extras) return kExtrasOptions;
    if (page == Page::ChampionshipWaves) return g_reverse ? kReverseOptions : kChampionshipOptions;
    if (page != Page::FreeRoam) return kRaceOptions;
    return g_dolphin_park ? kDolphinParkOptions : kFreeRoamOptions;
}
int rows_of(Page page) { return int(options_of(page).size()) + 1; }

// Where a page sits: on the main menu as the game's pages do (bars x 90-230,
// words from 98, rows 20 apart from 78; its caller backs each row); on the
// course select, wider for the courses' names, centred, over a dark box; on
// the OPTIONS screen, as wide, in its rows' place and its title's.
struct Layout { int bar_x, bar_w, label_x, value_right, top, step; bool backdrop; };
Layout layout_of(Page page) {
    if (page == Page::ChampionshipWaves) return { 90, 140, 98, 224, 78, 20, false };
    if (page == Page::Extras) return { 40, 240, 52, 268, 24, 17, false };
    return { 40, 240, 52, 268, 120 - rows_of(page) * 10, 20, true };
}

uint32_t draw_row(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, const Layout& at, int row) {
    const auto& options = options_of(g_page);
    const int y = at.top + row * at.step + 1;
    if (row == int(options.size())) {
        const Text last = game(g_page == Page::Extras ? kWordEnd : kWordContinue);
        return draw_text(rdram, ctx, gfx, last, at.bar_x + at.bar_w / 2 - width_of(rdram, last) / 2, y);
    }
    const Option& o = options[size_t(row)];
    gfx = draw_text(rdram, ctx, gfx, made(o.label), at.label_x, y);
    const Text t = o.values[value_of(o)];
    return draw_text(rdram, ctx, gfx, t, at.value_right - width_of(rdram, t), y);
}

uint32_t colour(uint8_t* rdram, recomp_context* ctx, uint32_t gfx, int style) {
    return call(rdram, ctx, func_801E946C, { gfx, uint32_t(style), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
}

uint32_t draw_page(uint8_t* rdram, recomp_context* ctx, uint32_t gfx) {
    place_pictures(rdram);
    const Layout at = layout_of(g_page);
    const int rows = rows_of(g_page);
    // Dark enough that the course list under the page's see-through bars
    // doesn't read through them.
    if (at.backdrop) {
        gfx = call(rdram, ctx, func_801E7C58, { gfx, 8, 20, 311, 219, 0, 0, 0, 0xE0 });
        // And solid behind each see-through bar.
        for (int row = 0; row < rows; ++row) {
            const int y = at.top + row * at.step - 2;
            gfx = call(rdram, ctx, func_801E7C58, { gfx, uint32_t(at.bar_x + 2), uint32_t(y + 1), uint32_t(at.bar_x + at.bar_w - 3),
                                                    uint32_t(y + 13), 0, 0, 0, 0xFF });
        }
    }
    store32(rdram, phys(gfx), 0x06000000u);
    store32(rdram, phys(gfx) + 4, kBarSetup);
    gfx += 8;
    for (int row = 0; row < rows; ++row) {
        gfx = call(rdram, ctx, func_80094338, { gfx, 0, uint32_t(at.bar_x), uint32_t(at.top + row * at.step - 2), uint32_t(at.bar_w), 0xF });
    }
    gfx = colour(rdram, ctx, gfx, kStyleNormal);
    for (int row = 0; row < rows; ++row) {
        if (row != g_row) gfx = draw_row(rdram, ctx, gfx, at, row);
    }
    gfx = colour(rdram, ctx, gfx, kStyleChosen);
    gfx = draw_row(rdram, ctx, gfx, at, g_row);
    gfx = call(rdram, ctx, func_801E5554, { gfx });
    if (g_page == Page::ChampionshipWaves) {
        // As the game's page draw leaves them: its caller backs each row, and
        // the markers either side of the chosen row follow the game's cursor.
        store32(rdram, kCursor, uint32_t(g_row));
        store32(rdram, kRowsDrawn, uint32_t(rows));
        store32(rdram, kRowsChosen, uint32_t(rows));
        store32(rdram, kTopRow, uint32_t(at.top));
    }
    return gfx;
}

void open(uint8_t* rdram, recomp_context* ctx, Page page) {
    g_page = page;
    g_row = 0;
    g_dolphin_park = chosen_course(rdram) == 0;
    std::fprintf(stderr, "[wr64] game menus: page %d opened\n", int(page));
    if (page == Page::ChampionshipWaves) store32(rdram, kIdle, 0);
    else if (page != Page::Extras) swallow_sound_once(kSoundStart);
    play(rdram, ctx, kSoundChoose);
}

// The page's controls. Returns true when CONTINUE is chosen (the page is
// then closed); B closes it too.
bool page_controls(uint8_t* rdram, recomp_context* ctx, uint16_t buttons) {
    const int rows = rows_of(g_page);
    if (buttons & kB) {
        g_page = Page::None;
        swallow_sound_once(-1);
        play(rdram, ctx, kSoundBack);
        return false;
    }
    if (buttons & (kUp | kDown)) {
        g_row = (g_row + ((buttons & kUp) ? rows - 1 : 1)) % rows;
        play(rdram, ctx, kSoundMove);
        return false;
    }
    if (g_row == rows - 1) {
        if ((buttons & (kA | kZ | kStart)) == 0) return false;
        g_page = Page::None;
        swallow_sound_once(-1);
        return true;
    }
    if (buttons & (kLeft | kRight | kA | kZ)) {
        const Option& o = options_of(g_page)[size_t(g_row)];
        const uint32_t count = uint32_t(o.values.size());
        const uint32_t now = value_of(o);
        change(o, (buttons & kLeft) ? (now + count - 1) % count : (now + 1) % count);
        play(rdram, ctx, kSoundMove);
    }
    return false;
}

// Runs the main menu's own controls; if they end the menu, the run is a free
// roam when the row they ended it on is FREE ROAM.
void game_controls(uint8_t* rdram, recomp_context* ctx) {
    const int cursor = load32(rdram, kCursor);
    func_i0_802C63AC(rdram, ctx);
    if (load32(rdram, kGameState) == 0x04) set_free_roam(load32(rdram, kPage) == 0 && cursor == kFreeRoamRow);
}

// Whether choosing the main menu's current row would end it on a
// Championship: the page's choice opens no other page and is Championship's.
// The choice there (physical).
uint32_t main_choice(const uint8_t* rdram) {
    const uint32_t items = phys(gpr(int64_t(load32(rdram, kPages + uint32_t(load32(rdram, kPage)) * 0x18))));
    return items + uint32_t(load32(rdram, kCursor)) * kChoiceSize;
}
bool ends_championship(const uint8_t* rdram) {
    const uint32_t choice = main_choice(rdram);
    const int16_t next = int16_t(uint32_t(load32(rdram, choice)) >> 16);
    return next == -1 && load32(rdram, choice + 4) == kChampionship;
}

// On the Free Roam options' MUSIC row, the music shown is heard (the
// course's own: the course chosen's), a course's being its sequence + 6 as
// the game numbers them.
void preview_music(const uint8_t* rdram) {
    if (g_page != Page::FreeRoam || g_row >= int(options_of(g_page).size())) return;
    const Option& o = options_of(g_page)[size_t(g_row)];
    if (o.label != kMusic) return;
    const uint32_t value = menu_setting(o.tab, o.id);
    const int course = value == 0 ? chosen_course(rdram) : int(value) - 1;
    music::preview(course + 6);
}

// ---------------------------------------------------------------- options --

int g_options_row = 0;   // the OPTIONS screen's row, EXTRAS among them

// The OPTIONS screen's rows (or EXTRAS, when open), drawn as the game draws
// them (func_i7_802C5A18): its setup, a bar a row, the title (font 2's 5),
// the words, the chosen one in style 6.
uint32_t draw_options(uint8_t* rdram, recomp_context* ctx, uint32_t gfx) {
    place_pictures(rdram);
    const uint32_t setup[][2] = { { 0x01030040u, 0x07000000u }, { 0x01020040u, 0x02000A40u }, { 0x06000000u, 0x0106F550u } };
    for (const auto& c : setup) {
        store32(rdram, phys(gfx), c[0]);
        store32(rdram, phys(gfx) + 4, c[1]);
        gfx += 8;
    }
    if (g_page == Page::Extras) return draw_page(rdram, ctx, gfx);
    store32(rdram, phys(gfx), 0x06000000u);
    store32(rdram, phys(gfx) + 4, kBarSetup);
    gfx += 8;
    for (int row = 0; row < kOptionsRowCount; ++row) {
        gfx = call(rdram, ctx, func_80094338, { gfx, 0, 0x3A, uint32_t(kOptionsTop + row * kOptionsStep), 0xCC, 0x10 });
    }
    gfx = colour(rdram, ctx, gfx, kStyleNormal);
    gfx = call(rdram, ctx, func_801E3EE0, { gfx, 2, 5, 0x6B, 0x22, 0 });
    const auto word = [&](int row, uint32_t at) {
        const int y = kOptionsTop + row * kOptionsStep + 3;
        if (row == kExtrasRow) return draw_text(rdram, ctx, at, made(kExtras), 160 - g_pictures[kExtras].ink / 2, y);
        const GameRow& r = kOptionRows[row < kExtrasRow ? row : row - 1];
        return draw_text(rdram, ctx, at, game(r.word), r.x + 0x6D, y);
    };
    for (int row = 0; row < kOptionsRowCount; ++row) {
        if (row != g_options_row) gfx = word(row, gfx);
    }
    gfx = colour(rdram, ctx, gfx, kStyleChosen);
    return word(g_options_row, gfx);
}

}  // namespace

// func_i7_802C5A18 in its section's table: the OPTIONS screen's draw.
void game_menus_options_draw_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!enabled()) { func_i7_802C5A18(rdram, ctx); return; }
    ctx->r2 = gpr(int64_t(int32_t(draw_options(rdram, ctx, uint32_t(ctx->r4)))));
}

// func_i7_802C5800 in its section's table: the OPTIONS screen's frame. Up
// and down move over its eight rows; EXTRAS opens the page; the game's own
// rows, and B, are the game's, on its row.
void game_menus_options_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!enabled()) { func_i7_802C5800(rdram, ctx); return; }
    const uint32_t state = uint32_t(load32(rdram, kGameState));
    if (state != kOptionsState || load32(rdram, kFirstFrame) != 0) {
        if (state == kOptionsState && load32(rdram, kPrevState) == 0x04) { g_options_row = 0; g_page = Page::None; }
        func_i7_802C5800(rdram, ctx);
        return;
    }
    const uint16_t buttons = load16(rdram, kButtons);
    const uint32_t gfx = uint32_t(ctx->r4);   // before any call of the game's changes a0
    if (g_page == Page::Extras) {
        if (page_controls(rdram, ctx, buttons)) play(rdram, ctx, kSoundBack);
    } else if ((buttons & (kA | kZ | kStart)) != 0 && g_options_row == kExtrasRow) {
        open(rdram, ctx, Page::Extras);
    } else if ((buttons & (kA | kZ | kStart | kB)) != 0) {
        store32(rdram, kOptionsItem, uint32_t(g_options_row < kExtrasRow ? g_options_row : g_options_row - 1));
        func_i7_802C5800(rdram, ctx);
        return;
    } else if ((buttons & (kUp | kDown)) != 0) {
        g_options_row = (g_options_row + ((buttons & kUp) ? kOptionsRowCount - 1 : 1)) % kOptionsRowCount;
        play(rdram, ctx, kSoundMove);
    }
    store32(rdram, kOptionsFlag, 0);
    ctx->r2 = gpr(int64_t(int32_t(draw_options(rdram, ctx, gfx))));
}

// func_i4_802C5800 in its section's table: the course select's setup (and
// the next state's, as a race loads). Coming back from a free roam's Warm
// Up, it is Stunt Mode's again.
void game_menus_course_init_hook(uint8_t* rdram, recomp_context* ctx) {
    if (enabled() && uint32_t(load32(rdram, kGameState)) == kCourseSelectState) leave_warm_up(rdram);
    func_i4_802C5800(rdram, ctx);
}

// func_i0_802C6044 in its section's table: the main menu's page draw.
// func_i0_802C5A7C in its section's table: the title's frame (state 0x03).
// While its logo is up (D_i0_802C6BE0 0, before the menu), the edition's
// version goes under "(C) 1996 Nintendo", in the game's menu lettering
// (func_801E9858, font 1: capitals, digits and full stops) at six tenths
// size: "VERSION 1.1.0: LATEST VERSION", the status in green (style 6), or
// "...: NEWER VERSION AVAILABLE" in red (style 4) (src/updates.cpp); the
// version alone while that isn't known.
void game_menus_title_hook(uint8_t* rdram, recomp_context* ctx) {
    func_i0_802C5A7C(rdram, ctx);
    if (!enabled() || load32(rdram, kGameState) != 0x03 || load32(rdram, 0x2C6BE0) != 0) return;
    std::string version = "VERSION ";
    for (const char* c = updates::kVersion; *c && *c != '-'; ++c) version += *c;
    const bool known = updates::known(), newer = known && updates::available();
    const std::string status = !known ? "" : newer ? "NEWER VERSION AVAILABLE" : "LATEST VERSION";
    // Each character on its own, an even step apart (the game's own spacing
    // squeezes the letters together at this size); font 1 has no colon, so
    // two of its full stops, one above the other.
    constexpr float kScale = 0.6f;
    uint32_t scale;
    std::memcpy(&scale, &kScale, 4);
    const std::string line = version + (known ? ": " + status : "");
    int width = 0;
    const auto step = [](char c) { return c == ' ' ? kTitleSpace : c == '.' || c == ':' ? kTitleStop : c == 'I' ? kTitleNarrow : kTitleAdvance; };
    for (char c : line) width += step(c);
    int x = 160 - width / 2;
    const int y = 200;
    uint32_t gfx = uint32_t(ctx->r2);
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c != ' ') {
            const int style = known && i > version.size() ? (newer ? 4 : kStyleChosen) : kStyleNormal;
            gfx = call(rdram, ctx, func_801E946C, { gfx, uint32_t(style), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
            for (int dot = 0; dot < (c == ':' ? 2 : 1); ++dot) {
                rdram[kNumberText ^ 3] = uint8_t(c == ':' ? '.' : c);
                rdram[(kNumberText + 1) ^ 3] = 0;
                store32(rdram, 0x226E24, scale);
                *reinterpret_cast<int16_t*>(rdram + (0x226E28 ^ 2)) = 1;
                gfx = call(rdram, ctx, func_801E9858, { gfx, 1, 0x80000000u | kNumberText, uint32_t(x), uint32_t(y - dot * 3) });
            }
        }
        x += step(c);
    }
    store32(rdram, 0x226E24, 0x3F800000u);
    *reinterpret_cast<int16_t*>(rdram + (0x226E28 ^ 2)) = 0;
    gfx = call(rdram, ctx, func_801E5554, { gfx });
    ctx->r2 = gpr(int64_t(int32_t(gfx)));
}

// A newer release out (src/updates.cpp) is said under the main menu's rows.
void game_menus_draw_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!enabled()) { func_i0_802C6044(rdram, ctx); return; }
    place_pictures(rdram);
    place_main_rows(rdram);
    uint32_t gfx;
    if (g_page != Page::ChampionshipWaves) {
        func_i0_802C6044(rdram, ctx);
        gfx = uint32_t(ctx->r2);
    } else {
        gfx = draw_page(rdram, ctx, uint32_t(ctx->r4));
    }
    if (updates::available()) {
        gfx = colour(rdram, ctx, gfx, kStyleChosen);
        gfx = draw_text(rdram, ctx, gfx, made(kUpdateAvailable), 160 - g_pictures[kUpdateAvailable].ink / 2, 200);
        gfx = call(rdram, ctx, func_801E5554, { gfx });
    }
    ctx->r2 = gpr(int64_t(int32_t(gfx)));
}

// func_i0_802C63AC in its section's table: the main menu's controls.
void game_menus_input_hook(uint8_t* rdram, recomp_context* ctx) {
    if (!enabled()) { func_i0_802C63AC(rdram, ctx); return; }
    const uint16_t buttons = load16(rdram, kButtons);
    if (g_page != Page::ChampionshipWaves) {
        g_page = Page::None;
        if ((buttons & (kA | kZ | kStart)) != 0 && ends_championship(rdram)) {
            g_saved_cursor = load32(rdram, kCursor);
            // The choice's difficulty (+0x14) and special (+0x16; 4 REVERSE).
            const uint32_t choice = main_choice(rdram);
            g_ram = rdram;
            g_difficulty = std::clamp<int>(int16_t(load16(rdram, choice + 0x14)), 0, 2);
            g_reverse = int16_t(load16(rdram, choice + 0x16)) == 4;
            open(rdram, ctx, Page::ChampionshipWaves);
            return;
        }
        game_controls(rdram, ctx);
        return;
    }
    if (buttons != 0) store32(rdram, kIdle, 0);
    const bool go = page_controls(rdram, ctx, buttons);
    if (g_page == Page::ChampionshipWaves) {
        store32(rdram, kCursor, uint32_t(g_row));
        return;
    }
    // Closed: back on the row it was opened from, and if CONTINUE, chosen.
    store32(rdram, kCursor, uint32_t(g_saved_cursor));
    if (go) game_controls(rdram, ctx);
}

// func_i4_802C7474 in its section's table: the course select's way out, to
// state a0. Starting a race there opens the page instead, once.
void game_menus_course_exit_hook(uint8_t* rdram, recomp_context* ctx) {
    if (enabled() && !g_starting && uint32_t(ctx->r4) == kStartRace &&
        uint32_t(load32(rdram, kGameState)) == kCourseSelectState) {
        const int32_t mode = load32(rdram, kChosenMode);
        if (mode == kStunt && free_roam_chosen()) { open(rdram, ctx, Page::FreeRoam); return; }
        if (mode == kTimeTrials || mode == kStunt || mode == kVersus) { open(rdram, ctx, Page::CourseWaves); return; }
    }
    func_i4_802C7474(rdram, ctx);
}

// func_i4_802C6BAC in its section's table: the course select's controls,
// the page's while it is up.
void game_menus_course_input_hook(uint8_t* rdram, recomp_context* ctx) {
    if (g_page != Page::CourseWaves && g_page != Page::FreeRoam) { func_i4_802C6BAC(rdram, ctx); return; }
    const uint32_t pad = uint32_t(std::clamp(load32(rdram, kCourseSelectPad), 0, 3));
    const uint16_t buttons = load16(rdram, kButtons + pad * kControllerSize);
    const bool roam = g_page == Page::FreeRoam;
    if (!page_controls(rdram, ctx, buttons)) return;
    // Dolphin Park's Warm Up: the championship's, ridden as a free roam.
    if (roam && g_dolphin_park && menu_setting("free_roam", "dolphin_park_layout") == 1) start_warm_up(rdram);
    g_starting = true;
    call(rdram, ctx, func_i4_802C7474, { kStartRace });
    g_starting = false;
    play(rdram, ctx, kSoundStart);
}

// func_i4_802C5B78 in its section's table: the course select's frame, the
// page drawn over it while it is up.
void game_menus_course_draw_hook(uint8_t* rdram, recomp_context* ctx) {
    func_i4_802C5B78(rdram, ctx);
    preview_music(rdram);
    if (g_page != Page::CourseWaves && g_page != Page::FreeRoam) return;
    if (uint32_t(load32(rdram, kGameState)) != kCourseSelectState) { g_page = Page::None; return; }
    ctx->r2 = gpr(int64_t(int32_t(draw_page(rdram, ctx, uint32_t(ctx->r2)))));
}

}  // namespace wr64

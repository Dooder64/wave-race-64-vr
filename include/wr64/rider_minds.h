#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

// What the Free Roam riders feel like doing (src/rider_minds.cpp,
// docs/RIDER_PERSONALITIES.md). Pure decision-making: src/companions.cpp
// rides whatever is chosen.
namespace wr64::minds {

// What a rider wants: each 0 (had plenty) to 1 (longing for it).
enum Want { kThrill, kSights, kCompany, kShowOff, kRest, kCuriosity, kWants };

// What a rider can be doing. The first rider's routes (Stunts, Lap,
// Practice), the pair's routines (Sights, Weave, Trick), and either's
// Follow (riding along with the player), Rest (drifting somewhere quiet) and
// Watch (gone to see something that caught its eye).
enum class Activity { Stunts, Lap, Practice, Sights, Weave, Trick, Follow, Rest, Watch };
const char* name(Activity a);

// Who a rider is: how much each want counts with it, and how quickly it
// tires of what it's doing.
struct Personality {
    const char* name;
    float weight[kWants];
    float patience;     // seconds an activity holds it, on average
};
const std::vector<Personality>& personalities();

// Something worth a look: where, how striking (0-1), and when it stops
// being news.
struct Interest {
    float x = 0, z = 0, strength = 0;
    double until = 0;   // seconds, on the minds' clock
    std::string what;
};

struct Mind {
    const Personality* who = nullptr;
    float want[kWants] = {};
    Activity doing = Activity::Lap;
    double since = 0, until = 0;    // when it started, and when it may tire of it
    // What it's watching, if anything.
    Interest watching;
    // How it feels about the player: warms with time spent together, and
    // a show-off who keeps being outdone starts challenging.
    float fondness = 0.5f;
    // When it last went to see something: news doesn't pull it away again
    // straight after.
    double noticed_at = -1e9;
};

// What a rider could do now, and how far away the player is (or < 0 if
// not about).
struct Options {
    std::vector<Activity> open;
    float player_distance = -1.0f;
    bool player_moving = false;
};

// Starts a mind: personality `who` (or by chance, if null), wants by chance.
void start(Mind& m, const Personality* who, std::mt19937& chance, double now);
// Time passing: the wants of what it isn't doing rise, what it is doing
// satisfies its own.
void feel(Mind& m, double now, float dt);
// Whether it is time to choose again: tired of the activity, or something
// more interesting has come up (then `watching` is set).
bool restless(const Mind& m, double now);
// Picks the next activity from `options`, best by its wants and who it is,
// with some chance in it; starts it.
Activity choose(Mind& m, const Options& options, std::mt19937& chance, double now);
// Something noticed: whether it's interesting enough to go and see now,
// given how far away it is. Sets `watching` if so.
bool notice(Mind& m, const Interest& interest, float distance, std::mt19937& chance, double now);

// A rider's personality for a scripted test: WR64_COMPANION_TEST_MINDS=
// <first>,<pair>, names from personalities() (e.g. daredevil,social).
const Personality* test_personality(int which);

}  // namespace wr64::minds

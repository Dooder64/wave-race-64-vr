// What the Free Roam riders feel like doing.
//
// Each rider (the pair counting as one: they go everywhere together) has a
// personality -- how much each of six wants counts with it: thrills, sights,
// company, showing off, rest and curiosity -- and the wants themselves, each
// 0 to 1. Whatever it is doing satisfies some of them (kSatisfies) and they
// fall; the rest rise, faster the more they count with it. When it tires of
// an activity (after about its personality's patience), it scores each one
// open to it by how well it meets what it wants now and who it is, and
// takes the best, with some chance in it and the one it just did counted
// down. So a daredevil mostly jumps, but even it drifts somewhere quiet now
// and then; a social rider comes and rides along with the player for a
// while, then peels off.
//
// Things worth a look (Interest: the player's big air, so far) are noticed
// as they happen: the more curious the rider, the nearer and the more
// striking, the likelier it drops what it's doing and goes to watch -- but
// not again straight after. A show-off who sees the player's big air wants
// to show off all the more.
//
// src/companions.cpp asks, rides what is chosen, and reports what it sees.

#include "wr64/rider_minds.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace wr64::minds {
namespace {

// How much each activity satisfies each want (thrill, sights, company,
// showing off, rest, curiosity).
constexpr float kSatisfies[][kWants] = {
    { 1.0f, 0.0f, 0.0f, 0.6f, 0.0f, 0.0f },   // Stunts: the ramp, again and again
    { 0.2f, 0.6f, 0.0f, 0.0f, 0.3f, 0.1f },   // Lap: a cruise round the park's edge
    { 0.9f, 0.0f, 0.0f, 0.4f, 0.0f, 0.0f },   // Practice: Stunt Mode's laps
    { 0.0f, 1.0f, 0.2f, 0.0f, 0.6f, 0.3f },   // Sights
    { 0.5f, 0.2f, 0.6f, 0.4f, 0.0f, 0.0f },   // Weave
    { 1.0f, 0.0f, 0.3f, 1.0f, 0.0f, 0.0f },   // Trick
    { 0.2f, 0.0f, 1.0f, 0.4f, 0.0f, 0.4f },   // Follow the player
    { 0.0f, 0.2f, 0.0f, 0.0f, 1.0f, 0.0f },   // Rest
    { 0.0f, 0.2f, 0.2f, 0.0f, 0.3f, 1.0f },   // Watch
};

float uniform(std::mt19937& chance, float lo, float hi) {
    return lo + (hi - lo) * float(chance() % 100000u) / 100000.0f;
}

}  // namespace

const char* name(Activity a) {
    switch (a) {
    case Activity::Stunts: return "stunt runs";
    case Activity::Lap: return "a lap of the park";
    case Activity::Practice: return "practice laps";
    case Activity::Sights: return "sightseeing";
    case Activity::Weave: return "weaving";
    case Activity::Trick: return "a jump together";
    case Activity::Follow: return "riding along with the player";
    case Activity::Rest: return "a rest";
    case Activity::Watch: return "watching";
    }
    return "?";
}

const std::vector<Personality>& personalities() {
    //                                     thrill sights company show-off rest curiosity  patience
    static const std::vector<Personality> all = {
        { "daredevil", { 1.0f, 0.1f, 0.2f, 0.7f, 0.1f, 0.4f }, 70.0f },
        { "sightseer", { 0.2f, 1.0f, 0.3f, 0.1f, 0.6f, 0.7f }, 60.0f },
        { "social",    { 0.4f, 0.4f, 1.0f, 0.3f, 0.3f, 0.6f }, 50.0f },
        { "loner",     { 0.5f, 0.7f, 0.0f, 0.1f, 0.7f, 0.2f }, 80.0f },
        { "show-off",  { 0.7f, 0.1f, 0.6f, 1.0f, 0.2f, 0.5f }, 45.0f },
        { "practiser", { 0.8f, 0.2f, 0.2f, 0.3f, 0.3f, 0.2f }, 120.0f },
    };
    return all;
}

void start(Mind& m, const Personality* who, std::mt19937& chance, double now) {
    m = Mind{};
    const auto& all = personalities();
    m.who = who != nullptr ? who : &all[chance() % all.size()];
    for (float& w : m.want) w = uniform(chance, 0.2f, 0.8f);
    m.since = now;
    m.until = now;
}

void feel(Mind& m, double now, float dt) {
    (void)now;
    if (m.who == nullptr) return;
    const float* satisfies = kSatisfies[int(m.doing)];
    for (int i = 0; i < kWants; ++i) {
        // Satisfied in about 40 seconds of a perfect match; longing again
        // in about two minutes for what counts most with it.
        if (satisfies[i] > 0.0f) m.want[i] -= satisfies[i] * dt / 40.0f;
        else m.want[i] += (0.15f + m.who->weight[i]) * dt / 140.0f;
        m.want[i] = std::clamp(m.want[i], 0.0f, 1.0f);
    }
    // Time with the player warms it to them.
    if (m.doing == Activity::Follow || m.doing == Activity::Watch) m.fondness = std::min(1.0f, m.fondness + dt / 300.0f);
}

bool restless(const Mind& m, double now) { return now >= m.until; }

Activity choose(Mind& m, const Options& options, std::mt19937& chance, double now) {
    Activity best = options.open.empty() ? m.doing : options.open[0];
    float best_score = -1e9f;
    for (Activity a : options.open) {
        if (a == Activity::Watch) continue;     // only ever by noticing something
        if (a == Activity::Follow && (options.player_distance < 0.0f || options.player_distance > 9000.0f || !options.player_moving))
            continue;
        float score = 0.0f;
        for (int i = 0; i < kWants; ++i) score += m.who->weight[i] * m.want[i] * kSatisfies[int(a)][i];
        if (a == Activity::Follow) score *= 0.5f + m.fondness;
        if (a == m.doing) score *= 0.35f;       // something else for a change
        score += uniform(chance, 0.0f, 0.25f);
        if (score > best_score) {
            best_score = score;
            best = a;
        }
    }
    m.doing = best;
    m.since = now;
    float length = m.who->patience * uniform(chance, 0.6f, 1.4f);
    if (best == Activity::Follow) length = uniform(chance, 20.0f, 60.0f) * (0.6f + m.who->weight[kCompany]);
    if (best == Activity::Rest) length = uniform(chance, 15.0f, 40.0f) * (0.6f + m.who->weight[kRest]);
    // WR64_COMPANION_TEST_CHANGE=<seconds>, in a scripted run only.
    if (const char* test = std::getenv("WR64_COMPANION_TEST_CHANGE"); test != nullptr && std::getenv("WR64_INPUT_SCRIPT") != nullptr)
        length = float(std::atof(test));
    m.until = now + length;
    return best;
}

bool notice(Mind& m, const Interest& interest, float distance, std::mt19937& chance, double now) {
    if (m.who == nullptr || now >= interest.until) return false;
    // A show-off who sees someone else's big moment wants the spotlight back.
    if (m.who->weight[kShowOff] >= 0.7f) m.want[kShowOff] = std::min(1.0f, m.want[kShowOff] + 0.15f * interest.strength);
    if (now - m.noticed_at < 25.0 || m.doing == Activity::Watch || m.doing == Activity::Trick) return false;
    const float near = std::clamp(1.0f - (distance - 2000.0f) / 7000.0f, 0.0f, 1.0f);
    const float pull = interest.strength * near * (0.3f + m.who->weight[kCuriosity]) * (0.5f + m.want[kCuriosity]);
    if (pull + uniform(chance, 0.0f, 0.2f) < 0.6f) return false;
    m.watching = interest;
    m.noticed_at = now;
    m.doing = Activity::Watch;
    m.since = now;
    m.until = now + uniform(chance, 8.0f, 18.0f);
    return true;
}

const Personality* test_personality(int which) {
    const char* value = std::getenv("WR64_INPUT_SCRIPT") != nullptr ? std::getenv("WR64_COMPANION_TEST_MINDS") : nullptr;
    if (value == nullptr) return nullptr;
    const char* at = value;
    for (int i = 0; i < which && at != nullptr; ++i) {
        at = std::strchr(at, ',');
        if (at != nullptr) ++at;
    }
    if (at == nullptr) return nullptr;
    for (const Personality& p : personalities()) {
        const size_t n = std::strlen(p.name);
        if (std::strncmp(at, p.name, n) == 0 && (at[n] == '\0' || at[n] == ',')) return &p;
    }
    return nullptr;
}

}  // namespace wr64::minds

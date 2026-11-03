// The decisions journal: what the Free Roam riders and the dolphin decide,
// why, and how it turns out, for looking back at an outing -- a moment that
// was fun or dull, or something odd -- and for tuning their minds from what
// actually happened.
//
// One JSON object a line, appended to decisions.jsonl beside the log
// (%LOCALAPPDATA%\WaveRace64Recomp), kept from one game to the next. Each
// line has "utc" (to the millisecond, to match a mark's or a screenshot's
// time), "t" (seconds since the game started), "session" (when this game
// started) and "event"; the rest is the event's own (see the events'
// writers, src/companions.cpp and src/rider_minds.cpp's notes).
//
// On by default; the Debug tab's "Record riders' decisions" turns it off.

#include "wr64/journal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace wr64::journal {
namespace {

std::atomic<bool> g_on{ true };
std::mutex g_writing;

std::string utc(std::chrono::system_clock::time_point when) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()).count() % 1000;
    std::tm t{};
#if defined(_WIN32)
    gmtime_s(&t, &seconds);
#else
    gmtime_r(&seconds, &t);
#endif
    char text[40];
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                  t.tm_min, t.tm_sec, ms);
    return text;
}

void quoted(std::string& out, const char* value) {
    out += '"';
    for (const char* c = value; *c != '\0'; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (*c == '\n') { out += "\\n"; continue; }
        out += *c;
    }
    out += '"';
}

}  // namespace

void set_on(bool on) { g_on.store(on, std::memory_order_relaxed); }
bool on() {
    static const bool off = [] {
        const char* value = std::getenv("WR64_DECISIONS");
        return value != nullptr && value[0] == '0';
    }();
    return !off && g_on.load(std::memory_order_relaxed);
}

void Fields::name(const char* n) {
    text_ += ',';
    quoted(text_, n);
    text_ += ':';
}
Fields& Fields::add(const char* n, double value) {
    name(n);
    char number[32];
    std::snprintf(number, sizeof number, std::isfinite(value) ? "%.3g" : "null", value);
    // Places and seconds read better whole, or to a tenth.
    if (std::isfinite(value) && std::fabs(value) >= 100.0) std::snprintf(number, sizeof number, "%.0f", value);
    else if (std::isfinite(value) && std::fabs(value) >= 1.0) std::snprintf(number, sizeof number, "%.2f", value);
    text_ += number;
    return *this;
}
Fields& Fields::add(const char* n, int value) {
    name(n);
    text_ += std::to_string(value);
    return *this;
}
Fields& Fields::add(const char* n, bool value) {
    name(n);
    text_ += value ? "true" : "false";
    return *this;
}
Fields& Fields::add(const char* n, const char* value) {
    name(n);
    quoted(text_, value);
    return *this;
}
Fields& Fields::raw(const char* n, const std::string& json) {
    name(n);
    text_ += json;
    return *this;
}

void write(const char* event, const Fields& fields) {
    if (!on()) return;
    using Clock = std::chrono::system_clock;
    static const Clock::time_point started = Clock::now();
    static const std::string session = utc(started);
    const Clock::time_point now = Clock::now();
    std::string line = "{\"utc\":\"" + utc(now) + "\"";
    char t[32];
    std::snprintf(t, sizeof t, ",\"t\":%.2f", std::chrono::duration<double>(now - started).count());
    line += t;
    line += ",\"session\":\"" + session + "\",\"event\":";
    quoted(line, event);
    line += fields.text();
    line += "}\n";
    const char* local_app = std::getenv("LOCALAPPDATA");
    if (local_app == nullptr) return;
    std::lock_guard<std::mutex> lock(g_writing);
    if (std::FILE* f = std::fopen((std::string(local_app) + "\\WaveRace64Recomp\\decisions.jsonl").c_str(), "a")) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
}

}  // namespace wr64::journal

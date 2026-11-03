#pragma once

#include <string>

// The decisions journal (src/journal.cpp): what the Free Roam riders and the
// dolphin decide, why, and how it turns out, one JSON object a line.
namespace wr64::journal {

// The settings' switch (Debug tab).
void set_on(bool on);
bool on();

// A line's fields, written as JSON as they are added.
class Fields {
public:
    Fields& add(const char* name, double value);
    Fields& add(const char* name, int value);
    Fields& add(const char* name, bool value);
    Fields& add(const char* name, const char* value);
    Fields& add(const char* name, const std::string& value) { return add(name, value.c_str()); }
    // A value already written as JSON (an object or a list).
    Fields& raw(const char* name, const std::string& json);
    const std::string& text() const { return text_; }

private:
    void name(const char* n);
    std::string text_;
};

// Adds a line: the time (UTC, and seconds since the game started), the
// event's name, and its fields.
void write(const char* event, const Fields& fields);

}  // namespace wr64::journal

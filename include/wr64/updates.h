#pragma once

#include <string>

// The VR edition's version, and whether a newer one is out (src/updates.cpp).
namespace wr64::updates {

// This build's release, as its GitHub tag reads without the "v" (bump it
// with each release).
inline constexpr char kVersion[] = "1.3.0-vr";

// Asks GitHub once, on a thread of its own, for the latest release of
// Dooder64/wave-race-64-vr. Nothing is sent but the request itself.
void check();
// Whether a newer release than this one was found, and its tag.
bool available();
// Whether the latest release is known (the check is on and GitHub answered).
bool known();
std::string latest();
// Opens the latest release's page in the browser.
void open_page();

}  // namespace wr64::updates

// Whether a newer release of the VR edition is out.
//
// Once at start, if the settings' "Check for updates" is on (src/frontend.cpp),
// a thread asks GitHub's API for the latest release of Dooder64/wave-race-64-vr
// (one HTTPS GET, no data of the player's) and compares its tag, "v1.2.0-vr",
// with this build's kVersion number by number. A newer one shows UPDATE
// AVAILABLE on the main menu (src/game_menus.cpp) and is logged; the settings'
// "Open the download page" opens its page. Nothing is downloaded.
//
// WR64_UPDATE_TEST=<tag> pretends that tag is the latest, without asking.

#include "wr64/updates.h"

#include <SDL.h>
#include <json/json.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

namespace wr64::updates {
namespace {

constexpr char kPage[] = "https://github.com/Dooder64/wave-race-64-vr/releases/latest";

std::atomic<bool> g_available{ false };
std::mutex g_mutex;
std::string g_latest;

// "v1.2.0-vr" or "1.2.0-vr" as three numbers; false if it isn't one.
bool parse(const std::string& tag, std::array<int, 3>& out) {
    const char* p = tag.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    return std::sscanf(p, "%d.%d.%d", &out[0], &out[1], &out[2]) == 3;
}

void found(const std::string& tag) {
    std::array<int, 3> mine{}, theirs{};
    if (!parse(kVersion, mine) || !parse(tag, theirs)) {
        std::fprintf(stderr, "[wr64] updates: can't read the latest release's tag \"%s\"\n", tag.c_str());
        return;
    }
    {
        std::lock_guard lock(g_mutex);
        g_latest = tag;
    }
    g_available.store(theirs > mine, std::memory_order_relaxed);
    std::fprintf(stderr, "[wr64] updates: the latest release is %s, this is %s%s\n", tag.c_str(), kVersion,
                 theirs > mine ? " -- an update is available" : "");
}

#if defined(_WIN32)
// The latest release's JSON, or empty.
std::string fetch() {
    std::string body;
    HINTERNET session = WinHttpOpen(L"WaveRace64Recomp-VR", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return body;
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection == nullptr ? nullptr :
        WinHttpOpenRequest(connection, L"GET", L"/repos/Dooder64/wave-race-64-vr/releases/latest", nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request != nullptr &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        for (DWORD available = 0; status == 200 && WinHttpQueryDataAvailable(request, &available) && available > 0;) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) break;
            body.append(chunk.data(), read);
            if (body.size() > (1u << 20)) break;
        }
        if (status != 200) std::fprintf(stderr, "[wr64] updates: GitHub answered %lu\n", static_cast<unsigned long>(status));
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return body;
}
#else
std::string fetch() { return {}; }
#endif

}  // namespace

void check() {
    if (const char* test = std::getenv("WR64_UPDATE_TEST")) {
        found(test);
        return;
    }
    std::thread([] {
        const std::string body = fetch();
        if (body.empty()) {
            std::fprintf(stderr, "[wr64] updates: couldn't reach GitHub\n");
            return;
        }
        try {
            found(nlohmann::json::parse(body).at("tag_name").get<std::string>());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[wr64] updates: unexpected answer (%s)\n", error.what());
        }
    }).detach();
}

bool available() { return g_available.load(std::memory_order_relaxed); }

bool known() {
    std::lock_guard lock(g_mutex);
    return !g_latest.empty();
}

std::string latest() {
    std::lock_guard lock(g_mutex);
    return g_latest;
}

void open_page() { SDL_OpenURL(kPage); }

}  // namespace wr64::updates

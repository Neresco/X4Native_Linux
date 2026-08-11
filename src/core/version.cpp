// ---------------------------------------------------------------------------
// version.cpp — Game version detection (Linux port)
//
// On Windows this read the game's version.dat next to X4.exe, with a PE
// version-info fallback. On Linux there is no PE image; the game directory
// is provided via the X4_GAME_ROOT environment variable (set by the launcher
// scripts) and defaults to the current working directory. Only version.dat is
// used — the PE fallback is dropped.
// ---------------------------------------------------------------------------
#include "version.h"
#include "logger.h"

#include "common/platform.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace x4n {

/// Resolve the game root directory (with trailing separator).
static std::string game_root() {
    const char* env = std::getenv("X4_GAME_ROOT");
    if (env && env[0]) {
        std::string dir(env);
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
            dir += '/';
        return dir;
    }
    // Default: the current working directory (the launcher scripts run from
    // the game directory).
    return "./";
}

/// Primary: read <game_root>/version.dat — a plain-text file containing
/// the build number as a single line (e.g. "900" for v9.00).
static std::string read_version_dat() {
    std::string dir = game_root();
    std::ifstream f(dir + "version.dat");
    if (!f.is_open()) return {};

    std::string line;
    std::getline(f, line);

    // Trim whitespace
    line.erase(line.begin(), std::find_if(line.begin(), line.end(),
               [](unsigned char c) { return !std::isspace(c); }));
    line.erase(std::find_if(line.rbegin(), line.rend(),
               [](unsigned char c) { return !std::isspace(c); }).base(), line.end());

    return line;
}

/// Format the raw build number (e.g. "900") into a readable version
/// string (e.g. "9.00").
static std::string format_version(const std::string& raw) {
    // The version.dat value is an integer: major * 100 + minor.
    // Examples: 900 → 9.00, 750 → 7.50, 800 → 8.00
    try {
        int v = std::stoi(raw);
        int major = v / 100;
        int minor = v % 100;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d.%02d", major, minor);
        return buf;
    } catch (...) {
        return raw;  // Couldn't parse — return as-is
    }
}

static std::string s_build_number;

std::string Version::detect() {
    // Read version.dat from the game root (reliable on both platforms).
    std::string raw = read_version_dat();
    if (!raw.empty()) {
        s_build_number = raw;
        std::string ver = format_version(raw);
        Logger::info("Detected game version: {} (build {})", ver, raw);
        return ver;
    }

    Logger::warn("Version: could not read version.dat from '{}'", game_root());
    return "unknown";
}

const std::string& Version::build() {
    return s_build_number;
}

} // namespace x4n
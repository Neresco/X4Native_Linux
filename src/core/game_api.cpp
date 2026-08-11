// ---------------------------------------------------------------------------
// x4native_core.dll — Game API Implementation
//
// Resolves all typed game functions via GetProcAddress into the
// X4GameFunctions struct. Uses the auto-generated x4_game_func_names.inc
// data to iterate over {name, offset} pairs.
//
// Internal (non-exported) functions are resolved from an RVA database
// (native/version_db/internal_functions.json).
// ---------------------------------------------------------------------------

#include "game_api.h"
#include "logger.h"

#include <x4_game_func_table.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include "common/windows_shim.h"
#include <cstdint>
#endif

#include <cstddef>
#include <cstring>
#include <fstream>
#include <vector>

namespace x4n {

X4GameFunctions GameAPI::s_table = {};
bool GameAPI::s_initialized = false;
std::unordered_map<std::string, void*> GameAPI::s_internal_funcs;

// ---------------------------------------------------------------------------
// Resolver data — reuse the X4_FUNC list to build {name, offset} pairs
// ---------------------------------------------------------------------------

struct FuncEntry {
    const char* name;
    size_t      offset;
};

#define X4_FUNC(ret, name, params) { #name, offsetof(X4GameFunctions, name) },
static const FuncEntry s_func_entries[] = {
#include <x4_game_func_list.inc>
};
#undef X4_FUNC

static constexpr int TOTAL_FUNCTIONS =
    static_cast<int>(sizeof(s_func_entries) / sizeof(s_func_entries[0]));

// Internal function resolver data — same struct, resolved from RVA database
#define X4_FUNC(ret, name, params) { #name, offsetof(X4GameFunctions, name) },
static const FuncEntry s_ifunc_entries[] = {
#include <x4_internal_func_list.inc>
    { nullptr, 0 }  // sentinel (list may be empty)
};
#undef X4_FUNC

#ifdef _WIN32
static HMODULE s_x4_module = nullptr;
#else
static void*   s_x4_module = nullptr;
#endif
static int     s_resolved  = 0;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool GameAPI::init() {
    // X4 is the host process. On Windows we use GetModuleHandle(NULL); on Linux
    // we open the main executable's global symbol table via dlopen(NULL). This
    // only yields symbols the binary actually exports (X4's no-Steam binary
    // exports very few, so most get_function() lookups will fail — which is
    // expected; hooks are disabled on Linux anyway, see minhook_shim).
#ifdef _WIN32
    s_x4_module = GetModuleHandleA(nullptr);
#else
    s_x4_module = dlopen(nullptr, RTLD_LAZY | RTLD_GLOBAL);
#endif
    if (!s_x4_module) {
        Logger::error("GameAPI: Failed to get X4 module handle");
        return false;
    }

    memset(&s_table, 0, sizeof(s_table));
    s_resolved = 0;

    auto* base = reinterpret_cast<char*>(&s_table);
    for (const auto& entry : s_func_entries) {
#ifdef _WIN32
        FARPROC proc = GetProcAddress(s_x4_module, entry.name);
#else
        void* proc = dlsym(s_x4_module, entry.name);
#endif
        if (proc) {
            *reinterpret_cast<void**>(base + entry.offset) = proc;
            s_resolved++;
        }
    }

    s_initialized = true;
    Logger::info("GameAPI: Resolved {}/{} game functions", s_resolved, TOTAL_FUNCTIONS);

    if (s_resolved < TOTAL_FUNCTIONS) {
        Logger::warn("GameAPI: {} functions could not be resolved",
                     TOTAL_FUNCTIONS - s_resolved);
    }

    return true;
}

void GameAPI::shutdown() {
    memset(&s_table, 0, sizeof(s_table));
    s_initialized = false;
    s_resolved = 0;
    s_x4_module = nullptr;
    s_internal_funcs.clear();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

X4GameFunctions* GameAPI::table() {
    return s_initialized ? &s_table : nullptr;
}

void* GameAPI::get_function(const char* name) {
    if (!s_x4_module || !name) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(s_x4_module, name));
#else
    return dlsym(s_x4_module, name);
#endif
}

void* GameAPI::get_internal(const char* name) {
    if (!name) return nullptr;
    auto it = s_internal_funcs.find(name);
    if (it != s_internal_funcs.end()) return it->second;
    return nullptr;
}

uintptr_t GameAPI::exe_base() {
#ifdef _WIN32
    return reinterpret_cast<uintptr_t>(s_x4_module);
#else
    // On Linux the "base" is not meaningful from a dlopen handle; return 0.
    // Internal RVA resolution is disabled on Linux (no exported symbols), so
    // this is only used as a fallback and is safe to be 0.
    return 0;
#endif
}

// Parse a "48 8B ?? C4" style signature into byte + mask vectors.
// Returns false on any malformed token.
static bool parse_byte_sig(const std::string& sig,
                           std::vector<uint8_t>& bytes,
                           std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    size_t i = 0;
    while (i < sig.size()) {
        if (sig[i] == ' ') { ++i; continue; }
        if (i + 1 >= sig.size()) return false;
        char a = sig[i], b = sig[i + 1];
        if (a == '?' && b == '?') {
            bytes.push_back(0);
            mask.push_back(0);
        } else {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(a), lo = hex(b);
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
            mask.push_back(1);
        }
        i += 2;
    }
    return !bytes.empty();
}

// Signature compare against live binary memory.
// On Windows this is SEH-guarded so an unmapped/unreadable address returns -1
// instead of crashing. On Linux, GCC/Clang lack SEH; we do a plain read. A
// truly unmapped page would SIGSEGV — acceptable for this PoC, and internal
// RVA resolution is disabled on Linux anyway (see exe_base / load_internal_db
// guard below). Returns 1 on match, 0 on mismatch.
static int seh_sig_match(const uint8_t* addr, const uint8_t* bytes,
                         const uint8_t* mask, size_t n) {
#ifdef _WIN32
    __try {
        for (size_t i = 0; i < n; ++i)
            if (mask[i] && addr[i] != bytes[i]) return 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#else
    for (size_t i = 0; i < n; ++i)
        if (mask[i] && addr[i] != bytes[i]) return 0;
    return 1;
#endif
}

void GameAPI::load_internal_db(const std::string& ext_root,
                               const std::string& build_label) {
#ifndef _WIN32
    // Internal RVA resolution requires reading the game binary at absolute
    // addresses derived from the module base. On Linux the no-Steam binary
    // does not export its symbol table, so the base is 0 and any RVA deref
    // would be an invalid low-address read. Frame-tick / MD / radar hooks are
    // therefore disabled on Linux (they are also disabled by the MinHook
    // shim). Skip the DB entirely.
    Logger::warn("GameAPI: internal RVA database is DISABLED on Linux "
                 "(no exported symbol base). Frame/MD/radar hooks unavailable.");
    return;
#endif
    std::string db_path = ext_root + "native/version_db/internal_functions.json";
    std::ifstream file(db_path);
    if (!file.is_open()) {
        Logger::error("GameAPI: internal functions database not found at {} — "
                      "frame tick, MD events and radar events are DISABLED "
                      "(is native/version_db/ missing from this install?)", db_path);
        return;
    }

    try {
        auto db = nlohmann::json::parse(file);
        if (!db.contains("functions") || !db["functions"].is_object()) return;

        auto base = reinterpret_cast<uintptr_t>(s_x4_module);
        int count = 0;
        int no_build = 0;
        int sig_rejected = 0;

        for (auto& [name, entry] : db["functions"].items()) {
            // Per-entry guard: one malformed entry must not discard the rest.
            try {
                if (!entry.contains(build_label)) { no_build++; continue; }
                auto& ver = entry[build_label];
                if (!ver.contains("rva") || !ver["rva"].is_string()) continue;

                auto rva_str = ver["rva"].get<std::string>();
                uintptr_t rva = std::stoull(rva_str, nullptr, 16);
                void* addr = reinterpret_cast<void*>(base + rva);

                // Verify the byte signature (if the entry has one) against the
                // live binary — a stale RVA then disables this one function
                // instead of hooking a wrong address.
                if (entry.contains("_find_hints") &&
                    entry["_find_hints"].contains("byte_sig") &&
                    entry["_find_hints"]["byte_sig"].is_string()) {
                    std::vector<uint8_t> bytes, sig_mask;
                    auto sig = entry["_find_hints"]["byte_sig"].get<std::string>();
                    if (parse_byte_sig(sig, bytes, sig_mask)) {
                        int m = seh_sig_match(static_cast<const uint8_t*>(addr),
                                              bytes.data(), sig_mask.data(), bytes.size());
                        if (m != 1) {
                            Logger::warn("GameAPI: '{}' byte signature {} at RVA {} — "
                                         "function disabled (stale RVA for this game build?)",
                                         name, m == 0 ? "mismatch" : "unreadable", rva_str);
                            sig_rejected++;
                            continue;
                        }
                    }
                }

                s_internal_funcs[name] = addr;
                count++;
            } catch (const std::exception& e) {
                Logger::warn("GameAPI: skipping malformed DB entry '{}': {}", name, e.what());
            }
        }

        if (no_build > 0)
            Logger::warn("GameAPI: {} internal function(s) have no RVA entry for build {} — skipped",
                         no_build, build_label);
        if (sig_rejected > 0)
            Logger::warn("GameAPI: {} internal function(s) failed signature verification — disabled",
                         sig_rejected);

        // Populate internal function pointers into the unified game table
        auto* tbl_base = reinterpret_cast<char*>(&s_table);
        int struct_resolved = 0;
        for (const auto& ie : s_ifunc_entries) {
            if (!ie.name) break;  // sentinel
            auto it = s_internal_funcs.find(ie.name);
            if (it != s_internal_funcs.end()) {
                *reinterpret_cast<void**>(tbl_base + ie.offset) = it->second;
                struct_resolved++;
            }
        }

        if (count > 0)
            Logger::info("GameAPI: Resolved {} internal function(s) from RVA database",
                         count);
    } catch (const std::exception& e) {
        Logger::warn("GameAPI: Failed to parse internal_functions.json: {}", e.what());
    }
}

int GameAPI::resolved_count() { return s_resolved; }
int GameAPI::total_count()    { return TOTAL_FUNCTIONS; }
int GameAPI::internal_count() { return static_cast<int>(s_internal_funcs.size()); }

} // namespace x4n

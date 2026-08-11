#pragma once
// ---------------------------------------------------------------------------
// x4native_core.dll — Hook Manager
//
// Managed function hooking via MinHook. One detour per target function,
// with before/after callback chains dispatched in extension priority order.
//
// Extensions register hooks via the C ABI (hook_before/hook_after/unhook
// function pointers on X4NativeAPI). The SDK's x4n::hook namespace wraps
// these into type-safe C++ templates.
//
// Chain execution:
//   1. All before-callbacks run (priority order)
//   2. If no callback set skip_original → call original
//   3. All after-callbacks run (priority order)
//
// SEH wrapping per callback — crashing callbacks are auto-disabled.
// ---------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "common/windows_shim.h"

#include <x4native_extension.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace x4n {

// ---------------------------------------------------------------------------
// Hook callback info (internal representation)
// ---------------------------------------------------------------------------
struct HookCallbackInfo {
    int                id;
    int                priority;        // Extension priority (lower = first)
    std::string        extension_name;
    X4HookCallback     callback;
    void*              userdata;
    bool               enabled;
    bool               is_before;       // true = before, false = after
};

// ---------------------------------------------------------------------------
// Per-function hook state
// ---------------------------------------------------------------------------
struct HookedFunction {
    std::string                   name;
    void*                         target;       // Original function address
    void*                         trampoline;   // MinHook trampoline (calls original)
    void*                         detour;       // Installed detour (code lives in the FIRST hooking extension's DLL)
    HMODULE                       detour_module;// Module containing the detour code
    std::vector<HookCallbackInfo> before_hooks;
    std::vector<HookCallbackInfo> after_hooks;
};

// ---------------------------------------------------------------------------
// HookManager — singleton, owned by core lifecycle
// ---------------------------------------------------------------------------
class HookManager {
public:
    static bool init();
    static void shutdown();

    // Register a before/after hook. Returns hook ID (>0) or -1 on error.
    static int hook_before(const char* function_name,
                           X4HookCallback callback, void* userdata,
                           int priority, const char* ext_name);
    static int hook_after(const char* function_name,
                          X4HookCallback callback, void* userdata,
                          int priority, const char* ext_name);

    // Remove a specific hook by ID.
    static void unhook(int hook_id);

    // Remove all hooks registered by a specific extension.
    static void remove_all_for_extension(const char* ext_name);

    // Called before FreeLibrary on an extension module. If other extensions
    // still dispatch through a detour whose code lives in the unloading DLL,
    // pin that DLL in memory — unmapping it would leave the game's patched
    // entry point jumping into freed memory.
    static void protect_dangling_detours(HMODULE unloading_module);

    // Remove all hooks and uninitialize MinHook.
    static void remove_all();

    // Get the trampoline (original) for a hooked function.
    static void* get_trampoline(const char* function_name);

    // Install a MinHook detour for a function (if not already installed).
    // Returns the trampoline pointer, or nullptr on error.
    static void* ensure_detour(const char* function_name, void* detour_fn);

    // Run before-hooks / after-hooks for a function.
    // Called by typed detours generated in x4native.h.
    static void run_before_hooks(X4HookContext* ctx);
    static void run_after_hooks(X4HookContext* ctx);

private:
    // Ensure a function is hooked (MinHook detour installed)
    static HookedFunction* ensure_hooked(const char* function_name);

    // Sort callbacks by priority
    static void sort_callbacks(std::vector<HookCallbackInfo>& cbs);

    static std::unordered_map<std::string, HookedFunction> s_hooks;
    static std::recursive_mutex s_mutex;
    static int s_next_id;
    static bool s_initialized;
};

} // namespace x4n

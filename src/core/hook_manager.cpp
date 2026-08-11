// ---------------------------------------------------------------------------
// x4native_core.dll — Hook Manager Implementation
//
// Uses MinHook for inline hooking. Each hooked game function gets one
// MinHook detour that routes through our managed dispatcher, which runs
// before/after callback chains with SEH protection.
// ---------------------------------------------------------------------------

#include "hook_manager.h"
#include "logger.h"
#include "game_api.h"

#ifdef _WIN32
#include <MinHook.h>
#else
#include "common/minhook_shim.h"
#endif
#include <algorithm>

namespace x4n {

std::unordered_map<std::string, HookedFunction> HookManager::s_hooks;
std::recursive_mutex HookManager::s_mutex;
int HookManager::s_next_id = 1;
bool HookManager::s_initialized = false;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool HookManager::init() {
    std::lock_guard lock(s_mutex);
    if (s_initialized) return true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        Logger::error("HookManager: MH_Initialize failed: {}", MH_StatusToString(status));
        return false;
    }

    s_initialized = true;
    Logger::info("HookManager: initialized");
    return true;
}

void HookManager::shutdown() {
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return;

    // Disable and remove all hooks
    for (auto& [name, hf] : s_hooks) {
        MH_DisableHook(hf.target);
        MH_RemoveHook(hf.target);
    }
    s_hooks.clear();

    MH_Uninitialize();
    s_initialized = false;
    Logger::info("HookManager: shut down");
}

void HookManager::remove_all() {
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return;

    MH_DisableHook(MH_ALL_HOOKS);
    for (auto& [name, hf] : s_hooks) {
        MH_RemoveHook(hf.target);
    }
    s_hooks.clear();
    Logger::info("HookManager: all hooks removed");
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

int HookManager::hook_before(const char* function_name,
                             X4HookCallback callback, void* userdata,
                             int priority, const char* ext_name) {
    std::lock_guard lock(s_mutex);
    if (!s_initialized || !function_name || !callback) return -1;

    HookedFunction* hf = ensure_hooked(function_name);
    if (!hf) return -1;

    int id = s_next_id++;
    hf->before_hooks.push_back({
        id, priority, ext_name ? ext_name : "", callback, userdata, true, true
    });
    sort_callbacks(hf->before_hooks);

    Logger::debug("HookManager: before-hook #{} on '{}' (ext={}, pri={})",
                  id, function_name, ext_name ? ext_name : "?", priority);
    return id;
}

int HookManager::hook_after(const char* function_name,
                            X4HookCallback callback, void* userdata,
                            int priority, const char* ext_name) {
    std::lock_guard lock(s_mutex);
    if (!s_initialized || !function_name || !callback) return -1;

    HookedFunction* hf = ensure_hooked(function_name);
    if (!hf) return -1;

    int id = s_next_id++;
    hf->after_hooks.push_back({
        id, priority, ext_name ? ext_name : "", callback, userdata, true, false
    });
    sort_callbacks(hf->after_hooks);

    Logger::debug("HookManager: after-hook #{} on '{}' (ext={}, pri={})",
                  id, function_name, ext_name ? ext_name : "?", priority);
    return id;
}

void HookManager::unhook(int hook_id) {
    std::lock_guard lock(s_mutex);
    if (!s_initialized || hook_id <= 0) return;

    for (auto& [name, hf] : s_hooks) {
        auto remove_from = [&](std::vector<HookCallbackInfo>& cbs) -> bool {
            for (auto it = cbs.begin(); it != cbs.end(); ++it) {
                if (it->id == hook_id) {
                    Logger::debug("HookManager: removed hook #{} from '{}'",
                                  hook_id, name);
                    cbs.erase(it);
                    return true;
                }
            }
            return false;
        };
        if (remove_from(hf.before_hooks) || remove_from(hf.after_hooks)) {
            // If no callbacks remain, remove the MinHook detour
            if (hf.before_hooks.empty() && hf.after_hooks.empty()) {
                MH_DisableHook(hf.target);
                MH_RemoveHook(hf.target);
                s_hooks.erase(name);
                Logger::debug("HookManager: detour removed for '{}' (no callbacks left)", name);
            }
            return;
        }
    }
}

void HookManager::remove_all_for_extension(const char* ext_name) {
    std::lock_guard lock(s_mutex);
    if (!s_initialized || !ext_name) return;

    std::string en(ext_name);
    std::vector<std::string> empty_hooks;

    for (auto& [name, hf] : s_hooks) {
        auto remove_matching = [&](std::vector<HookCallbackInfo>& cbs) {
            cbs.erase(std::remove_if(cbs.begin(), cbs.end(),
                [&](const HookCallbackInfo& cb) { return cb.extension_name == en; }),
                cbs.end());
        };
        remove_matching(hf.before_hooks);
        remove_matching(hf.after_hooks);

        if (hf.before_hooks.empty() && hf.after_hooks.empty()) {
            empty_hooks.push_back(name);
        }
    }

    for (const auto& name : empty_hooks) {
        auto it = s_hooks.find(name);
        if (it != s_hooks.end()) {
            MH_DisableHook(it->second.target);
            MH_RemoveHook(it->second.target);
            s_hooks.erase(it);
        }
    }
}

void* HookManager::get_trampoline(const char* function_name) {
    std::lock_guard lock(s_mutex);
    auto it = s_hooks.find(function_name);
    if (it != s_hooks.end()) return it->second.trampoline;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Internal: ensure a MinHook detour is installed for a function
// ---------------------------------------------------------------------------
// Note: s_mutex must be held by caller
HookedFunction* HookManager::ensure_hooked(const char* function_name) {
    // Already hooked?
    auto it = s_hooks.find(function_name);
    if (it != s_hooks.end()) return &it->second;

    // Resolve the function address — try exports first, then internal RVA db
    void* target = GameAPI::get_function(function_name);
    if (!target)
        target = GameAPI::get_internal(function_name);
    if (!target) {
        Logger::error("HookManager: function '{}' not found (exports or internal DB)",
                      function_name);
        return nullptr;
    }

    // Install MinHook detour — but we can't use a generic dispatcher directly
    // because MinHook replaces the function entry point with a jump to our
    // detour. The detour must have the same calling convention and signature.
    // The typed SDK wrappers (x4native.h) generate per-function detours and
    // pass them to ensure_detour() which calls MH_CreateHook.

    HookedFunction hf;
    hf.name = function_name;
    hf.target = target;
    hf.trampoline = nullptr;
    hf.detour = nullptr;
    hf.detour_module = nullptr;

    auto [ins, _] = s_hooks.emplace(function_name, std::move(hf));
    return &ins->second;
}

void HookManager::sort_callbacks(std::vector<HookCallbackInfo>& cbs) {
    std::sort(cbs.begin(), cbs.end(),
              [](const HookCallbackInfo& a, const HookCallbackInfo& b) {
                  return a.priority < b.priority;
              });
}

// ---------------------------------------------------------------------------
// Detour installation — called by SDK typed wrappers via _ensure_detour
// ---------------------------------------------------------------------------

void* HookManager::ensure_detour(const char* function_name, void* detour_fn) {
    std::lock_guard lock(s_mutex);
    if (!s_initialized || !function_name || !detour_fn) return nullptr;

    HookedFunction* hf = ensure_hooked(function_name);
    if (!hf) return nullptr;

    // Already detoured — return existing trampoline
    if (hf->trampoline) return hf->trampoline;

    void* trampoline = nullptr;
    MH_STATUS status = MH_CreateHook(hf->target, detour_fn, &trampoline);
    if (status != MH_OK) {
        Logger::error("HookManager: MH_CreateHook failed for '{}': {}",
                      function_name, MH_StatusToString(status));
        return nullptr;
    }

    status = MH_EnableHook(hf->target);
    if (status != MH_OK) {
        Logger::error("HookManager: MH_EnableHook failed for '{}': {}",
                      function_name, MH_StatusToString(status));
        MH_RemoveHook(hf->target);
        return nullptr;
    }

    hf->trampoline = trampoline;
    hf->detour = detour_fn;
    // The detour code lives in the calling extension's DLL (typed_detour
    // template instantiation). Remember which module that is so unload can
    // detect when other extensions still route through it.
    hf->detour_module = nullptr;
#ifdef _WIN32
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCSTR>(detour_fn), &hf->detour_module);
#endif
    Logger::info("HookManager: detour installed for '{}'", function_name);
    return trampoline;
}

void HookManager::protect_dangling_detours(HMODULE unloading_module) {
    std::lock_guard lock(s_mutex);
    if (!s_initialized || !unloading_module) return;

#ifdef _WIN32
    for (auto& [name, hf] : s_hooks) {
        if (hf.detour_module != unloading_module) continue;
        if (hf.before_hooks.empty() && hf.after_hooks.empty()) continue;

        // Other extensions still dispatch through detour code that lives in
        // the DLL about to be unloaded. A survivor re-install is not possible
        // yet (extensions cache the trampoline pointer in their own statics),
        // so pin the module — it stays mapped until process exit, which is a
        // small leak instead of a guaranteed crash on the next hooked call.
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           static_cast<LPCSTR>(hf.detour), &pinned);
        Logger::warn("HookManager: '{}' is still hooked by other extensions through "
                     "a detour owned by the unloading DLL — DLL pinned in memory to "
                     "prevent a crash. Hot-reloading this extension will fail until "
                     "the other extensions release their hooks.", name);
    }
#else
    // On Linux no detours are installed (see minhook_shim), so there is nothing
    // to protect. This is a no-op.
    (void)unloading_module;
#endif
}

// ---------------------------------------------------------------------------
// Dispatch — called by typed detours generated in x4native.h
// ---------------------------------------------------------------------------

// Callback invocation wrapper.
// On Windows this is an SEH __try/__except guard that also catches hardware
// faults (access violations inside an extension). On Linux, GCC/Clang do not
// support SEH, so we fall back to a plain call. A crashing extension will take
// the game down — extension authors must ensure their hooks are safe. (A
// sigsetjmp-based guard could be added later if needed.)
static int seh_call_hook(X4HookCallback callback, X4HookContext* ctx) {
#ifdef _WIN32
    __try {
        return callback(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#else
    return callback(ctx);
#endif
}

void HookManager::run_before_hooks(X4HookContext* ctx) {
    if (!ctx || !ctx->function_name) return;

    std::vector<HookCallbackInfo> hooks_copy;
    {
        std::lock_guard lock(s_mutex);
        auto it = s_hooks.find(ctx->function_name);
        if (it == s_hooks.end()) return;
        hooks_copy = it->second.before_hooks;
    }

    for (auto& hook : hooks_copy) {
        if (!hook.enabled) continue;
        ctx->userdata = hook.userdata;
        if (seh_call_hook(hook.callback, ctx) == -1) {
            Logger::error("HookManager: before-hook #{} crashed in '{}' (ext={}) — disabling",
                          hook.id, ctx->function_name, hook.extension_name);
            // Disable in the actual chain
            std::lock_guard lock(s_mutex);
            auto it = s_hooks.find(ctx->function_name);
            if (it != s_hooks.end()) {
                for (auto& h : it->second.before_hooks)
                    if (h.id == hook.id) { h.enabled = false; break; }
            }
        }
    }
}

void HookManager::run_after_hooks(X4HookContext* ctx) {
    if (!ctx || !ctx->function_name) return;

    std::vector<HookCallbackInfo> hooks_copy;
    {
        std::lock_guard lock(s_mutex);
        auto it = s_hooks.find(ctx->function_name);
        if (it == s_hooks.end()) return;
        hooks_copy = it->second.after_hooks;
    }

    for (auto& hook : hooks_copy) {
        if (!hook.enabled) continue;
        ctx->userdata = hook.userdata;
        if (seh_call_hook(hook.callback, ctx) == -1) {
            Logger::error("HookManager: after-hook #{} crashed in '{}' (ext={}) — disabling",
                          hook.id, ctx->function_name, hook.extension_name);
            std::lock_guard lock(s_mutex);
            auto it = s_hooks.find(ctx->function_name);
            if (it != s_hooks.end()) {
                for (auto& h : it->second.after_hooks)
                    if (h.id == hook.id) { h.enabled = false; break; }
            }
        }
    }
}

} // namespace x4n

// ---------------------------------------------------------------------------
// common/minhook_shim.cpp — Minimal MinHook compatibility shim (Linux)
//
// On Windows the framework links the real MinHook (which patches x86-64 code
// to install detours). This shim implements the same API surface on Linux
// using POSIX mprotect + a classic 5-byte relative-jump trampoline.
//
// Model (same as MinHook short jump):
//   - target entry is patched with JMP rel32 -> detour
//   - trampoline = original prologue + JMP rel32 -> target + prologue_len
//     so a detour can invoke the "original" behaviour via the trampoline.
//
// Within a single Linux process the game binary and our .so are normally
// mapped within +/-2 GiB of each other, so the 32-bit displacement is
// sufficient. If the displacement is out of range we report
// MH_ERROR_UNSUPPORTED rather than writing a corrupt jump.
// ---------------------------------------------------------------------------

#ifndef _WIN32

#include "minhook_shim.h"

#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <unistd.h>
#include <sys/mman.h>

namespace {

struct Hook {
    void*    target    = nullptr;
    void*    detour    = nullptr;
    uint8_t  orig_bytes[16] = {};   // saved prologue (5 used)
    size_t   orig_len  = 0;
    uint8_t* trampoline = nullptr;
    bool     enabled   = false;
};

std::mutex                                  g_lock;
std::unordered_map<void*, Hook>            g_hooks;
bool                                       g_initialized = false;

bool set_writable(uint8_t* addr, size_t len, bool writable) {
    long pagesize = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)addr & ~(uintptr_t)(pagesize - 1);
    size_t   span = ((uintptr_t)addr + len) - page;
    int prot = PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0);
    return mprotect((void*)page, span, prot) == 0;
}

// 32-bit relative displacement from `from` to `to`; sentinel if out of range.
int32_t rel32(uintptr_t from, uintptr_t to) {
    int64_t delta = (int64_t)to - (int64_t)from;
    if (delta < INT32_MIN || delta > INT32_MAX) return 0x7FFFFFFF;
    return (int32_t)delta;
}

// Patch `target` with JMP rel32 -> `dest`. Caller must have made it writable.
void write_jmp(uint8_t* target, uintptr_t dest) {
    target[0] = 0xE9; // JMP rel32
    *reinterpret_cast<int32_t*>(target + 1) =
        rel32((uintptr_t)target, dest);
}

} // namespace

MH_STATUS MH_Initialize(void) {
    g_initialized = true;
    return MH_OK;
}

MH_STATUS MH_Uninitialize(void) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (auto& kv : g_hooks) {
        Hook& h = kv.second;
        if (h.enabled) {
            set_writable((uint8_t*)h.target, h.orig_len, true);
            memcpy(h.target, h.orig_bytes, h.orig_len);
            set_writable((uint8_t*)h.target, h.orig_len, false);
        }
        delete[] h.trampoline;
    }
    g_hooks.clear();
    g_initialized = false;
    return MH_OK;
}

MH_STATUS MH_CreateHook(void* pTarget, void* pDetour, void** ppOriginal) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;
    if (!pTarget || !pDetour) return MH_ERROR;

    std::lock_guard<std::mutex> lk(g_lock);
    if (g_hooks.count(pTarget)) return MH_ERROR; // already created

    constexpr size_t PROLOGUE = 5;

    uintptr_t tgt = (uintptr_t)pTarget;
    uintptr_t det = (uintptr_t)pDetour;
    if (rel32(tgt, det) == 0x7FFFFFFF) return MH_ERROR_UNSUPPORTED;

    Hook h;
    h.target   = pTarget;
    h.detour   = pDetour;
    h.orig_len = PROLOGUE;
    memcpy(h.orig_bytes, pTarget, PROLOGUE);

    // trampoline = original prologue + JMP rel32 -> target + PROLOGUE
    h.trampoline = new uint8_t[PROLOGUE + 5];
    memcpy(h.trampoline, h.orig_bytes, PROLOGUE);
    write_jmp(h.trampoline + PROLOGUE, tgt + PROLOGUE);

    if (ppOriginal) *ppOriginal = h.trampoline;

    g_hooks[pTarget] = std::move(h);
    return MH_OK;
}

MH_STATUS MH_EnableHook(void* pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;

    std::lock_guard<std::mutex> lk(g_lock);
    if (pTarget == MH_ALL_HOOKS) {
        for (auto& kv : g_hooks) {
            Hook& h = kv.second;
            if (h.enabled) continue;
            if (!set_writable((uint8_t*)h.target, h.orig_len, true))
                return MH_ERROR_NOT_EXECUTABLE;
            write_jmp((uint8_t*)h.target, (uintptr_t)h.detour);
            set_writable((uint8_t*)h.target, h.orig_len, false);
            h.enabled = true;
        }
        return MH_OK;
    }

    auto it = g_hooks.find(pTarget);
    if (it == g_hooks.end()) return MH_ERROR_NOT_CREATED;
    Hook& h = it->second;
    if (h.enabled) return MH_OK;
    if (!set_writable((uint8_t*)h.target, h.orig_len, true))
        return MH_ERROR_NOT_EXECUTABLE;
    write_jmp((uint8_t*)h.target, (uintptr_t)h.detour);
    set_writable((uint8_t*)h.target, h.orig_len, false);
    h.enabled = true;
    return MH_OK;
}

MH_STATUS MH_DisableHook(void* pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;

    std::lock_guard<std::mutex> lk(g_lock);
    if (pTarget == MH_ALL_HOOKS) {
        for (auto& kv : g_hooks) {
            Hook& h = kv.second;
            if (!h.enabled) continue;
            set_writable((uint8_t*)h.target, h.orig_len, true);
            memcpy(h.target, h.orig_bytes, h.orig_len);
            set_writable((uint8_t*)h.target, h.orig_len, false);
            h.enabled = false;
        }
        return MH_OK;
    }

    auto it = g_hooks.find(pTarget);
    if (it == g_hooks.end()) return MH_ERROR_NOT_CREATED;
    Hook& h = it->second;
    if (!h.enabled) return MH_OK;
    set_writable((uint8_t*)h.target, h.orig_len, true);
    memcpy(h.target, h.orig_bytes, h.orig_len);
    set_writable((uint8_t*)h.target, h.orig_len, false);
    h.enabled = false;
    return MH_OK;
}

MH_STATUS MH_RemoveHook(void* pTarget) {
    if (!g_initialized) return MH_ERROR_NOT_INITIALIZED;

    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_hooks.find(pTarget);
    if (it == g_hooks.end()) return MH_ERROR_NOT_CREATED;
    Hook& h = it->second;
    if (h.enabled) {
        set_writable((uint8_t*)h.target, h.orig_len, true);
        memcpy(h.target, h.orig_bytes, h.orig_len);
        set_writable((uint8_t*)h.target, h.orig_len, false);
    }
    delete[] h.trampoline;
    g_hooks.erase(it);
    return MH_OK;
}

const char* MH_StatusToString(MH_STATUS status) {
    switch (status) {
        case MH_OK:                   return "OK";
        case MH_ERROR:                return "error";
        case MH_ERROR_NOT_INITIALIZED: return "not initialized";
        case MH_ERROR_NOT_CREATED:    return "not created";
        case MH_ERROR_UNSUPPORTED:    return "unsupported (out of jump range)";
        case MH_ERROR_NOT_EXECUTABLE: return "memory not executable/writable";
        default:                      return "unknown";
    }
}

#endif // _WIN32
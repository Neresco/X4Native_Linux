// ---------------------------------------------------------------------------
// common/minhook_shim.h — Minimal MinHook compatibility shim (Linux)
//
// On Windows the framework links the real MinHook (which patches x86-64 code
// to install detours). On Linux there is no first-class MinHook port shipped
// with this project, and incorrect code patching can crash the host game.
//
// This shim implements the MinHook *API surface* but reports every hook
// installation as unsupported, so the core installs zero code patches and the
// game runs unmodified. Hooks that extensions register via hook_before /
// hook_after are simply not invoked (logged as unavailable). Frame-tick,
// MD-event and radar hooks are likewise skipped.
//
// This keeps the dedicated-server / client path stable. A full MinHook
// Linux port (or a pointer-swap detour scheme for the game's internal tables)
// can replace this shim later if those events are needed.
// ---------------------------------------------------------------------------
#pragma once

#ifndef _WIN32

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef intptr_t MH_STATUS;
#define MH_OK                   0
#define MH_ERROR                (-1)
#define MH_ERROR_NOT_INITIALIZED   (-6)
#define MH_ERROR_NOT_CREATED       (-9)
#define MH_ERROR_UNSUPPORTED       (-13)
#define MH_ERROR_NOT_EXECUTABLE    (-14)

typedef intptr_t  MH_STATUS_;  // alias kept for compatibility
typedef void*     LPVOID;
typedef void*     FARPROC;
typedef void*     PVOID;

#define MH_ALL_HOOKS ((void*)(-1))

// Hook installation is reported as unsupported on Linux (see .cpp).
MH_STATUS MH_Initialize(void);
MH_STATUS MH_Uninitialize(void);
MH_STATUS MH_CreateHook(void* pTarget, void* pDetour, void** ppOriginal);
MH_STATUS MH_EnableHook(void* pTarget);
MH_STATUS MH_DisableHook(void* pTarget);
MH_STATUS MH_RemoveHook(void* pTarget);
const char* MH_StatusToString(MH_STATUS status);

#ifdef __cplusplus
}
#endif

#endif // _WIN32
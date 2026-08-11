// ---------------------------------------------------------------------------
// common/windows_shim.h — minimal Windows API surface for the Linux port.
//
// The original x4native sources include <windows.h> and use a handful of
// Win32 types/macros. On Linux we provide a source-compatible shim so the
// code compiles unchanged. platform.h already defines HMODULE / FARPROC and
// the module/symbol-resolution helpers; this header adds the file-handle
// types and the few remaining macros used by the core.
// ---------------------------------------------------------------------------
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else // !_WIN32 ------------------------------------------------------------

#include <cstdint>
#include <cstddef>

#include "common/platform.h"

// HANDLE / HMODULE are void* (see platform.h / winfile_shim.h).
using HANDLE = void*;
using HMODULE = void*;

#ifndef INVALID_HANDLE_VALUE
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))
#endif

// FILETIME — on Linux the extension manager tracks mtimes as struct timespec
// (see platform.h compare_file_time), so alias FILETIME to timespec so the
// existing mtime code compiles unchanged.
using FILETIME = struct timespec;

// Access / share flags passed to CreateFileA (winfile_shim.cpp ignores most).
#ifndef GENERIC_WRITE
#define GENERIC_WRITE 0x40000000u
#endif
#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001u
#endif
#ifndef FILE_SHARE_WRITE
#define FILE_SHARE_WRITE 0x00000002u
#endif
#ifndef CREATE_ALWAYS
#define CREATE_ALWAYS 2
#endif
#ifndef OPEN_ALWAYS
#define OPEN_ALWAYS 4
#endif

// Module pinning flags used by HookManager::protect_dangling_detours.
#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004u
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002u
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_PIN
#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001u
#endif

inline bool GetModuleHandleExA(uint32_t /*flags*/, const char* /*addr_or_name*/, HMODULE* out) {
    // On Linux we cannot pin an arbitrary module; return a placeholder.
    if (out) *out = reinterpret_cast<HMODULE>(RTLD_DEFAULT);
    return true;
}

#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

#ifndef DWORD
using DWORD = uint32_t;
#endif

#endif // _WIN32
// ---------------------------------------------------------------------------
// common/platform.h — Cross-platform abstraction layer
//
// The Windows build of x4native uses the Win32 API directly (HMODULE,
// GetModuleHandle, GetProcAddress, LoadLibrary, HANDLE/CreateFile, SEH, ...).
// On Linux none of these exist, so this header provides a minimal, source-
// compatible surface that maps the Win32 calls onto POSIX equivalents:
//
//   HMODULE            -> void* (a dlopen handle, or the live process image)
//   FARPROC           -> void*
//   GetModuleHandleA  -> get_module_handle()
//   GetProcAddress    -> get_proc_address()
//   LoadLibraryA      -> load_library()        (dlopen)
//   FreeLibrary       -> free_library()        (dlclose)
//   CopyFileA         -> copy_file()
//   DeleteFileA       -> delete_file()
//   GetFileAttributesExA / WIN32_FILE_ATTRIBUTE_DATA -> stat-based mtime
//   CompareFileTime   -> compare_file_time()
//   GetLastError      -> last_error()
//   HANDLE / CreateFileA / ReadFile / WriteFile / CloseHandle / FlushFileBuffers
//                      -> POSIX fd wrappers (see common/winfile_shim.h)
//   __try/__except    -> X4N_SEH_GUARD(TRY, CATCH) macro (no-op on Linux)
//
// On Windows this header simply re-includes <windows.h> and defines the
// X4N_SEH_GUARD macro as the real SEH construct.
// ---------------------------------------------------------------------------
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// On Windows, X4N_SEH_GUARD expands to real structured exception handling.
#define X4N_SEH_GUARD(try_block, catch_block) \
    __try { return ([&]() -> int try_block()); } \
    __except (EXCEPTION_EXECUTE_HANDLER) { return ([&]() -> int catch_block()); }

#else // !_WIN32 --------------------------------------------------------------

#include <cstdint>
#include <cstddef>
#include <string>
#include <dlfcn.h>
#include <sys/stat.h>

using HMODULE = void*;
using FARPROC = void*;

#ifndef X4N_WINDOWS
#define X4N_WINDOWS 0
#endif

// SEH is not available on Linux. The guard macro simply runs the try block;
// the catch block is provided for API compatibility but never executed because
// a faulting callback on Linux would normally crash the process. x4native
// relies on this only for isolating misbehaving extensions; on Linux we still
// wrap the call so the structure is identical, but a real segfault will abort.
// (A production build could install a SIGSEGV handler per-call, but that is
// out of scope for this port.)
#define X4N_SEH_GUARD(try_block, catch_block) \
    try {                                      \
        return ([&]() -> int try_block());     \
    } catch (...) {                            \
        return ([&]() -> int catch_block());   \
    }

// Module / symbol resolution --------------------------------------------------
inline HMODULE get_module_handle() {
    // On Linux the extension is loaded into the X4 process image. We resolve
    // symbols against the global symbol table (RTLD_DEFAULT) so the "handle"
    // is just a placeholder pointer; it is never used for dlclose.
    return reinterpret_cast<HMODULE>(RTLD_DEFAULT);
}

inline FARPROC get_proc_address(HMODULE module, const char* name) {
    // Use the actual module handle, NOT RTLD_DEFAULT. The extension is
    // dlopen'd with RTLD_LOCAL (see load_library), so its exported symbols
    // are NOT visible in the global scope. dlsym(RTLD_DEFAULT, name) would
    // therefore fail to find x4native_api_version / x4native_init /
    // x4native_shutdown, causing "missing required exports" and the extension
    // to be rejected. dlsym(handle, name) resolves them correctly.
    // (On Windows GetProcAddress(hModule, name) is handle-based too, so this
    // also restores parity with the Windows build.)
    if (!module) module = reinterpret_cast<HMODULE>(RTLD_DEFAULT);
    return reinterpret_cast<FARPROC>(dlsym(module, name));
}

inline HMODULE load_library(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

inline void free_library(HMODULE module) {
    if (module) dlclose(module);
}

// Identify which module a code address belongs to (for dangling-detour pinning).
HMODULE get_module_from_addr(void* addr);
// Increment the reference count / pin a module so it cannot be unloaded.
void pin_module(void* addr);

// Filesystem helpers ----------------------------------------------------------
inline bool copy_file(const char* src, const char* dst) {
    // Simple copy via cat; sufficient for the copy-on-load step.
    std::string cmd = std::string("cat '") + src + "' > '" + dst + "'";
    return system(cmd.c_str()) == 0;
}

inline bool delete_file(const char* path) {
    return std::remove(path) == 0;
}

struct WIN32_FILE_ATTRIBUTE_DATA {
    struct timespec ftLastWriteTime {};
};

inline bool get_file_attributes_ex(const char* path, void* /*level*/, WIN32_FILE_ATTRIBUTE_DATA* data) {
    struct stat st {};
    if (stat(path, &st) != 0) return false;
    data->ftLastWriteTime = st.st_mtim;
    return true;
}

inline int compare_file_time(const struct timespec* a, const struct timespec* b) {
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

inline unsigned long last_error() { return (unsigned long)dlerror(); }

#endif // _WIN32
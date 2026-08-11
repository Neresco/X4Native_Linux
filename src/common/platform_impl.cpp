// ---------------------------------------------------------------------------
// common/platform_impl.cpp — Linux implementation of platform.h helpers.
// ---------------------------------------------------------------------------
#include "common/platform.h"

#ifndef _WIN32

#include <dlfcn.h>
#include <link.h>

HMODULE get_module_from_addr(void* addr) {
    Dl_info info {};
    if (dladdr(addr, &info)) {
        // Return the base address of the module containing addr.
        return reinterpret_cast<HMODULE>(const_cast<void*>(info.dli_fbase));
    }
    return nullptr;
}

void pin_module(void* /*addr*/) {
    // On Linux we never dlclose extension modules (see free_library in
    // platform.h), so there is nothing to pin: the module image stays
    // resident for the lifetime of the process. This keeps dangling detour
    // callbacks valid without reference counting.
}

#endif // _WIN32
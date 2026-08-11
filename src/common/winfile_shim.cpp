// ---------------------------------------------------------------------------
// common/winfile_shim.cpp — POSIX implementation of the Win32 file shim.
// ---------------------------------------------------------------------------
#include "winfile_shim.h"

#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

HANDLE CreateFileA(const char* filename, unsigned long access, unsigned long /*share*/,
                   void* /*sa*/, unsigned long creation, unsigned long /*flags*/, HANDLE /*tmpl*/) {
    int flags = O_WRONLY | O_CREAT;
    // FILE_APPEND_DATA => append; otherwise (truncate) => O_TRUNC.
    if (access & FILE_APPEND_DATA)
        flags |= O_APPEND;
    else
        flags |= O_TRUNC;
    int fd = open(filename, flags, 0644);
    if (fd < 0) return INVALID_HANDLE_VALUE;
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd));
}

void SetFilePointer(HANDLE /*h*/, long /*dist*/, long* /*hi*/, unsigned long /*mode*/) {
    // No-op: O_APPEND handles positioning automatically.
}

void CloseHandle(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) return;
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(h));
    if (fd >= 0) close(fd);
}

void FlushFileBuffers(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) return;
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(h));
    if (fd >= 0) fsync(fd);
}

#endif // _WIN32
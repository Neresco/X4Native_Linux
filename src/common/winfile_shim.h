// ---------------------------------------------------------------------------
// common/winfile_shim.h — Minimal Win32 file-handle shim for Linux
//
// Maps the handful of Win32 file APIs used by extension_manager.cpp onto
// POSIX file descriptors. A HANDLE is just an int fd (or -1 for invalid).
// ---------------------------------------------------------------------------
#pragma once

#ifdef _WIN32
#include "common/windows_shim.h"
#else

#include <cstdint>
#include <string>

using HANDLE = void*;
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))

// Access / share / creation / flags (values are arbitrary; the shim only
// needs to distinguish "append" vs "truncate" for open mode).
#define GENERIC_WRITE        0x40000000u
#define FILE_APPEND_DATA     0x00000004u
#define FILE_SHARE_READ      0x00000001u
#define OPEN_ALWAYS          4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_END             2

HANDLE CreateFileA(const char* filename, unsigned long access, unsigned long share,
                   void* /*sa*/, unsigned long creation, unsigned long flags, HANDLE /*tmpl*/);
void SetFilePointer(HANDLE h, long /*dist*/, long* /*hi*/, unsigned long /*mode*/);
void CloseHandle(HANDLE h);
void FlushFileBuffers(HANDLE h);

#endif // _WIN32
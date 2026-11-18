// x4mp_platform.h -- single portability shim for the x4mp / x4mp_stream native
// extensions. On Linux it just pulls in the POSIX headers the code already used;
// on Windows (MinGW cross-build) it maps the POSIX socket/file names onto
// WinSock2 / Win32 equivalents so the .cpp bodies stay platform-independent.
//
// Design notes:
//  * All socket handles in the code are `int`, initialized to -1, and validity
//    is checked with `fd < 0`. WinSock's INVALID_SOCKET is (SOCKET)~0
//    (0xFFFFFFFF); assigning it to an int yields -1, so those checks still work.
//    Valid socket numbers are small and convert int<->SOCKET losslessly.
//  * close() is only ever used for sockets here (files use fclose), so it maps
//    cleanly to closesocket().
//  * Every standard header the .cpp includes is pulled in HERE, before the
//    POSIX-name macros are defined, so a later `#include <thread>` in the .cpp
//    is skipped by include guards and its internal errno/close usage is not
//    corrupted by our macros.
#pragma once

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>   // _mkdir

// Standard headers (see note above: must precede the macro definitions).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sstream>
#include <string>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <sys/types.h>

// ---- POSIX name -> WinSock/Win32 mappings --------------------------------
// Socket close.
#define close(fd)               closesocket((SOCKET)(fd))
// Socket errors live in WSAGetLastError(), not errno. All errno reads in the
// code are socket-related, so remap the identifier. (MinGW's <errno.h> may
// already define errno as a macro -- undef first to avoid a redefinition warning.)
#undef errno
#define errno                   WSAGetLastError()
#define strerror(e)             x4mp_winsock_strerror((e))
// poll() maps to WSAPoll(). winsock2.h already defines `struct pollfd`
// (fd/events/revents, identical to POSIX) as the WSAPOLLFD typedef, so only the
// function name needs remapping.
#define poll                    WSAPoll
// Filesystem.
#define mkdir(p, m)             _mkdir((p))
#define usleep(us)              Sleep(((us) + 999) / 1000)
// Non-blocking: replaces the POSIX fcntl(F_GETFL/F_SETFL|O_NONBLOCK) pair.
#define fcntl(fd, cmd, arg)     (((cmd) == F_SETFL) ? x4mp_sock_set_nonblocking((fd)) : 0)
#define F_GETFL                 1
#define F_SETFL                 2
#define O_NONBLOCK              0
// WinSock uses int for optlen/fromlen; the code declares them socklen_t.
#define socklen_t               int
// WinSock set/getsockopt take char* (not POSIX void*) for the option value.
#define setsockopt(s, l, o, v, lv)  setsockopt((SOCKET)(s), (l), (o), (const char*)(v), (int)(lv))
#define getsockopt(s, l, o, v, lv)  getsockopt((SOCKET)(s), (l), (o), (char*)(v), (int*)(lv))
// Flags / types absent from WinSock.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL            0
#endif
#ifndef ssize_t
#define ssize_t                 int
#endif
// Error constants -> WinSock equivalents.
#undef EINTR
#undef EAGAIN
#undef EWOULDBLOCK
#undef EINPROGRESS
#undef EBADF
#undef EPIPE
#undef ECONNRESET
#undef ECONNABORTED
#define EINTR                   WSAEINTR
#define EAGAIN                  WSAEWOULDBLOCK
#define EWOULDBLOCK             WSAEWOULDBLOCK
#define EINPROGRESS             WSAEINPROGRESS
#define EBADF                   WSAEBADF
#define EPIPE                   WSAECONNRESET
#define ECONNRESET              WSAECONNRESET
#define ECONNABORTED            WSAECONNABORTED
#ifdef _MSC_VER
#define strtok_r               strtok_s
#endif

static inline int x4mp_sock_set_nonblocking(int fd) {
    u_long nb = 1;
    return ioctlsocket((SOCKET)fd, FIONBIO, &nb);
}

static inline const char* x4mp_winsock_strerror(int e) {
    static thread_local char buf[256];
    if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       0, (DWORD)e, 0, buf, (DWORD)sizeof(buf), 0) == 0) {
        buf[0] = 0;
    }
    return buf;
}

// WinSock must be started before any socket call. Idempotent; call from
// x4native_init (after DLL load, so not under the loader lock).
static inline int x4mp_sock_ensure_init(void) {
    static WSADATA wsa;
    static bool done = false;
    if (!done) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        done = true;
    }
    return 0;
}

static inline bool x4mp_connect_pending(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}

// dirent stubs: net_dump_fds() is Linux-only diagnostics (reads /proc/self/fd).
// opendir() returns null so the function no-ops on Windows.
typedef void DIR;
struct dirent { char d_name[256]; };
static inline DIR*            opendir(const char*) { return nullptr; }
static inline struct dirent*  readdir(DIR*)        { return nullptr; }
static inline int             closedir(DIR*)       { return 0; }
static inline ssize_t         readlink(const char*, char*, size_t) { return -1; }
#define X4MP_NO_PROC_FD 1

#else
// POSIX (Linux): pull in the real headers the code relies on. Nothing to remap.
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#define X4MP_PLATFORM_POSIX 1

// POSIX: WinSock is not a thing; socket() works immediately.
static inline int x4mp_sock_ensure_init(void) { return 0; }
static inline bool x4mp_connect_pending(int error) {
    return error == EINPROGRESS || error == EALREADY;
}

#endif

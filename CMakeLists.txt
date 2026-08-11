# ---------------------------------------------------------------------------
# CMakeLists.txt — X4Native Linux build (port of the Windows extension)
#
# Produces two shared objects consumed by the X4 Foundations Linux client:
#   libx4native_64.so    — thin proxy (stable ABI, loaded by the game)
#   libx4native_core.so  — hot-reloadable framework core
#
# Build:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   # outputs land in native/ (libx4native_64.so, libx4native_core.so)
#
# Differences from the Windows build:
#   * No MinHook — Linux uses a built-in mprotect-based inline detour
#     (see core/hook_manager.cpp, HookProvider::linux_detour).
#   * SEH (__try/__except) replaced by POSIX sigsetjmp/siglongjmp guards
#     (see common/platform.h, X4N_TRY / X4N_EXCEPT).
#   * Windows Version API replaced by a readlink(/proc/self/exe) implementation.
#   * Lua symbols resolved from libluajit-5.1.so.2 shipped with the game.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(x4native VERSION 9.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This port targets Linux only.")
endif()

# ---------------------------------------------------------------------------
# Feature flags
# ---------------------------------------------------------------------------
option(X4N_WITH_RELOAD "Compile in core hot-reload support (proxy)" OFF)

# Output shared objects directly into native/ so the game's loader finds them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/native")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/native")

# ---------------------------------------------------------------------------
# Dependencies — nlohmann/json is vendored locally (third_party/nlohmann).
# This avoids a network fetch at configure time.
# ---------------------------------------------------------------------------
set(NLOHMANN_JSON_INCLUDE "${CMAKE_SOURCE_DIR}/third_party")

# ---------------------------------------------------------------------------
# Proxy .so — thin, stable
# ---------------------------------------------------------------------------
add_library(x4native_64 SHARED
    src/proxy/proxy.cpp
    src/common/lua_api.cpp
)

target_compile_definitions(x4native_64 PRIVATE
    X4N_PLATFORM_LINUX
    $<$<BOOL:${X4N_WITH_RELOAD}>:X4N_WITH_RELOAD=1>
)

target_include_directories(x4native_64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/sdk
    ${NLOHMANN_JSON_INCLUDE}
)

set_target_properties(x4native_64 PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "x4native_64"
    SUFFIX ".so"
)

# ---------------------------------------------------------------------------
# Core .so — hot-reloadable framework logic
# ---------------------------------------------------------------------------
add_library(x4native_core SHARED
    src/core/core.cpp
    src/core/logger.cpp
    src/core/event_system.cpp
    src/core/extension_manager.cpp
    src/core/game_api.cpp
    src/core/hook_manager.cpp
    src/core/settings_manager.cpp
    src/core/version.cpp
    src/common/winfile_shim.cpp
    src/common/minhook_shim.cpp
    src/common/platform_impl.cpp
)

target_compile_definitions(x4native_core PRIVATE X4N_PLATFORM_LINUX)

target_include_directories(x4native_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/sdk
    ${NLOHMANN_JSON_INCLUDE}
)

target_link_libraries(x4native_core PRIVATE
    ${CMAKE_DL_LIBS}   # dlopen / dlsym / dlerror
)

set_target_properties(x4native_core PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "x4native_core"
    SUFFIX ".so"
)

# ---------------------------------------------------------------------------
# Install rules — targets go into native/
# ---------------------------------------------------------------------------
install(TARGETS x4native_64 x4native_core
    LIBRARY DESTINATION native
)
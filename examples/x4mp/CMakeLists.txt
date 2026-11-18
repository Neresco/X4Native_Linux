cmake_minimum_required(VERSION 3.20)
project(x4mp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows (MinGW cross-build): statically link the MinGW runtime so the DLL only
# depends on Windows system libs (KERNEL32, UCRT, WS2_32). Must be set before
# add_library() so it lands at the start of the shared link command.
if(MINGW)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")
endif()

add_library(x4mp SHARED x4mp.cpp)

target_include_directories(x4mp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/sdk
)

if(WIN32)
    target_link_libraries(x4mp PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(x4mp PRIVATE Threads::Threads)
endif()

set_target_properties(x4mp PROPERTIES
    PREFIX ""
    OUTPUT_NAME "x4mp"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(x4mp_host_lifecycle_test tests/host_lifecycle.cpp)
    target_include_directories(x4mp_host_lifecycle_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/sdk
    )
    if(WIN32)
        target_link_libraries(x4mp_host_lifecycle_test PRIVATE ws2_32)
    else()
        target_link_libraries(x4mp_host_lifecycle_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME x4mp_host_lifecycle COMMAND x4mp_host_lifecycle_test)
    add_executable(x4mp_client_lifecycle_test tests/client_lifecycle.cpp)
    target_include_directories(x4mp_client_lifecycle_test PRIVATE ../../sdk ../../sdk/sdk)
    if(WIN32)
        target_link_libraries(x4mp_client_lifecycle_test PRIVATE ws2_32)
    else()
        target_link_libraries(x4mp_client_lifecycle_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME x4mp_client_lifecycle COMMAND x4mp_client_lifecycle_test)
endif()

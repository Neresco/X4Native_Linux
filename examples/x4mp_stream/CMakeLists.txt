cmake_minimum_required(VERSION 3.20)
project(x4mp_stream)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(x4mp_stream SHARED x4mp_stream.cpp)

target_include_directories(x4mp_stream PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/sdk
)

target_link_libraries(x4mp_stream PRIVATE pthread)

set_target_properties(x4mp_stream PROPERTIES
    PREFIX ""
    OUTPUT_NAME "x4mp_stream"
)

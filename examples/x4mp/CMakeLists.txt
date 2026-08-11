cmake_minimum_required(VERSION 3.20)
project(x4mp)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(x4mp SHARED x4mp.cpp)

target_include_directories(x4mp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/sdk
)

set_target_properties(x4mp PROPERTIES
    PREFIX ""
    OUTPUT_NAME "x4mp"
)
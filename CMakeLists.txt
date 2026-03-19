cmake_minimum_required(VERSION 3.16)

project(Project2HighPerfWebServer LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING "Build type" FORCE)
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug
    Release
    ASanUBSan
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(webserver_options INTERFACE)
target_compile_features(webserver_options INTERFACE cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(webserver_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# [Week3 Day4] Begin:
# Day4 只补 asan_ubsan 口径，不提前接 tsan 或 wrk。
# 这里把构建口径直接固化到 CMake 的一个独立 build type 里，方便后续统一复用。
if(CMAKE_BUILD_TYPE STREQUAL "ASanUBSan")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "ASanUBSan build type currently requires GCC or Clang")
    endif()

    target_compile_options(webserver_options INTERFACE
        -O1
        -g
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
    target_link_options(webserver_options INTERFACE
        -fsanitize=address,undefined
    )
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
# [Week3 Day4] End

add_subdirectory(src)

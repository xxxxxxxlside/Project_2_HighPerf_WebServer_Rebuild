cmake_minimum_required(VERSION 3.16)

project(Project2HighPerfWebServer LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

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

add_subdirectory(src)

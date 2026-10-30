cmake_minimum_required(VERSION 3.25)

project(axon
    VERSION 0.1.0
    DESCRIPTION "Parallel, resumable file transfer over TCP"
    LANGUAGES CXX)

option(AXON_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(AXON_BUILD_TESTS "Build the test suite" ON)
set(AXON_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. 'address,undefined' or 'thread' (empty = none)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # -std=c++20, not gnu++20

# Settings shared by every Axon target (not applied to third-party code).
add_library(axon_options INTERFACE)
target_compile_features(axon_options INTERFACE cxx_std_20)
target_compile_options(axon_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wnon-virtual-dtor
    $<$<BOOL:${AXON_WARNINGS_AS_ERRORS}>:-Werror>)

if(AXON_SANITIZE)
    target_compile_options(axon_options INTERFACE
        -fsanitize=${AXON_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(axon_options INTERFACE -fsanitize=${AXON_SANITIZE})
endif()

# Core library: everything except the CLI lives here so tests can link it.
add_library(axon_core
    src/fd.cpp
    src/net/socket.cpp
    src/storage/file.cpp
    src/version.cpp)
target_include_directories(axon_core PUBLIC include PRIVATE src)
target_compile_definitions(axon_core PRIVATE AXON_VERSION="${PROJECT_VERSION}")
target_link_libraries(axon_core PUBLIC axon_options)

add_executable(axon apps/axon/main.cpp)
target_link_libraries(axon PRIVATE axon_core)

if(AXON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

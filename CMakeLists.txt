# Eigenbook top-level CMakeLists.txt: project, options and add_subdirectory only.
#
# CMake 3.25 is the minimum because the build uses
#   - target_sources(FILE_SET HEADERS) for the header-only library and its install rules (3.23),
#   - SYSTEM on FetchContent_Declare, so test-only dependency headers do not trigger our
#     warnings (3.25),
#   - CMakePresets.json schema version 6 (3.25).
cmake_minimum_required(VERSION 3.25)

project(Eigenbook
        VERSION 0.1.0
        DESCRIPTION "Low-latency limit order book in C++23"
        LANGUAGES CXX)

# Developer targets default to ON only when Eigenbook is the top-level project, so a consumer
# that uses add_subdirectory / FetchContent gets the library and nothing else.
option(EIGENBOOK_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(EIGENBOOK_BUILD_APPS "Build the example applications in apps/" ${PROJECT_IS_TOP_LEVEL})
option(EIGENBOOK_BUILD_BENCH "Build the benchmarks in bench/" ${PROJECT_IS_TOP_LEVEL})
option(EIGENBOOK_INSTALL "Generate install and package export rules" ${PROJECT_IS_TOP_LEVEL})
option(EIGENBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(EIGENBOOK_LONG_TESTS "Run the long randomised differential tests" OFF)
set(EIGENBOOK_SANITIZE "" CACHE STRING
    "Sanitizers for every target in this build: address;undefined, or thread (empty = none)")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_subdirectory(src)

if (EIGENBOOK_BUILD_APPS)
    add_subdirectory(apps/demo)
endif ()

# The bench support library (harness + workloads) is also used by the tests.
if (EIGENBOOK_BUILD_BENCH OR EIGENBOOK_BUILD_TESTS)
    add_subdirectory(bench)
endif ()

if (EIGENBOOK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (EIGENBOOK_INSTALL)
    include(EigenbookInstall)
endif ()

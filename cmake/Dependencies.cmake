# Test and benchmark dependencies. The libraries themselves have no third-party dependencies.
#
# Every dependency is pinned to a commit SHA (not a tag, which can be moved). To build offline,
# point FetchContent at a local checkout of the same commit:
#   -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/path/to/googletest

include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59 # v1.15.2
        SYSTEM
)

# Same CRT as the project on MSVC; no gmock; keep GoogleTest out of our install tree.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

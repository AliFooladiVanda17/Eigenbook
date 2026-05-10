include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
)

FetchContent_MakeAvailable(googletest)

add_executable(hpc_queue_tests
        hpc_queue_tests.cpp
)

target_link_libraries(hpc_queue_tests
        PRIVATE
        hpc_queue
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(hpc_queue_tests)
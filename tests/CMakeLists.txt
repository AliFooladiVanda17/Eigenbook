include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
)

FetchContent_MakeAvailable(googletest)

add_executable(TradingEngine_tests
        TradingEngine_tests.cpp
)

target_link_libraries(TradingEngine_tests
        PRIVATE
        TradingEngine
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(TradingEngine_tests)
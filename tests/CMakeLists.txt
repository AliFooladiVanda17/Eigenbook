include(FetchContent)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
)

# Keep GoogleTest on the same CRT as the project on MSVC.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(TradingEngine_tests
        TestSupport.h
        Types_tests.cpp
        PriceLevel_tests.cpp
        PriceLadder_tests.cpp
        OrderBook_tests.cpp
        Util_tests.cpp
        Differential_tests.cpp
)

target_link_libraries(TradingEngine_tests
        PRIVATE
        TradingEngine
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(TradingEngine_tests)

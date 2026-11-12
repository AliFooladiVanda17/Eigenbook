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
        support/test_support.hpp
        unit/types_test.cpp
        unit/price_level_test.cpp
        unit/price_ladder_test.cpp
        unit/order_book_test.cpp
        unit/util_test.cpp
        differential/differential_test.cpp
)

target_link_libraries(TradingEngine_tests
        PRIVATE
        TradingEngine
        GTest::gtest_main
)

target_include_directories(TradingEngine_tests PRIVATE support)

include(GoogleTest)
gtest_discover_tests(TradingEngine_tests)

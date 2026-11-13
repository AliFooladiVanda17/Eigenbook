include(Dependencies)
include(GoogleTest)

add_executable(eigenbook_tests
        support/test_support.hpp
        unit/types_test.cpp
        unit/price_level_test.cpp
        unit/price_ladder_test.cpp
        unit/order_book_test.cpp
        unit/util_test.cpp
        unit/instantiation_test.cpp
        unit/histogram_test.cpp
        unit/workload_test.cpp
        unit/platform_test.cpp
        unit/capacity_test.cpp
        unit/fixed_block_resource_test.cpp
        unit/hybrid_ladder_test.cpp
        unit/locator_test.cpp
        unit/locator_book_test.cpp
        unit/limits_test.cpp
        unit/participant_test.cpp
        differential/differential_test.cpp
)

target_include_directories(eigenbook_tests PRIVATE support)
if (EIGENBOOK_LONG_TESTS)
    # Long differential runs (1,000,000 steps x 5 seeds): minutes, not seconds.
    target_compile_definitions(eigenbook_tests PRIVATE EIGENBOOK_LONG_TESTS)
endif ()

target_link_libraries(eigenbook_tests
        PRIVATE
        Eigenbook::book
        Eigenbook::platform
        eigenbook_bench_support
        eigenbook_warnings
        GTest::gtest_main
)

# Zero-allocation proof (NFR-02): its own executable, because it replaces global operator new.
add_executable(eigenbook_alloc_tests alloc/zero_alloc_test.cpp)
target_link_libraries(eigenbook_alloc_tests PRIVATE eigenbook_bench_support eigenbook_warnings GTest::gtest_main)

# Index probe statistics: its own executable, because EIGENBOOK_INSTRUMENT_INDEX changes
# OrderIndex's layout (mixing it with uninstrumented translation units would violate the ODR).
add_executable(eigenbook_index_instrumentation_tests unit/index_instrumentation_test.cpp)
target_compile_definitions(eigenbook_index_instrumentation_tests PRIVATE EIGENBOOK_INSTRUMENT_INDEX)
target_link_libraries(eigenbook_index_instrumentation_tests
        PRIVATE eigenbook_bench_support eigenbook_warnings GTest::gtest_main)

# The first launch of a freshly linked test binary sometimes took over 5 s on Windows (observed
# as "Process terminated due to timeout" in --gtest_list_tests; later launches take ~0.1 s), so
# the default 5 s discovery timeout is raised. PRE_TEST lists the tests when ctest runs, so a slow
# discovery can no longer fail the build itself.
foreach (target eigenbook_tests eigenbook_alloc_tests eigenbook_index_instrumentation_tests)
    eigenbook_copy_sanitizer_runtime(${target})
    gtest_discover_tests(${target}
            DISCOVERY_MODE PRE_TEST
            DISCOVERY_TIMEOUT 60
    )
endforeach ()

# Installs this build into a scratch prefix, then configures, builds and runs
# tests/package_consumer against it with find_package(Eigenbook).
if (EIGENBOOK_INSTALL)
    add_test(NAME package_consumer
            COMMAND ${CMAKE_COMMAND}
            -D EIGENBOOK_BINARY_DIR=${PROJECT_BINARY_DIR}
            -D CONFIG=$<CONFIG>
            -D PREFIX=${CMAKE_CURRENT_BINARY_DIR}/package_consumer/install
            -D CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_consumer
            -D CONSUMER_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package_consumer/build
            -D GENERATOR=${CMAKE_GENERATOR}
            -D GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -D CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -D MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/package_consumer/run_consumer_test.cmake
    )
    set_tests_properties(package_consumer PROPERTIES LABELS package)
endif ()

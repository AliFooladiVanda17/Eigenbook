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
        differential/differential_test.cpp
)

target_include_directories(eigenbook_tests PRIVATE support)

target_link_libraries(eigenbook_tests
        PRIVATE
        Eigenbook::book
        eigenbook_warnings
        GTest::gtest_main
)

eigenbook_copy_sanitizer_runtime(eigenbook_tests)

# The first launch of a freshly linked test binary sometimes took over 5 s on Windows (observed
# as "Process terminated due to timeout" in --gtest_list_tests; later launches take ~0.1 s), so
# the default 5 s discovery timeout is raised. PRE_TEST lists the tests when ctest runs, so a slow
# discovery can no longer fail the build itself.
gtest_discover_tests(eigenbook_tests
        DISCOVERY_MODE PRE_TEST
        DISCOVERY_TIMEOUT 60
)

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

# Benchmarks. Not part of the installed package.
#
# eigenbook_bench_support: header-only harness (timer use, histogram, executor, environment) and
# seeded workloads. Also used by the tests (histogram, workload and zero-allocation tests), so it
# is defined whenever tests or benchmarks are built.

add_library(eigenbook_bench_support INTERFACE)
target_include_directories(eigenbook_bench_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(eigenbook_bench_support INTERFACE Eigenbook::book Eigenbook::platform)

if (EIGENBOOK_BUILD_BENCH)
    add_subdirectory(latency)
endif ()

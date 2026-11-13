# Library targets.
#
# Eigenbook::book      header-only order book (INTERFACE)
# Eigenbook::platform  OS / CPU facilities for benchmarks and, later, the engine (STATIC)
# Eigenbook::eigenbook umbrella target that links every public library

add_library(eigenbook_book INTERFACE)
add_library(Eigenbook::book ALIAS eigenbook_book)
set_target_properties(eigenbook_book PROPERTIES EXPORT_NAME book)

target_sources(eigenbook_book
        INTERFACE
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES
        ${PROJECT_SOURCE_DIR}/include/eigenbook/core/types.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/core/order.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/core/reject_reason.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/core/book_limits.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/core/checked_math.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/price_level.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/price_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/ladder/map_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/ladder/dense_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/ladder/hybrid_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/locator/order_locator.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/locator/hash_order_index.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/locator/hardened_hash_index.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/locator/direct_order_table.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/order_book.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/util/level_bitmap.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/util/object_pool.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/util/fixed_block_resource.hpp
)

target_compile_features(eigenbook_book INTERFACE cxx_std_23)

# --- Eigenbook::platform ------------------------------------------------------------------------
# One module for everything OS- or CPU-specific; Windows and Linux implementations.

add_library(eigenbook_platform STATIC
        platform/common.cpp
        platform/cpu_x86.cpp
)
add_library(Eigenbook::platform ALIAS eigenbook_platform)
set_target_properties(eigenbook_platform PROPERTIES EXPORT_NAME platform)

if (WIN32)
    target_sources(eigenbook_platform PRIVATE
            platform/windows/affinity.cpp
            platform/windows/system_info.cpp)
    target_link_libraries(eigenbook_platform PRIVATE powrprof)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(eigenbook_platform PRIVATE
            platform/linux/affinity.cpp
            platform/linux/system_info.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(eigenbook_platform PUBLIC Threads::Threads)
else ()
    message(FATAL_ERROR "Eigenbook::platform supports Windows and Linux (got ${CMAKE_SYSTEM_NAME})")
endif ()

target_sources(eigenbook_platform
        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES
        ${PROJECT_SOURCE_DIR}/include/eigenbook/platform/tsc.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/platform/cpu_info.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/platform/affinity.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/platform/system_info.hpp
)

target_compile_features(eigenbook_platform PUBLIC cxx_std_23)
# Our warning set while building the library itself; not part of the exported interface.
target_link_libraries(eigenbook_platform PRIVATE $<BUILD_INTERFACE:eigenbook_warnings>)

# --- Umbrella -----------------------------------------------------------------------------------

add_library(eigenbook INTERFACE)
add_library(Eigenbook::eigenbook ALIAS eigenbook)
target_link_libraries(eigenbook INTERFACE eigenbook_book eigenbook_platform)

# --- Guard: every public header is installed ------------------------------------------------------
# A header missing from a FILE_SET still works in this build tree (the include directory is on the
# path) but is not installed, which only the package_consumer test would notice. Fail at configure
# time instead.
file(GLOB_RECURSE _eigenbook_public_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/eigenbook/*.hpp)
get_target_property(_eigenbook_book_headers eigenbook_book HEADER_SET_HEADERS)
get_target_property(_eigenbook_platform_headers eigenbook_platform HEADER_SET_HEADERS)
foreach (header IN LISTS _eigenbook_public_headers)
    if (NOT header IN_LIST _eigenbook_book_headers AND NOT header IN_LIST _eigenbook_platform_headers)
        message(FATAL_ERROR "Public header not in any FILE_SET (it would not be installed): ${header}")
    endif ()
endforeach ()

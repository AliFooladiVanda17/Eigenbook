# Library targets.
#
# Eigenbook::book      header-only order book (INTERFACE)
# Eigenbook::eigenbook umbrella target that links every public library
#
# Eigenbook::platform (STATIC, src/platform/*.cpp) is added together with its first sources.

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
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/price_level.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/price_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/ladder/map_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/ladder/dense_ladder.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/locator/hash_order_index.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/book/order_book.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/util/level_bitmap.hpp
        ${PROJECT_SOURCE_DIR}/include/eigenbook/util/object_pool.hpp
)

target_compile_features(eigenbook_book INTERFACE cxx_std_23)

add_library(eigenbook INTERFACE)
add_library(Eigenbook::eigenbook ALIAS eigenbook)
target_link_libraries(eigenbook INTERFACE eigenbook_book)

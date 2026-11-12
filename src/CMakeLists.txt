add_library(TradingEngine STATIC
        engine/TradingEngine.cpp
        ../include/tradingengine/TradingEngine.h
        ../include/eigenbook/core/types.hpp
        ../include/eigenbook/core/order.hpp
        book/orderbook.cpp
        ../include/eigenbook/book/order_book.hpp
        ../include/eigenbook/book/locator/hash_order_index.hpp
        ../include/eigenbook/book/price_ladder.hpp
        ../include/eigenbook/book/price_level.hpp
        ../include/eigenbook/util/level_bitmap.hpp
        ../include/eigenbook/util/object_pool.hpp
)

target_include_directories(TradingEngine
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(TradingEngine
        PUBLIC
        cxx_std_23
)

target_compile_options(TradingEngine
        PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

install(TARGETS TradingEngine
        EXPORT TradingEngineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT TradingEngineTargets
        FILE TradingEngineTargets.cmake
        NAMESPACE TradingEngine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TradingEngine
)

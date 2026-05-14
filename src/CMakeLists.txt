add_library(TradingEngine STATIC
        engine/TradingEngine.cpp
        ../include/tradingengine/types.h
        book/order.cpp
        ../include/tradingengine/order.h
        book/OrderBook.cpp
        ../include/tradingengine/OrderBook.h
        book/PriceLadder.cpp
        ../include/tradingengine/PriceLadder.h
        book/PriceLevel.cpp
        ../include/tradingengine/PriceLevel.h
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
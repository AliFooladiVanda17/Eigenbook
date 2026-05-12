add_executable(TradingEngine_app
                main.cpp)

target_link_libraries(TradingEngine_app
        PRIVATE
        TradingEngine
)

target_compile_features(TradingEngine_app
        PRIVATE
        cxx_std_23
)
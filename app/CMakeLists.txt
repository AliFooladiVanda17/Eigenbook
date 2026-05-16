

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

target_compile_options(TradingEngine_app PRIVATE
        $<$<CONFIG:Debug>:-O0 -g3>
)

target_link_options(TradingEngine_app PRIVATE
$<$<CONFIG:Debug>:-g3>
)
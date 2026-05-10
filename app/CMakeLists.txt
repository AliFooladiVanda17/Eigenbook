add_executable(hpc_queue_app main.cpp)

target_link_libraries(hpc_queue_app
        PRIVATE
        hpc_queue
)

target_compile_features(hpc_queue_app
        PRIVATE
        cxx_std_23
)
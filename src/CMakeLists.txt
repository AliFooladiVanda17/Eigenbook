add_library(orderbook STATIC)

target_sources(hpc_queue

        PRIVATE
        hpcqueue.cpp

        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include

        FILES
        ${PROJECT_SOURCE_DIR}/include/hpcqueue/hpcqueue.h
)

target_include_directories(hpc_queue
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(hpc_queue
        PUBLIC
        cxx_std_23
)

target_compile_options(hpc_queue
        PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

install(TARGETS hpc_queue
        EXPORT hpc_queueTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT hpc_queueTargets
        FILE hpc_queueTargets.cmake
        NAMESPACE hpc_queue::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hpc_queue
)
# hpc-queue CMakeLists.txt
cmake_minimum_required(VERSION 3.31)

project(TradingEngine VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

add_subdirectory(src)
add_subdirectory(app)

# Enable test build only when the BUILD_TESTING is ON
if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/hpc_queueConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/orderbookConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/hpc_queueConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hpc_queue
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/hpc_queueConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/hpc_queueConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hpc_queue
)
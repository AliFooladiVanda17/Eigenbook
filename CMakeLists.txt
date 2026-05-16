# hpc-queue CMakeLists.txt
cmake_minimum_required(VERSION 3.31)
set(CMAKE_CXX_STANDARD 23)

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
        ${CMAKE_CURRENT_BINARY_DIR}/TradingEngineConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TradingEngineConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/TradingEngineConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TradingEngine
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/TradingEngineConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/TradingEngineConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TradingEngine
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(TradingEngine
            PRIVATE
            DOCTEST_CONFIG_DISABLE
    )
endif()
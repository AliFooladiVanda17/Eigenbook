# Install and package export: find_package(Eigenbook CONFIG) provides Eigenbook::book and
# Eigenbook::eigenbook.
#
# The package config goes to <libdir>/cmake/Eigenbook and is not marked arch-independent, because
# Eigenbook::platform (a static library) joins the package in Phase A2.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(EIGENBOOK_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Eigenbook"
    CACHE STRING "Install directory for the Eigenbook CMake package files")

install(TARGETS eigenbook_book eigenbook
        EXPORT EigenbookTargets
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT EigenbookTargets
        FILE EigenbookTargets.cmake
        NAMESPACE Eigenbook::
        DESTINATION ${EIGENBOOK_INSTALL_CMAKEDIR}
)

configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/EigenbookConfig.cmake.in
        ${PROJECT_BINARY_DIR}/EigenbookConfig.cmake
        INSTALL_DESTINATION ${EIGENBOOK_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
        ${PROJECT_BINARY_DIR}/EigenbookConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

install(FILES
        ${PROJECT_BINARY_DIR}/EigenbookConfig.cmake
        ${PROJECT_BINARY_DIR}/EigenbookConfigVersion.cmake
        DESTINATION ${EIGENBOOK_INSTALL_CMAKEDIR}
)

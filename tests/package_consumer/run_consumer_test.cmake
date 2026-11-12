# Driver for the package_consumer CTest test (cmake -P). Inputs come from tests/CMakeLists.txt.
#
# 1. install the Eigenbook build tree into a fresh PREFIX,
# 2. configure tests/package_consumer in a fresh directory with only PREFIX on the search path,
# 3. build it and run both executables.

foreach (var EIGENBOOK_BINARY_DIR PREFIX CONSUMER_SOURCE_DIR CONSUMER_BINARY_DIR GENERATOR)
    if (NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "run_consumer_test.cmake: ${var} is not set")
    endif ()
endforeach ()

if ("${CONFIG}" STREQUAL "")
    set(CONFIG Release)
endif ()

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if (NOT rc EQUAL 0)
        list(JOIN ARGN " " cmd)
        message(FATAL_ERROR "package_consumer: command failed (${rc}): ${cmd}")
    endif ()
endfunction()

file(REMOVE_RECURSE "${PREFIX}" "${CONSUMER_BINARY_DIR}")

run(${CMAKE_COMMAND} --install "${EIGENBOOK_BINARY_DIR}" --config ${CONFIG} --prefix "${PREFIX}")

set(configure_args
        -S "${CONSUMER_SOURCE_DIR}" -B "${CONSUMER_BINARY_DIR}" -G "${GENERATOR}"
        "-DCMAKE_PREFIX_PATH=${PREFIX}"
        "-DCMAKE_BUILD_TYPE=${CONFIG}"
        # Find the package only through PREFIX, never through a registry or the source tree.
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
if (NOT "${GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND configure_args -A "${GENERATOR_PLATFORM}")
endif ()
# Visual Studio generators pick the compiler and build tool themselves.
if (NOT GENERATOR MATCHES "^Visual Studio")
    if (NOT "${CXX_COMPILER}" STREQUAL "")
        list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
    endif ()
    if (NOT "${MAKE_PROGRAM}" STREQUAL "")
        list(APPEND configure_args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
    endif ()
endif ()

run(${CMAKE_COMMAND} ${configure_args})
run(${CMAKE_COMMAND} --build "${CONSUMER_BINARY_DIR}" --config ${CONFIG})

foreach (exe package_consumer package_consumer_umbrella)
    file(GLOB_RECURSE found "${CONSUMER_BINARY_DIR}/${exe}" "${CONSUMER_BINARY_DIR}/${exe}.exe")
    list(FILTER found INCLUDE REGEX "/${exe}(\\.exe)?$")
    if (NOT found)
        message(FATAL_ERROR "package_consumer: ${exe} was not built")
    endif ()
    list(GET found 0 path)
    run("${path}")
endforeach ()

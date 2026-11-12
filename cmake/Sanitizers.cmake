# Sanitizers, selected with EIGENBOOK_SANITIZE (a CMake list: "address;undefined" or "thread").
#
# Applied to every target in the build, dependencies included: mixing instrumented and
# uninstrumented code breaks ASan's container annotations on MSVC's STL and hides bugs elsewhere.
#
# Provides eigenbook_copy_sanitizer_runtime(<target>): on Windows with Clang, copies the ASan
# runtime DLL next to the executable after it links, so the binary runs from any shell, from
# ctest and from GoogleTest's discovery step (which does not take a modified PATH).

set(EIGENBOOK_SANITIZER_RUNTIME_DLL "")

function(eigenbook_copy_sanitizer_runtime target)
    if (EIGENBOOK_SANITIZER_RUNTIME_DLL)
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EIGENBOOK_SANITIZER_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
    endif ()
endfunction()

if (NOT EIGENBOOK_SANITIZE)
    return()
endif ()

if ("thread" IN_LIST EIGENBOOK_SANITIZE AND
        ("address" IN_LIST EIGENBOOK_SANITIZE OR "undefined" IN_LIST EIGENBOOK_SANITIZE))
    message(FATAL_ERROR "EIGENBOOK_SANITIZE: 'thread' cannot be combined with 'address' or 'undefined'")
endif ()

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # cl.exe supports AddressSanitizer only.
    if (NOT EIGENBOOK_SANITIZE STREQUAL "address")
        message(FATAL_ERROR "MSVC supports EIGENBOOK_SANITIZE=address only (got '${EIGENBOOK_SANITIZE}')")
    endif ()
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)
    # MSVC ships its ASan runtime next to cl.exe.
    get_filename_component(_eigenbook_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(EIGENBOOK_SANITIZER_RUNTIME_DLL "${_eigenbook_cl_dir}/clang_rt.asan_dynamic-x86_64.dll")
    if (NOT EXISTS "${EIGENBOOK_SANITIZER_RUNTIME_DLL}")
        message(FATAL_ERROR "MSVC ASan runtime not found: ${EIGENBOOK_SANITIZER_RUNTIME_DLL}")
    endif ()
    return()
endif ()

if (WIN32 AND "thread" IN_LIST EIGENBOOK_SANITIZE)
    message(FATAL_ERROR "ThreadSanitizer is not available on Windows")
endif ()

list(JOIN EIGENBOOK_SANITIZE "," _eigenbook_sanitizers)
add_compile_options(-fsanitize=${_eigenbook_sanitizers} -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=${_eigenbook_sanitizers})
if ("undefined" IN_LIST EIGENBOOK_SANITIZE)
    # Make UB fatal so the test run fails instead of printing and continuing.
    add_compile_options(-fno-sanitize-recover=all)
endif ()

if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND "address" IN_LIST EIGENBOOK_SANITIZE)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
            OUTPUT_VARIABLE _eigenbook_resource_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    file(TO_CMAKE_PATH
            "${_eigenbook_resource_dir}/lib/windows/clang_rt.asan_dynamic-x86_64.dll"
            EIGENBOOK_SANITIZER_RUNTIME_DLL)
    if (NOT EXISTS "${EIGENBOOK_SANITIZER_RUNTIME_DLL}")
        message(FATAL_ERROR "ASan runtime not found: ${EIGENBOOK_SANITIZER_RUNTIME_DLL}")
    endif ()
endif ()

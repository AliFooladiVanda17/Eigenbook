# eigenbook_warnings: INTERFACE target with the project's warning set.
#
# Tests, apps and benchmarks link it PRIVATE, so the public headers are compiled under these flags
# without imposing them on consumers of the installed package.

add_library(eigenbook_warnings INTERFACE)

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # cl.exe, and clang-cl (which also understands the GCC-style -W flags below).
    target_compile_options(eigenbook_warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(eigenbook_warnings INTERFACE
                -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast)
    endif ()
    if (EIGENBOOK_WARNINGS_AS_ERRORS)
        target_compile_options(eigenbook_warnings INTERFACE /WX)
    endif ()
else ()
    target_compile_options(eigenbook_warnings INTERFACE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast)
    if (EIGENBOOK_WARNINGS_AS_ERRORS)
        target_compile_options(eigenbook_warnings INTERFACE -Werror)
    endif ()
endif ()

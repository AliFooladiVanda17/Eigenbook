add_executable(eigenbook_demo main.cpp)

target_link_libraries(eigenbook_demo
        PRIVATE
        Eigenbook::book
        eigenbook_warnings
)

eigenbook_copy_sanitizer_runtime(eigenbook_demo)

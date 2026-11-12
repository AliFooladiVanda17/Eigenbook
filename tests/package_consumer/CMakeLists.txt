# Smoke test for the installed package: a separate project that only knows the install prefix.
cmake_minimum_required(VERSION 3.25)

project(EigenbookPackageConsumer LANGUAGES CXX)

find_package(Eigenbook 0.1 CONFIG REQUIRED)

add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE Eigenbook::book)

# The umbrella target must resolve too.
add_executable(package_consumer_umbrella main.cpp)
target_link_libraries(package_consumer_umbrella PRIVATE Eigenbook::eigenbook)

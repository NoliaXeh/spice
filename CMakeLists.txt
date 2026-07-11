cmake_minimum_required(VERSION 3.20)

project(spice LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

enable_testing()

# Each module under src/ builds to its own static library, with headers in
# inc/spice/<module> and tests in tests/<module>. See README.md.
add_subdirectory(src/core)

add_executable(spice src/main.cpp)
target_link_libraries(spice PRIVATE core)

cmake_minimum_required(VERSION 3.20)

project(spice VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -O3)

enable_testing()

# Each module under src/ builds to its own static library, with headers in
# inc/spice/<module> and tests in tests/<module>. See README.md.
add_subdirectory(src/core)
add_subdirectory(src/config)
add_subdirectory(src/plugin)

add_executable(spice src/main.cpp)
target_link_libraries(spice PRIVATE core config plugin)

add_subdirectory(plugins/greeter)
target_compile_definitions(spice PRIVATE SPICE_VERSION="${PROJECT_VERSION}")

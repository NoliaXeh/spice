add_library(config STATIC
    KeyName.cpp
    Config.cpp
)
set_target_properties(config PROPERTIES PREFIX "")

target_include_directories(config PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)
# toml++ is an implementation detail: private, and absent from public headers
target_include_directories(config PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/tomlplusplus
)

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/config)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/config ${CMAKE_BINARY_DIR}/tests/config)
endif()

add_library(plugin STATIC
    MsgPack.cpp
    Message.cpp
    PluginProcess.cpp
    PluginHost.cpp
)
set_target_properties(plugin PROPERTIES PREFIX "")

target_include_directories(plugin PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/plugin)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/plugin ${CMAKE_BINARY_DIR}/tests/plugin)
endif()

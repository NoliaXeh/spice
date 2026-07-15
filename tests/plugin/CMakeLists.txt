add_executable(plugin_test
    test_MsgPack.cpp
    test_Message.cpp
    test_PluginHost.cpp
)

target_link_libraries(plugin_test PRIVATE plugin)

target_include_directories(plugin_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

# the reference plugin the host tests spawn (built by the top-level project)
target_compile_definitions(plugin_test PRIVATE
    GREETER_PATH="${CMAKE_BINARY_DIR}/plugins/greeter/spice-greeter"
)

add_test(NAME plugin_test COMMAND plugin_test)

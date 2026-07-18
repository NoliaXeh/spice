add_executable(plugin_test
    test_MsgPack.cpp
    test_Message.cpp
    test_PluginHost.cpp
)

target_link_libraries(plugin_test PRIVATE plugin)

target_include_directories(plugin_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

# the reference plugins the host tests spawn: the built greeter, and the
# Python highlighter straight from the source tree
target_compile_definitions(plugin_test PRIVATE
    GREETER_PATH="${CMAKE_BINARY_DIR}/plugins/greeter/spice-greeter"
    CPP_KEYWORDS_PATH="${CMAKE_SOURCE_DIR}/plugins/cpp-keywords/cpp_keywords.py"
)

add_test(NAME plugin_test COMMAND plugin_test)

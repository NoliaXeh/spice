add_executable(core_test
    test_Spice.cpp
    test_TermInfo.cpp
    test_Grid.cpp
    test_Theme.cpp
    test_EventParser.cpp
    test_EventReader.cpp
    test_Buffer.cpp
    test_Layout.cpp
    test_Pane.cpp
    test_Command.cpp
    test_Palette.cpp
    test_Pty.cpp
)

target_link_libraries(core_test PRIVATE core)

target_include_directories(core_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME core_test COMMAND core_test)

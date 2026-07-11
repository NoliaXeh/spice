add_executable(core_test
    test_Spice.cpp
    test_TermInfo.cpp
    test_Grid.cpp
    test_Theme.cpp
    test_EventParser.cpp
    test_EventReader.cpp
)

target_link_libraries(core_test PRIVATE core)

target_include_directories(core_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME core_test COMMAND core_test)

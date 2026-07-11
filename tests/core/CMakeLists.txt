add_executable(core_test
    test_Spice.cpp
)

target_link_libraries(core_test PRIVATE core)

target_include_directories(core_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME core_test COMMAND core_test)

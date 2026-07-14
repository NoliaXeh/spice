add_executable(config_test
    test_Config.cpp
    test_KeyName.cpp
)

target_link_libraries(config_test PRIVATE config)

target_include_directories(config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME config_test COMMAND config_test)

add_library(core STATIC
    Spice.cpp
    TermInfo.cpp
    Grid.cpp
    Theme.cpp
    EventParser.cpp
    EventReader.cpp
    Buffer.cpp
    Layout.cpp
    Pane.cpp
)
set_target_properties(core PROPERTIES PREFIX "")

target_include_directories(core PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/core)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/core ${CMAKE_BINARY_DIR}/tests/core)
endif()

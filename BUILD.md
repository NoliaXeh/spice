# Building Spice

Spice uses CMake. No C++20 modules are involved anywhere in this file - "module" always means
a `src/<name>` + `inc/spice/<name>` + `tests/<name>` group, built as a plain static library.

## Building

```sh
cmake -S . -B build
cmake --build build -j
```

This produces:

- `build/spice` - the main executable (`src/main.cpp` linked against every module library).
- `build/src/<module>/<module>.a` - the static library for each module.
- `build/tests/<module>/<module>_test` - the test executable for each module.

`compile_commands.json` is emitted into `build/` for clangd/IDE tooling.

## Running tests

```sh
ctest --test-dir build --output-on-failure
```

Or run a single module's test binary directly, e.g. `./build/tests/core/core_test`.

## Adding a file to an existing module

Add the `.cpp` to `src/<module>/CMakeLists.txt`'s `add_library(...)` call (headers go in
`inc/spice/<module>` and don't need to be listed - they're picked up via the include path).
Add a `.cpp` to the matching `tests/<module>/CMakeLists.txt`'s `add_executable(...)` call if it has
tests.

CMake does not glob sources here - new files must be added to the relevant `CMakeLists.txt` by
hand. This is deliberate: a typo'd filename should fail to configure, not silently fail to build.

## Adding a new module

Say you're adding a `palette` module. You need:

```
src/palette/CMakeLists.txt
src/palette/Palette.cpp
inc/spice/palette/Palette.hpp
tests/palette/CMakeLists.txt
tests/palette/test_Palette.cpp
```

`src/palette/CMakeLists.txt`, modeled on `src/core/CMakeLists.txt`:

```cmake
add_library(palette STATIC
    Palette.cpp
)
set_target_properties(palette PROPERTIES PREFIX "")

target_include_directories(palette PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)

if(EXISTS ${CMAKE_SOURCE_DIR}/tests/palette)
    add_subdirectory(${CMAKE_SOURCE_DIR}/tests/palette ${CMAKE_BINARY_DIR}/tests/palette)
endif()
```

`tests/palette/CMakeLists.txt`, modeled on `tests/core/CMakeLists.txt`:

```cmake
add_executable(palette_test
    test_Palette.cpp
)

target_link_libraries(palette_test PRIVATE palette)

target_include_directories(palette_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
)

add_test(NAME palette_test COMMAND palette_test)
```

Then wire the module into the root `CMakeLists.txt`:

1. Add `add_subdirectory(src/palette)` next to `add_subdirectory(src/core)`.
2. Add `palette` to the `target_link_libraries(spice PRIVATE ...)` call, so `src/main.cpp` can use
   it.

That's it - `PREFIX ""` makes the static library file `palette.a` rather than `libpalette.a`,
matching the naming used in README.md. Tests for a module only build if `tests/<module>` exists,
so a brand new module without tests yet is fine.

## Notes on the toolchain

- C++23, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`).
- GCC is the primary target (GCC 16.1+ per README.md); Clang must also build cleanly since it runs
  in CI - don't rely on GCC-only extensions or behavior.
- `-Wall -Wextra` are on for every target; fix warnings rather than suppressing them.
- Tests use [doctest](https://github.com/doctest/doctest) via the single header at
  `tests/doctest.h`, shared across all modules. Exactly one `.cpp` per module's test executable may
  define `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` (see `tests/core/test_Spice.cpp`) - it generates
  `main()`. If a module gains more than one test file, the rest just `#include "doctest.h"` and add
  `TEST_CASE`s, without redefining that macro.

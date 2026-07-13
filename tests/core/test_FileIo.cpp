#include "doctest.h"

#include "spice/core/FileIo.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <unistd.h>

using namespace spice::core;

namespace {

//! A unique temp path, removed when the test ends.
struct TempFile {
    std::string path;

    TempFile()
        : path { (std::filesystem::temp_directory_path()
                  / std::format("spice-test-{}", ::getpid())).string() }
    {}
    ~TempFile() { std::remove(path.c_str()); }
};

}

TEST_CASE("core::FileIo write and read round-trip a buffer") {
    TempFile file;
    Buffer buffer { "b", BufferCapability::editable, "line one\nline two" };
    REQUIRE(write_file(file.path, buffer));

    auto const content { read_file(file.path) };
    REQUIRE(content.has_value());
    CHECK_EQ(*content, "line one\nline two");

    // and back into a buffer, line for line
    Buffer loaded { "b2", BufferCapability::editable, *content };
    CHECK_EQ(loaded.line_count(), 2u);
    CHECK_EQ(loaded.line(0), "line one");
    CHECK_EQ(loaded.line(1), "line two");
}

TEST_CASE("core::FileIo writes a trailing newline, reading strips it") {
    TempFile file;
    Buffer buffer { "b", BufferCapability::editable, "text" };
    REQUIRE(write_file(file.path, buffer));

    auto const raw { read_file(file.path) };
    REQUIRE(raw.has_value());
    CHECK_EQ(*raw, "text"); // stripped on read...

    std::ifstream check { file.path, std::ios::binary };
    std::string const on_disk(
        (std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>()
    );
    CHECK_EQ(on_disk, "text\n"); // ...but present on disk
}

TEST_CASE("core::FileIo::read_file() returns nothing for a missing file") {
    CHECK_FALSE(read_file("/nonexistent/spice/nowhere").has_value());
}

TEST_CASE("core::FileIo::write_file() fails on an unwritable path") {
    Buffer buffer { "b", BufferCapability::editable, "x" };
    CHECK_FALSE(write_file("/nonexistent/spice/nowhere", buffer));
}

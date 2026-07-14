#include "doctest.h"

#include "spice/core/KeyBytes.hpp"

using namespace spice::core;

TEST_CASE("core::key_to_bytes() encodes text and control characters") {
    CHECK_EQ(key_to_bytes({ Key::character, {}, "a" }), "a");
    CHECK_EQ(key_to_bytes({ Key::character, {}, "\xc3\xa9" }), "\xc3\xa9");
    CHECK_EQ(
        key_to_bytes({ Key::character, { .shift = false, .alt = false, .ctrl = true }, "c" }),
        "\x03"
    );
    CHECK_EQ(
        key_to_bytes({ Key::character, { .shift = false, .alt = true, .ctrl = false }, "x" }),
        "\x1b" "x"
    );
}

TEST_CASE("core::key_to_bytes() encodes special keys as escape sequences") {
    CHECK_EQ(key_to_bytes({ Key::enter, {}, {} }), "\r");
    CHECK_EQ(key_to_bytes({ Key::backspace, {}, {} }), "\x7f");
    CHECK_EQ(key_to_bytes({ Key::up, {}, {} }), "\x1b[A");
    CHECK_EQ(key_to_bytes({ Key::del, {}, {} }), "\x1b[3~");
    CHECK_EQ(key_to_bytes({ Key::f1, {}, {} }), ""); // no encoding here
}

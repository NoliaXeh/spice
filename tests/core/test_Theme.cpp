#include "doctest.h"

#include "spice/core/Theme.hpp"

using namespace spice::core;

TEST_CASE("core::colors constants hold the expected channels") {
    CHECK_EQ(colors::black.r, 0x00);
    CHECK_EQ(colors::black.g, 0x00);
    CHECK_EQ(colors::black.b, 0x00);

    CHECK_EQ(colors::white.r, 0xFF);
    CHECK_EQ(colors::white.g, 0xFF);
    CHECK_EQ(colors::white.b, 0xFF);

    CHECK_EQ(colors::red.r, 0xFF);
    CHECK_EQ(colors::red.g, 0x00);
    CHECK_EQ(colors::red.b, 0x00);

    bool const plain { colors::red.style.bold || colors::red.style.italic };
    CHECK_FALSE(plain); // constants carry no style
}

TEST_CASE("core::Theme default theme maps every usage") {
    Theme theme;

    Color const text { theme.color(Theme::Usage::text) };
    CHECK_EQ(text.r, colors::light_gray.r);
    CHECK_EQ(text.g, colors::light_gray.g);
    CHECK_EQ(text.b, colors::light_gray.b);

    Color const error { theme.color(Theme::Usage::error) };
    CHECK_EQ(error.r, colors::red.r);
    bool const error_bold { error.style.bold };
    CHECK(error_bold);
}

TEST_CASE("core::Theme::set_color() round-trips") {
    Theme theme;
    theme.set_color(Theme::Usage::cursor, colors::magenta);

    Color const cursor { theme.color(Theme::Usage::cursor) };
    CHECK_EQ(cursor.r, colors::magenta.r);
    CHECK_EQ(cursor.g, colors::magenta.g);
    CHECK_EQ(cursor.b, colors::magenta.b);

    // other usages untouched
    Color const text { theme.color(Theme::Usage::text) };
    CHECK_EQ(text.r, colors::light_gray.r);
}

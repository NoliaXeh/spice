#include "doctest.h"

#include "spice/config/KeyName.hpp"

using namespace spice::config;

TEST_CASE("config::parse_key() maps config names to event ids") {
    CHECK_EQ(parse_key("ctrl-space"), "C-' '");
    CHECK_EQ(parse_key("ctrl-x"), "C-'x'");
    CHECK_EQ(parse_key("alt-p"), "M-'p'");
    CHECK_EQ(parse_key("meta-p"), "M-'p'");
    CHECK_EQ(parse_key("shift-return"), "S-enter");
    CHECK_EQ(parse_key("return"), "enter");
    CHECK_EQ(parse_key("ctrl-left"), "C-left");
    CHECK_EQ(parse_key("ctrl-page-up"), "C-page-up");
    CHECK_EQ(parse_key("ctrl-alt-shift-z"), "C-M-S-'z'");
    CHECK_EQ(parse_key("shift-ctrl-z"), "C-S-'z'"); // order normalized
    CHECK_EQ(parse_key("f5"), "f5");
    CHECK_EQ(parse_key("f12"), "f12");
    CHECK_EQ(parse_key("g"), "'g'");
    CHECK_EQ(parse_key("delete"), "delete");
}

TEST_CASE("config::parse_key() rejects nonsense") {
    CHECK_FALSE(parse_key("").has_value());
    CHECK_FALSE(parse_key("ctrl-").has_value());
    CHECK_FALSE(parse_key("bogus-key").has_value());
    CHECK_FALSE(parse_key("f13").has_value());
    CHECK_FALSE(parse_key("ctrl-doubleclick").has_value());
}

TEST_CASE("config::key_name() reverses parse_key") {
    for (auto const name : { "ctrl-space", "ctrl-x", "alt-p", "shift-return",
                             "ctrl-left", "ctrl-page-up", "f5", "g", "delete" }) {
        auto const id { parse_key(name) };
        REQUIRE(id.has_value());
        CHECK_EQ(key_name(*id), name);
    }
    CHECK_FALSE(key_name("press left").has_value()); // mouse ids have no name
}

#include "doctest.h"

#include "spice/core/EventParser.hpp"

using namespace spice::core;

namespace {

//! Feeds bytes expected to produce exactly one event, and returns it.
auto single(EventParser& parser, std::string_view bytes) -> Event {
    auto const events { parser.feed(bytes) };
    REQUIRE_EQ(events.size(), 1u);
    return events[0];
}

auto check_mods(Modifiers mods, bool shift, bool alt, bool ctrl) -> void {
    bool const got_shift { mods.shift };
    bool const got_alt { mods.alt };
    bool const got_ctrl { mods.ctrl };
    CHECK_EQ(got_shift, shift);
    CHECK_EQ(got_alt, alt);
    CHECK_EQ(got_ctrl, ctrl);
}

}

TEST_CASE("core::EventParser parses printable ascii") {
    EventParser parser;
    Event const event { single(parser, "a") };
    CHECK_EQ(event.type, EventType::key);
    CHECK_EQ(event.key.key, Key::character);
    CHECK_EQ(event.key.text, "a");
    check_mods(event.key.mods, false, false, false);
}

TEST_CASE("core::EventParser parses multi-byte utf-8 characters") {
    EventParser parser;
    Event const event { single(parser, "\xc3\xa9") }; // "é"
    CHECK_EQ(event.key.key, Key::character);
    CHECK_EQ(event.key.text, "\xc3\xa9");
}

TEST_CASE("core::EventParser parses a utf-8 character split across feeds") {
    EventParser parser;
    CHECK(parser.feed("\xe2\x82").empty()); // first 2 bytes of "€"
    CHECK(parser.pending());
    Event const event { single(parser, "\xac") };
    CHECK_EQ(event.key.text, "\xe2\x82\xac");
}

TEST_CASE("core::EventParser parses control characters") {
    EventParser parser;
    CHECK_EQ(single(parser, "\r").key.key, Key::enter);
    CHECK_EQ(single(parser, "\t").key.key, Key::tab);
    CHECK_EQ(single(parser, "\x7f").key.key, Key::backspace);
}

TEST_CASE("core::EventParser parses ctrl-letter") {
    EventParser parser;
    Event const event { single(parser, "\x01") }; // ctrl-a
    CHECK_EQ(event.key.key, Key::character);
    CHECK_EQ(event.key.text, "a");
    check_mods(event.key.mods, false, false, true);
}

TEST_CASE("core::EventParser parses ctrl-space (NUL)") {
    EventParser parser;
    Event const event { single(parser, std::string_view("\x00", 1)) };
    CHECK_EQ(event.key.key, Key::character);
    CHECK_EQ(event.key.text, " ");
    check_mods(event.key.mods, false, false, true);
}

TEST_CASE("core::EventParser parses arrow keys") {
    EventParser parser;
    CHECK_EQ(single(parser, "\x1b[A").key.key, Key::up);
    CHECK_EQ(single(parser, "\x1b[B").key.key, Key::down);
    CHECK_EQ(single(parser, "\x1b[C").key.key, Key::right);
    CHECK_EQ(single(parser, "\x1b[D").key.key, Key::left);
    CHECK_EQ(single(parser, "\x1bOA").key.key, Key::up); // application mode
}

TEST_CASE("core::EventParser parses modified arrows") {
    EventParser parser;
    Event const event { single(parser, "\x1b[1;5C") }; // ctrl-right
    CHECK_EQ(event.key.key, Key::right);
    check_mods(event.key.mods, false, false, true);

    Event const shifted { single(parser, "\x1b[1;2A") }; // shift-up
    CHECK_EQ(shifted.key.key, Key::up);
    check_mods(shifted.key.mods, true, false, false);
}

TEST_CASE("core::EventParser parses tilde-coded keys") {
    EventParser parser;
    CHECK_EQ(single(parser, "\x1b[2~").key.key, Key::insert);
    CHECK_EQ(single(parser, "\x1b[3~").key.key, Key::del);
    CHECK_EQ(single(parser, "\x1b[5~").key.key, Key::page_up);
    CHECK_EQ(single(parser, "\x1b[6~").key.key, Key::page_down);
    CHECK_EQ(single(parser, "\x1b[H").key.key, Key::home);
    CHECK_EQ(single(parser, "\x1b[F").key.key, Key::end);
}

TEST_CASE("core::EventParser parses function keys") {
    EventParser parser;
    CHECK_EQ(single(parser, "\x1bOP").key.key, Key::f1);
    CHECK_EQ(single(parser, "\x1bOS").key.key, Key::f4);
    CHECK_EQ(single(parser, "\x1b[15~").key.key, Key::f5);
    CHECK_EQ(single(parser, "\x1b[24~").key.key, Key::f12);
}

TEST_CASE("core::EventParser parses backtab as shift-tab") {
    EventParser parser;
    Event const event { single(parser, "\x1b[Z") };
    CHECK_EQ(event.key.key, Key::tab);
    check_mods(event.key.mods, true, false, false);
}

TEST_CASE("core::EventParser parses alt-character") {
    EventParser parser;
    Event const event { single(parser, "\x1b" "a") };
    CHECK_EQ(event.key.key, Key::character);
    CHECK_EQ(event.key.text, "a");
    check_mods(event.key.mods, false, true, false);
}

TEST_CASE("core::EventParser resolves a lone ESC on flush") {
    EventParser parser;
    CHECK(parser.feed("\x1b").empty());
    CHECK(parser.pending());
    auto const events { parser.flush() };
    REQUIRE_EQ(events.size(), 1u);
    CHECK_EQ(events[0].key.key, Key::escape);
    CHECK_FALSE(parser.pending());
}

TEST_CASE("core::EventParser parses a sequence split across feeds") {
    EventParser parser;
    CHECK(parser.feed("\x1b[").empty());
    CHECK(parser.pending());
    Event const event { single(parser, "A") };
    CHECK_EQ(event.key.key, Key::up);
    CHECK_FALSE(parser.pending());
}

TEST_CASE("core::EventParser parses mouse press and release") {
    EventParser parser;

    Event const press { single(parser, "\x1b[<0;10;5M") };
    CHECK_EQ(press.type, EventType::mouse);
    CHECK_EQ(press.mouse.action, MouseAction::press);
    CHECK_EQ(press.mouse.button, MouseButton::left);
    CHECK_EQ(press.mouse.position.column, 9u); // 1-based on the wire
    CHECK_EQ(press.mouse.position.line, 4u);

    Event const release { single(parser, "\x1b[<0;10;5m") };
    CHECK_EQ(release.mouse.action, MouseAction::release);
}

TEST_CASE("core::EventParser parses the other mouse buttons") {
    EventParser parser;
    CHECK_EQ(single(parser, "\x1b[<1;1;1M").mouse.button, MouseButton::middle);
    CHECK_EQ(single(parser, "\x1b[<2;1;1M").mouse.button, MouseButton::right);
}

TEST_CASE("core::EventParser parses mouse drag as move") {
    EventParser parser;
    Event const event { single(parser, "\x1b[<32;3;4M") };
    CHECK_EQ(event.mouse.action, MouseAction::move);
    CHECK_EQ(event.mouse.button, MouseButton::left);
    CHECK_EQ(event.mouse.position.column, 2u);
    CHECK_EQ(event.mouse.position.line, 3u);
}

TEST_CASE("core::EventParser parses the mouse wheel") {
    EventParser parser;
    CHECK_EQ(single(parser, "\x1b[<64;1;1M").mouse.button, MouseButton::wheel_up);
    CHECK_EQ(single(parser, "\x1b[<65;1;1M").mouse.button, MouseButton::wheel_down);
}

TEST_CASE("core::EventParser parses mouse modifiers") {
    EventParser parser;
    Event const event { single(parser, "\x1b[<16;2;2M") }; // ctrl-click
    CHECK_EQ(event.mouse.button, MouseButton::left);
    check_mods(event.mouse.mods, false, false, true);
}

TEST_CASE("core::EventParser drops unknown sequences without desyncing") {
    EventParser parser;
    CHECK(parser.feed("\x1b[?1049h").empty()); // not an input sequence
    Event const event { single(parser, "x") };  // parsing continues cleanly
    CHECK_EQ(event.key.text, "x");
}

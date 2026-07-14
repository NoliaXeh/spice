#include "doctest.h"

#include "spice/core/EventText.hpp"

using namespace spice::core;

namespace {

auto key_event(Key key, std::string text = {}, Modifiers mods = {}) -> Event {
    return { EventType::key, { key, mods, std::move(text) }, {} };
}

}

TEST_CASE("core::event_id() names keys with their modifiers") {
    CHECK_EQ(event_id(key_event(Key::character, "w")), "'w'");
    CHECK_EQ(
        event_id(key_event(Key::character, "w", { .shift = false, .alt = false, .ctrl = true })),
        "C-'w'"
    );
    CHECK_EQ(
        event_id(key_event(Key::up, {}, { .shift = true, .alt = false, .ctrl = false })),
        "S-up"
    );
    CHECK_EQ(event_id(key_event(Key::f5)), "f5");
}

TEST_CASE("core::event_id() names mouse events without their position") {
    Event const press {
        EventType::mouse, {},
        { MouseAction::press, MouseButton::left, {}, { 4, 20, 0 } },
    };
    CHECK_EQ(event_id(press), "press left");
}

TEST_CASE("core::event_id() names resizes") {
    CHECK_EQ(event_id({ EventType::resize, {}, {} }), "resize");
}

TEST_CASE("core::describe() adds the kind and the mouse position") {
    CHECK_EQ(describe(key_event(Key::enter)), "key enter");
    Event const press {
        EventType::mouse, {},
        { MouseAction::press, MouseButton::left, {}, { 4, 20, 0 } },
    };
    CHECK_EQ(describe(press), "mouse press left 4:20");
    CHECK_EQ(describe({ EventType::resize, {}, {} }), "resize");
}

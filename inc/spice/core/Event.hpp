#ifndef SPICE_CORE_EVENT_H
#define SPICE_CORE_EVENT_H

#include "spice/core/Position.hpp"
#include <cstdint>
#include <string>

namespace spice::core {

//! Modifier keys held during an input event.
struct Modifiers {
    bool shift: 1;
    bool alt: 1;
    bool ctrl: 1;
};

//! Every key the core can name. `character` is any printable character
//! (including multi-byte utf-8), carried in KeyEvent::text.
enum class Key : uint8_t {
    character,
    enter,
    tab,
    backspace,
    escape,
    up,
    down,
    left,
    right,
    home,
    end,
    page_up,
    page_down,
    insert,
    del,
    f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
};

struct KeyEvent {
    Key key;
    Modifiers mods;
    std::string text; //!< the utf-8 character when key == Key::character, else empty
};

enum class MouseButton : uint8_t {
    none, //!< motion without a held button
    left,
    middle,
    right,
    wheel_up,
    wheel_down,
};

enum class MouseAction : uint8_t {
    press,
    release,
    move,
};

struct MouseEvent {
    MouseAction action;
    MouseButton button;
    Modifiers mods;
    Position position; //!< cell under the pointer, 0-based; layer is always 0
};

enum class EventType : uint8_t {
    key,
    mouse,
};

//! A single user input. `type` says which member carries the event.
struct Event {
    EventType type;
    KeyEvent key;     //!< meaningful when type == EventType::key
    MouseEvent mouse; //!< meaningful when type == EventType::mouse
};

}

#endif // SPICE_CORE_EVENT_H

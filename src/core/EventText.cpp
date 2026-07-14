#include "spice/core/EventText.hpp"

#include <format>
#include <string_view>

namespace {

using namespace spice::core;

auto mods_text(Modifiers mods) -> std::string {
    std::string out;
    if (mods.ctrl) out += "C-";
    if (mods.alt) out += "M-";
    if (mods.shift) out += "S-";
    return out;
}

auto key_name(Key key) -> std::string_view {
    switch (key) {
        case Key::character: return "char";
        case Key::enter: return "enter";
        case Key::tab: return "tab";
        case Key::backspace: return "backspace";
        case Key::escape: return "escape";
        case Key::up: return "up";
        case Key::down: return "down";
        case Key::left: return "left";
        case Key::right: return "right";
        case Key::home: return "home";
        case Key::end: return "end";
        case Key::page_up: return "page-up";
        case Key::page_down: return "page-down";
        case Key::insert: return "insert";
        case Key::del: return "delete";
        case Key::f1: return "f1";
        case Key::f2: return "f2";
        case Key::f3: return "f3";
        case Key::f4: return "f4";
        case Key::f5: return "f5";
        case Key::f6: return "f6";
        case Key::f7: return "f7";
        case Key::f8: return "f8";
        case Key::f9: return "f9";
        case Key::f10: return "f10";
        case Key::f11: return "f11";
        case Key::f12: return "f12";
    }
    return "?";
}

auto action_name(MouseAction action) -> std::string_view {
    switch (action) {
        case MouseAction::press: return "press";
        case MouseAction::release: return "release";
        case MouseAction::move: return "move";
    }
    return "?";
}

auto button_name(MouseButton button) -> std::string_view {
    switch (button) {
        case MouseButton::none: return "none";
        case MouseButton::left: return "left";
        case MouseButton::middle: return "middle";
        case MouseButton::right: return "right";
        case MouseButton::wheel_up: return "wheel-up";
        case MouseButton::wheel_down: return "wheel-down";
    }
    return "?";
}

}

namespace spice::core {

auto event_id(Event const& event) -> std::string {
    switch (event.type) {
    case EventType::resize:
        return "resize";

    case EventType::key: {
        auto const& key { event.key };
        if (key.key == Key::character) {
            return std::format("{}'{}'", mods_text(key.mods), key.text);
        }
        return std::format("{}{}", mods_text(key.mods), key_name(key.key));
    }

    default: {
        auto const& mouse { event.mouse };
        return std::format(
            "{}{} {}",
            mods_text(mouse.mods), action_name(mouse.action), button_name(mouse.button)
        );
    }
    }
}

auto describe(Event const& event) -> std::string {
    switch (event.type) {
    case EventType::key:
        return "key " + event_id(event);
    case EventType::resize:
        return event_id(event);
    default:
        return std::format(
            "mouse {} {}:{}",
            event_id(event), event.mouse.position.line, event.mouse.position.column
        );
    }
}

}

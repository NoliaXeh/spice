#include "spice/core/EventParser.hpp"
#include "spice/core/Utf8.hpp"

#include <cstddef>
#include <optional>
#include <utility>

namespace {

using namespace spice::core;

auto key_event(Key key, Modifiers mods = {}, std::string text = {}) -> Event {
    return Event {
        .type = EventType::key,
        .key = KeyEvent { .key = key, .mods = mods, .text = std::move(text) },
        .mouse = {},
    };
}

//! xterm encodes modifiers as 1 + bitmask(shift=1, alt=2, ctrl=4);
//! 0 means the parameter was absent.
auto decode_modifiers(uint32_t value) -> Modifiers {
    if (value == 0) {
        return {};
    }
    uint32_t const bits { value - 1 };
    return Modifiers {
        .shift = (bits & 1) != 0,
        .alt = (bits & 2) != 0,
        .ctrl = (bits & 4) != 0,
    };
}

//! Parses CSI parameters like "1;5" into integers; absent fields become 0.
auto parse_params(std::string_view text) -> std::vector<uint32_t> {
    std::vector<uint32_t> params { 0 };
    for (char const c : text) {
        if (c == ';') {
            params.push_back(0);
        } else if (c >= '0' && c <= '9') {
            params.back() = params.back() * 10 + static_cast<uint32_t>(c - '0');
        }
    }
    return params;
}

//! Key named by a "CSI code ~" sequence.
auto tilde_key(uint32_t code) -> std::optional<Key> {
    switch (code) {
        case 1: return Key::home;
        case 2: return Key::insert;
        case 3: return Key::del;
        case 4: return Key::end;
        case 5: return Key::page_up;
        case 6: return Key::page_down;
        case 11: return Key::f1;
        case 12: return Key::f2;
        case 13: return Key::f3;
        case 14: return Key::f4;
        case 15: return Key::f5;
        case 17: return Key::f6;
        case 18: return Key::f7;
        case 19: return Key::f8;
        case 20: return Key::f9;
        case 21: return Key::f10;
        case 23: return Key::f11;
        case 24: return Key::f12;
        default: return std::nullopt;
    }
}

//! Event for a C0 control byte (never called with ESC). Terminals send
//! ctrl-letter as the letter's position in the alphabet (ctrl-a = 0x01),
//! and ctrl-space as NUL.
auto control_event(char byte, bool alt) -> std::optional<Event> {
    Modifiers mods { .shift = false, .alt = alt, .ctrl = false };
    switch (byte) {
        case '\r':
        case '\n': return key_event(Key::enter, mods);
        case '\t': return key_event(Key::tab, mods);
        case '\x7f':
        case '\x08': return key_event(Key::backspace, mods);
        case '\x00':
            mods.ctrl = true;
            return key_event(Key::character, mods, " ");
        default: break;
    }
    if (byte >= 0x01 && byte <= 0x1a) {
        mods.ctrl = true;
        return key_event(Key::character, mods, std::string(1, static_cast<char>('a' + byte - 1)));
    }
    if (byte >= 0x1c && byte <= 0x1f) {
        constexpr char names[] { '\\', ']', '^', '_' };
        mods.ctrl = true;
        return key_event(Key::character, mods, std::string(1, names[byte - 0x1c]));
    }
    return std::nullopt;
}

}

namespace spice::core {

auto EventParser::feed(std::string_view bytes) -> std::vector<Event> {
    std::vector<Event> events;
    for (char const byte : bytes) {
        consume(byte, events);
    }
    return events;
}

auto EventParser::pending() const -> bool {
    return _state != State::ground;
}

auto EventParser::flush() -> std::vector<Event> {
    std::vector<Event> events;
    if (_state == State::escape) {
        events.push_back(key_event(Key::escape));
    }
    _state = State::ground;
    _pending.clear();
    _alt = false;
    return events;
}

auto EventParser::consume(char byte, std::vector<Event>& events) -> void {
    switch (_state) {
    case State::ground:
        handle_ground(byte, false, events);
        break;

    case State::escape:
        if (byte == '[') {
            _state = State::csi;
            _pending.clear();
        } else if (byte == 'O') {
            _state = State::ss3;
        } else if (byte == '\x1b') {
            events.push_back(key_event(Key::escape)); // ESC ESC; stay on the second ESC
        } else {
            _state = State::ground;
            handle_ground(byte, true, events);
        }
        break;

    case State::csi:
        if (byte >= 0x40 && byte <= 0x7e) { // final byte ends the sequence
            parse_csi(byte, events);
            _state = State::ground;
            _pending.clear();
        } else {
            _pending.push_back(byte); // parameter / intermediate bytes
        }
        break;

    case State::ss3:
        parse_ss3(byte, events);
        _state = State::ground;
        break;

    case State::utf8:
        if ((static_cast<unsigned char>(byte) & 0xC0) == 0x80) {
            _pending.push_back(byte);
            if (_pending.size() == utf8_length(_pending[0])) {
                Modifiers const mods { .shift = false, .alt = _alt, .ctrl = false };
                events.push_back(key_event(Key::character, mods, _pending));
                _state = State::ground;
                _pending.clear();
                _alt = false;
            }
        } else {
            // malformed: drop the partial character and reprocess this byte
            _state = State::ground;
            _pending.clear();
            _alt = false;
            consume(byte, events);
        }
        break;
    }
}

auto EventParser::handle_ground(char byte, bool alt, std::vector<Event>& events) -> void {
    auto const value { static_cast<unsigned char>(byte) };

    if (byte == '\x1b') {
        _state = State::escape;
        return;
    }
    if (value >= 0x80) {
        if (utf8_length(byte) == 1) {
            return; // stray continuation or invalid lead byte: drop
        }
        _state = State::utf8;
        _pending.assign(1, byte);
        _alt = alt;
        return;
    }
    if (value < 0x20 || value == 0x7f) {
        if (auto event { control_event(byte, alt) }) {
            events.push_back(*event);
        }
        return;
    }
    Modifiers const mods { .shift = false, .alt = alt, .ctrl = false };
    events.push_back(key_event(Key::character, mods, std::string(1, byte)));
}

auto EventParser::parse_csi(char final, std::vector<Event>& events) -> void {
    if (!_pending.empty() && _pending[0] == '<' && (final == 'M' || final == 'm')) {
        parse_mouse(final, events);
        return;
    }

    auto const params { parse_params(_pending) };
    auto const mods { decode_modifiers(params.size() > 1 ? params[1] : 0) };

    switch (final) {
        case 'A': events.push_back(key_event(Key::up, mods)); break;
        case 'B': events.push_back(key_event(Key::down, mods)); break;
        case 'C': events.push_back(key_event(Key::right, mods)); break;
        case 'D': events.push_back(key_event(Key::left, mods)); break;
        case 'H': events.push_back(key_event(Key::home, mods)); break;
        case 'F': events.push_back(key_event(Key::end, mods)); break;
        case 'P': events.push_back(key_event(Key::f1, mods)); break;
        case 'Q': events.push_back(key_event(Key::f2, mods)); break;
        case 'R': events.push_back(key_event(Key::f3, mods)); break;
        case 'S': events.push_back(key_event(Key::f4, mods)); break;
        case 'Z': { // backtab
            Modifiers shifted { mods };
            shifted.shift = true;
            events.push_back(key_event(Key::tab, shifted));
            break;
        }
        case '~':
            if (auto const key { tilde_key(params[0]) }) {
                events.push_back(key_event(*key, mods));
            }
            break;
        default: break; // unknown sequence: dropped
    }
}

auto EventParser::parse_ss3(char byte, std::vector<Event>& events) -> void {
    switch (byte) {
        case 'A': events.push_back(key_event(Key::up)); break;
        case 'B': events.push_back(key_event(Key::down)); break;
        case 'C': events.push_back(key_event(Key::right)); break;
        case 'D': events.push_back(key_event(Key::left)); break;
        case 'H': events.push_back(key_event(Key::home)); break;
        case 'F': events.push_back(key_event(Key::end)); break;
        case 'P': events.push_back(key_event(Key::f1)); break;
        case 'Q': events.push_back(key_event(Key::f2)); break;
        case 'R': events.push_back(key_event(Key::f3)); break;
        case 'S': events.push_back(key_event(Key::f4)); break;
        default: break; // unknown sequence: dropped
    }
}

auto EventParser::parse_mouse(char final, std::vector<Event>& events) -> void {
    // SGR report: CSI < button ; column ; row (M = press/move, m = release)
    auto const params { parse_params(std::string_view(_pending).substr(1)) };
    if (params.size() < 3) {
        return;
    }
    uint32_t const button_code { params[0] };
    uint32_t const column { params[1] };
    uint32_t const row { params[2] };

    MouseEvent mouse {};
    mouse.mods = Modifiers {
        .shift = (button_code & 4) != 0,
        .alt = (button_code & 8) != 0,
        .ctrl = (button_code & 16) != 0,
    };
    mouse.position = Position { // terminals report 1-based cells
        .line = row > 0 ? row - 1 : 0,
        .column = column > 0 ? column - 1 : 0,
        .layer = 0,
    };

    if ((button_code & 64) != 0) {
        mouse.button = (button_code & 3) == 0 ? MouseButton::wheel_up : MouseButton::wheel_down;
        mouse.action = MouseAction::press;
    } else {
        switch (button_code & 3) {
            case 0: mouse.button = MouseButton::left; break;
            case 1: mouse.button = MouseButton::middle; break;
            case 2: mouse.button = MouseButton::right; break;
            case 3: mouse.button = MouseButton::none; break;
        }
        if ((button_code & 32) != 0) {
            mouse.action = MouseAction::move;
        } else {
            mouse.action = final == 'M' ? MouseAction::press : MouseAction::release;
        }
    }

    events.push_back(Event { .type = EventType::mouse, .key = {}, .mouse = mouse });
}

}

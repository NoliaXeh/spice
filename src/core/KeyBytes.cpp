#include "spice/core/KeyBytes.hpp"

namespace spice::core {

auto key_to_bytes(KeyEvent const& key) -> std::string {
    if (key.key == Key::character) {
        if (key.mods.ctrl && key.text.size() == 1
            && key.text[0] >= 'a' && key.text[0] <= 'z') {
            return std::string(1, static_cast<char>(key.text[0] - 'a' + 1));
        }
        if (key.mods.alt) {
            return "\x1b" + key.text;
        }
        return key.text;
    }
    switch (key.key) {
        case Key::enter: return "\r";
        case Key::tab: return "\t";
        case Key::backspace: return "\x7f";
        case Key::escape: return "\x1b";
        case Key::up: return "\x1b[A";
        case Key::down: return "\x1b[B";
        case Key::right: return "\x1b[C";
        case Key::left: return "\x1b[D";
        case Key::home: return "\x1b[H";
        case Key::end: return "\x1b[F";
        case Key::del: return "\x1b[3~";
        case Key::insert: return "\x1b[2~";
        case Key::page_up: return "\x1b[5~";
        case Key::page_down: return "\x1b[6~";
        default: return {};
    }
}

}

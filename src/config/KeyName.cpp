#include "spice/config/KeyName.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace {

//! Named keys, config-file name on the left, event-id name on the right.
constexpr std::array<std::pair<std::string_view, std::string_view>, 24> named_keys { {
    { "return", "enter" },
    { "enter", "enter" },
    { "tab", "tab" },
    { "backspace", "backspace" },
    { "escape", "escape" },
    { "esc", "escape" },
    { "up", "up" },
    { "down", "down" },
    { "left", "left" },
    { "right", "right" },
    { "home", "home" },
    { "end", "end" },
    { "page-up", "page-up" },
    { "page-down", "page-down" },
    { "insert", "insert" },
    { "delete", "delete" },
    { "del", "delete" },
    { "f1", "f1" }, { "f2", "f2" }, { "f3", "f3" }, { "f4", "f4" },
    { "f5", "f5" }, { "f6", "f6" }, { "f7", "f7" },
} };

//! f8-f12 don't fit the array above nicely; handle every fN generically.
auto function_key(std::string_view base) -> bool {
    if (base.size() < 2 || base.size() > 3 || base[0] != 'f') {
        return false;
    }
    for (char const c : base.substr(1)) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    int const n { std::stoi(std::string(base.substr(1))) };
    return n >= 1 && n <= 12;
}

auto base_to_id(std::string_view base) -> std::optional<std::string> {
    if (base == "space") {
        return "' '";
    }
    if (base.size() == 1 && base[0] > 0x20 && base[0] < 0x7f) {
        return "'" + std::string(base) + "'";
    }
    for (auto const& [name, id] : named_keys) {
        if (base == name) {
            return std::string(id);
        }
    }
    if (function_key(base)) {
        return std::string(base);
    }
    return std::nullopt;
}

auto id_to_base(std::string_view id) -> std::optional<std::string> {
    if (id.size() == 3 && id.front() == '\'' && id.back() == '\'') {
        return id[1] == ' ' ? "space" : std::string(1, id[1]);
    }
    for (auto const& [name, mapped] : named_keys) {
        if (id == mapped) {
            return std::string(name); // first entry wins: the canonical name
        }
    }
    if (function_key(id)) {
        return std::string(id);
    }
    return std::nullopt;
}

}

namespace spice::config {

auto parse_key(std::string const& name) -> std::optional<std::string> {
    bool ctrl { false };
    bool alt { false };
    bool shift { false };

    std::string_view rest { name };
    while (true) { // strip modifier prefixes; what remains is the base key
        size_t const dash { rest.find('-') };
        if (dash == std::string_view::npos) {
            break;
        }
        std::string_view const head { rest.substr(0, dash) };
        if (head == "ctrl") {
            ctrl = true;
        } else if (head == "alt" || head == "meta") {
            alt = true;
        } else if (head == "shift") {
            shift = true;
        } else {
            break; // not a modifier: part of the base name (page-up...)
        }
        rest = rest.substr(dash + 1);
    }

    auto const base { base_to_id(rest) };
    if (!base) {
        return std::nullopt;
    }
    std::string id;
    if (ctrl) id += "C-";
    if (alt) id += "M-";
    if (shift) id += "S-";
    return id + *base;
}

auto key_name(std::string const& event_id) -> std::optional<std::string> {
    std::string_view rest { event_id };
    std::string name;
    while (rest.size() >= 2 && rest[1] == '-') {
        if (rest[0] == 'C') {
            name += "ctrl-";
        } else if (rest[0] == 'M') {
            name += "alt-";
        } else if (rest[0] == 'S') {
            name += "shift-";
        } else {
            return std::nullopt;
        }
        rest = rest.substr(2);
    }
    auto const base { id_to_base(rest) };
    if (!base) {
        return std::nullopt;
    }
    return name + *base;
}

}

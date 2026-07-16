#include "spice/core/Terminal.hpp"
#include "spice/core/Utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>

namespace {

using spice::core::Color;

//! The xterm 16-color palette (normal 0-7, bright 8-15).
constexpr std::array<std::array<uint8_t, 3>, 16> base_palette { {
    { 0x00, 0x00, 0x00 }, { 0xCD, 0x00, 0x00 }, { 0x00, 0xCD, 0x00 }, { 0xCD, 0xCD, 0x00 },
    { 0x00, 0x00, 0xEE }, { 0xCD, 0x00, 0xCD }, { 0x00, 0xCD, 0xCD }, { 0xE5, 0xE5, 0xE5 },
    { 0x7F, 0x7F, 0x7F }, { 0xFF, 0x00, 0x00 }, { 0x00, 0xFF, 0x00 }, { 0xFF, 0xFF, 0x00 },
    { 0x5C, 0x5C, 0xFF }, { 0xFF, 0x00, 0xFF }, { 0x00, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF },
} };

//! The xterm 256-color palette: 16 base + 6x6x6 cube + 24 grays.
auto indexed_color(int index, spice::core::StyleFlags style) -> Color {
    auto const clamped { std::clamp(index, 0, 255) };
    if (clamped < 16) {
        return {
            base_palette[clamped][0], base_palette[clamped][1], base_palette[clamped][2],
            style,
        };
    }
    if (clamped < 232) {
        int const v { clamped - 16 };
        auto const level = [](int n) -> uint8_t {
            return n == 0 ? 0 : static_cast<uint8_t>(55 + 40 * n);
        };
        return { level(v / 36), level((v / 6) % 6), level(v % 6), style };
    }
    auto const gray { static_cast<uint8_t>(8 + 10 * (clamped - 232)) };
    return { gray, gray, gray, style };
}

auto trim_trailing_blanks(std::string text) -> std::string {
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

//! The CSI parameter at `index`, defaulting to 1 (movement counts).
auto csi_count(std::vector<int> const& params, size_t index = 0) -> uint32_t {
    return index < params.size() && params[index] > 0
        ? static_cast<uint32_t>(params[index]) : 1U;
}

}

namespace spice::core {

Terminal::Terminal(uint32_t width, uint32_t height, Color foreground, Color background)
    : _width { std::max(width, 1u) }
    , _height { std::max(height, 1u) }
    , _cells(static_cast<size_t>(_width) * _height,
             TerminalCell { " ", foreground, background })
    , _default_foreground { foreground }
    , _default_background { background }
    , _foreground { foreground }
    , _background { background }
{}

auto Terminal::width() const -> uint32_t {
    return _width;
}

auto Terminal::height() const -> uint32_t {
    return _height;
}

auto Terminal::cursor() const -> Position {
    return _cursor;
}

auto Terminal::at(uint32_t line, uint32_t column) -> TerminalCell& {
    return _cells[static_cast<size_t>(line) * _width + column];
}

auto Terminal::cell(uint32_t line, uint32_t column) const -> TerminalCell const& {
    static TerminalCell const blank {};
    if (line >= _height || column >= _width) {
        return blank;
    }
    return _cells[static_cast<size_t>(line) * _width + column];
}

// -- feeding -----------------------------------------------------------

auto Terminal::feed(std::string_view bytes) -> void {
    for (char const byte : bytes) {
        auto const value { static_cast<unsigned char>(byte) };

        if (_state == State::ground) {
            if (!_pending_utf8.empty()) { // finish a multi-byte character
                _pending_utf8 += byte;
                if (_pending_utf8.size() >= utf8_length(_pending_utf8[0])) {
                    put(std::exchange(_pending_utf8, {}));
                }
                continue;
            }
            if (byte == '\x1b') {
                _state = State::escape;
            } else if (value < 0x20 || value == 0x7f) {
                handle_control(byte);
            } else if (value >= 0x80) {
                if (utf8_length(byte) > 1) {
                    _pending_utf8 += byte;
                } // stray continuation bytes are dropped
            } else {
                put(std::string(1, byte));
            }
        } else if (_state == State::escape) {
            handle_escape(byte);
        } else if (_state == State::csi) {
            if (value >= 0x40 && value <= 0x7e) {
                handle_csi(byte);
                _state = State::ground;
                _sequence.clear();
            } else {
                _sequence += byte;
            }
        } else { // osc/dcs payloads: swallow until BEL or ESC \ (the ESC
                 // path sees the '\' and falls back to ground)
            if (byte == '\x07') {
                _state = State::ground;
            } else if (byte == '\x1b') {
                _state = State::escape;
            }
        }
    }
}

auto Terminal::put(std::string glyph) -> void {
    if (_cursor.column >= _width) { // deferred wrap
        _cursor.column = 0;
        newline();
    }
    at(_cursor.line, _cursor.column) = { std::move(glyph), _foreground, _background };
    ++_cursor.column;
}

auto Terminal::newline() -> void {
    if (_cursor.line + 1 >= _height) {
        scroll_up();
    } else {
        ++_cursor.line;
    }
}

auto Terminal::scroll_up() -> void {
    _scrollback.push_back(trim_trailing_blanks(line_text(0)));
    std::move(
        _cells.begin() + _width, _cells.end(), _cells.begin()
    );
    erase_cells(_height - 1, 0, _width);
}

auto Terminal::erase_cells(uint32_t line, uint32_t from, uint32_t to) -> void {
    for (uint32_t column { from }; column < to && column < _width; ++column) {
        at(line, column) = { " ", _foreground, _background };
    }
}

auto Terminal::handle_control(char byte) -> void {
    switch (byte) {
        case '\r': _cursor.column = 0; break;
        case '\n':
        case '\v':
        case '\f': newline(); break;
        case '\b':
            if (_cursor.column > 0) {
                --_cursor.column;
            }
            break;
        case '\t':
            _cursor.column = std::min((_cursor.column / 8 + 1) * 8, _width - 1);
            break;
        default: break; // BEL and friends: nothing to show
    }
}

auto Terminal::handle_escape(char byte) -> void {
    switch (byte) {
    case '[':
        _state = State::csi;
        _sequence.clear();
        return;
    case ']':
        _state = State::osc;
        return;
    case 'P': // DCS: a query/data string we don't interpret
        _state = State::dcs;
        return;
    case '(': case ')': case '*': case '+': // charset selection: G0..G3
        _state = State::escape;             // swallow the set byte next
        _sequence = "charset";
        return;
    case '7': _saved_cursor = _cursor; break;
    case '8': _cursor = _saved_cursor; break;
    case 'D': newline(); break;             // index
    case 'M':                               // reverse index
        if (_cursor.line == 0) {
            std::move_backward(
                _cells.begin(), _cells.end() - _width, _cells.end()
            );
            erase_cells(0, 0, _width);
        } else {
            --_cursor.line;
        }
        break;
    case 'c': // full reset
        _foreground = _default_foreground;
        _background = _default_background;
        _cursor = { 0, 0, 0 };
        for (uint32_t line { 0 }; line < _height; ++line) {
            erase_cells(line, 0, _width);
        }
        break;
    default: break; // ESC = , ESC > and the rest: ignored
    }
    // a charset escape swallows exactly one extra byte
    if (_sequence == "charset") {
        _sequence.clear();
    }
    _state = State::ground;
}

auto Terminal::parameters() const -> std::vector<int> {
    std::vector<int> params { 0 };
    for (char const c : _sequence) {
        if (c == ';') {
            params.push_back(0);
        } else if (c >= '0' && c <= '9') {
            params.back() = params.back() * 10 + (c - '0');
        }
    }
    return params;
}

auto Terminal::handle_csi(char final) -> void {
    if (!_sequence.empty() && _sequence[0] == '>') {
        if (final == 'c') { // secondary DA: claim a VT220-ish terminal
            _responses += "\x1b[>1;10;0c";
        }
        return;
    }
    if (!_sequence.empty() && _sequence[0] == '?') {
        return; // private modes (cursor visibility, alt screen...): ignored
    }
    if (final == 'c') { // primary DA: VT220 with ANSI color
        _responses += "\x1b[?62;22c";
        return;
    }
    if (final == 'n') { // device status reports
        auto const request { parameters()[0] };
        if (request == 5) {
            _responses += "\x1b[0n"; // "I'm fine"
        } else if (request == 6) {
            _responses += std::format(
                "\x1b[{};{}R", _cursor.line + 1, _cursor.column + 1
            );
        }
        return;
    }
    auto const params { parameters() };
    switch (final) {
    case 'A': case 'B': case 'C': case 'D':
    case 'E': case 'F': case 'G': case 'd':
    case 'H': case 'f':
        csi_move_cursor(final, params);
        break;

    case 'J': case 'K': case 'X':
        csi_erase(final, params);
        break;

    case 'L': case 'M': case 'P': case '@':
        csi_edit_cells(final, params);
        break;

    case 's': _saved_cursor = _cursor; break;
    case 'u': _cursor = _saved_cursor; break;
    case 'm': apply_sgr(); break;
    default: break; // modes, reports, margins: ignored
    }
}

auto Terminal::csi_move_cursor(char final, std::vector<int> const& params) -> void {
    auto const count = [&](size_t index = 0) { return csi_count(params, index); };
    uint32_t const last_line { _height - 1 };
    uint32_t const last_column { _width - 1 };

    switch (final) {
    case 'A': _cursor.line = _cursor.line > count() ? _cursor.line - count() : 0; break;
    case 'B': _cursor.line = std::min(_cursor.line + count(), last_line); break;
    case 'C': _cursor.column = std::min(_cursor.column + count(), last_column); break;
    case 'D': _cursor.column = _cursor.column > count() ? _cursor.column - count() : 0; break;
    case 'E': _cursor.line = std::min(_cursor.line + count(), last_line);
              _cursor.column = 0; break;
    case 'F': _cursor.line = _cursor.line > count() ? _cursor.line - count() : 0;
              _cursor.column = 0; break;
    case 'G': _cursor.column = std::min(count() - 1, last_column); break;
    case 'd': _cursor.line = std::min(count() - 1, last_line); break;
    case 'H':
    case 'f':
        _cursor.line = std::min(count(0) - 1, last_line);
        _cursor.column = std::min(params.size() > 1 ? count(1) - 1 : 0U, last_column);
        break;
    default: break;
    }
}

auto Terminal::csi_erase(char final, std::vector<int> const& params) -> void {
    int const mode { params[0] };
    switch (final) {
    case 'J': // erase in display
        if (mode == 0) {
            erase_cells(_cursor.line, _cursor.column, _width);
            for (uint32_t line { _cursor.line + 1 }; line < _height; ++line) {
                erase_cells(line, 0, _width);
            }
        } else if (mode == 1) {
            for (uint32_t line { 0 }; line < _cursor.line; ++line) {
                erase_cells(line, 0, _width);
            }
            erase_cells(_cursor.line, 0, _cursor.column + 1);
        } else {
            for (uint32_t line { 0 }; line < _height; ++line) {
                erase_cells(line, 0, _width);
            }
        }
        break;

    case 'K': // erase in line
        if (mode == 0) {
            erase_cells(_cursor.line, _cursor.column, _width);
        } else if (mode == 1) {
            erase_cells(_cursor.line, 0, _cursor.column + 1);
        } else {
            erase_cells(_cursor.line, 0, _width);
        }
        break;

    case 'X': // erase characters in place
        erase_cells(_cursor.line, _cursor.column, _cursor.column + csi_count(params));
        break;
    default: break;
    }
}

auto Terminal::csi_edit_cells(char final, std::vector<int> const& params) -> void {
    switch (final) {
    case 'L': { // insert blank lines at the cursor
        uint32_t const n { std::min(csi_count(params), _height - _cursor.line) };
        auto const from { _cells.begin() + static_cast<size_t>(_cursor.line) * _width };
        std::move_backward(from, _cells.end() - static_cast<size_t>(n) * _width, _cells.end());
        for (uint32_t line { _cursor.line }; line < _cursor.line + n; ++line) {
            erase_cells(line, 0, _width);
        }
        break;
    }
    case 'M': { // delete lines at the cursor
        uint32_t const n { std::min(csi_count(params), _height - _cursor.line) };
        auto const from { _cells.begin() + static_cast<size_t>(_cursor.line) * _width };
        std::move(from + static_cast<size_t>(n) * _width, _cells.end(), from);
        for (uint32_t line { _height - n }; line < _height; ++line) {
            erase_cells(line, 0, _width);
        }
        break;
    }
    case 'P': { // delete characters (shift the rest of the line left)
        uint32_t const n { std::min(csi_count(params), _width - _cursor.column) };
        for (uint32_t column { _cursor.column }; column < _width; ++column) {
            at(_cursor.line, column) = column + n < _width
                ? at(_cursor.line, column + n)
                : TerminalCell { " ", _foreground, _background };
        }
        break;
    }
    case '@': { // insert blank characters
        uint32_t const n { std::min(csi_count(params), _width - _cursor.column) };
        for (uint32_t column { _width }; column-- > _cursor.column + n;) {
            at(_cursor.line, column) = at(_cursor.line, column - n);
        }
        erase_cells(_cursor.line, _cursor.column, _cursor.column + n);
        break;
    }
    default: break;
    }
}

auto Terminal::apply_sgr() -> void {
    auto const params { parameters() };
    for (size_t i { 0 }; i < params.size(); ++i) {
        int const p { params[i] };
        switch (p) {
        case 0:
            _foreground = _default_foreground;
            _background = _default_background;
            break;
        case 1: _foreground.style.bold = true; break;
        case 3: _foreground.style.italic = true; break;
        case 4: _foreground.style.underline = true; break;
        case 7: _foreground.style.selected = true; break;
        case 9: _foreground.style.strikethrough = true; break;
        case 22: _foreground.style.bold = false; break;
        case 23: _foreground.style.italic = false; break;
        case 24: _foreground.style.underline = false; break;
        case 27: _foreground.style.selected = false; break;
        case 29: _foreground.style.strikethrough = false; break;
        case 39: {
            auto const style { _foreground.style };
            _foreground = _default_foreground;
            _foreground.style = style;
            break;
        }
        case 49: _background = _default_background; break;
        case 38:
        case 48: {
            Color color {};
            if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size()) {
                color = indexed_color(params[i + 2], {});
                i += 2;
            } else if (i + 1 < params.size() && params[i + 1] == 2 && i + 4 < params.size()) {
                color = {
                    static_cast<uint8_t>(std::clamp(params[i + 2], 0, 255)),
                    static_cast<uint8_t>(std::clamp(params[i + 3], 0, 255)),
                    static_cast<uint8_t>(std::clamp(params[i + 4], 0, 255)),
                    {},
                };
                i += 4;
            } else {
                break;
            }
            if (p == 38) {
                color.style = _foreground.style;
                _foreground = color;
            } else {
                _background = color;
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37) {
                auto const style { _foreground.style };
                _foreground = indexed_color(p - 30, style);
            } else if (p >= 90 && p <= 97) {
                auto const style { _foreground.style };
                _foreground = indexed_color(p - 90 + 8, style);
            } else if (p >= 40 && p <= 47) {
                _background = indexed_color(p - 40, {});
            } else if (p >= 100 && p <= 107) {
                _background = indexed_color(p - 100 + 8, {});
            }
            break;
        }
    }
}

// -- geometry and history ------------------------------------------------

auto Terminal::resize(uint32_t width, uint32_t height) -> void {
    uint32_t const new_width { std::max(width, 1u) };
    uint32_t const new_height { std::max(height, 1u) };
    std::vector<TerminalCell> cells(
        static_cast<size_t>(new_width) * new_height,
        TerminalCell { " ", _default_foreground, _default_background }
    );
    for (uint32_t line { 0 }; line < std::min(_height, new_height); ++line) {
        for (uint32_t column { 0 }; column < std::min(_width, new_width); ++column) {
            cells[static_cast<size_t>(line) * new_width + column]
                = std::move(at(line, column));
        }
    }
    _cells = std::move(cells);
    _width = new_width;
    _height = new_height;
    _cursor.line = std::min(_cursor.line, _height - 1);
    _cursor.column = std::min(_cursor.column, _width - 1);
}

auto Terminal::line_text(uint32_t line) const -> std::string {
    std::string out;
    out.reserve(_width);
    for (uint32_t column { 0 }; column < _width; ++column) {
        out += cell(line, column).glyph;
    }
    return out;
}

auto Terminal::take_scrollback() -> std::vector<std::string> {
    return std::exchange(_scrollback, {});
}

auto Terminal::take_responses() -> std::string {
    return std::exchange(_responses, {});
}

auto Terminal::screen_text() const -> std::vector<std::string> {
    std::vector<std::string> lines;
    lines.reserve(_height);
    for (uint32_t line { 0 }; line < _height; ++line) {
        lines.push_back(trim_trailing_blanks(line_text(line)));
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

}

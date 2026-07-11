#include "spice/core/Grid.hpp"
#include "spice/core/Utf8.hpp"

#include <cstddef>
#include <cstdio>
#include <format>
#include <iterator>

namespace {

using spice::core::Color;
using spice::core::utf8_length;

//! Byte offset of the start of the `column`-th UTF-8 character in `line`.
//! Returns line.size() if `column` runs past the last character in the line.
auto column_offset(std::string const& line, uint32_t column) -> size_t {
    size_t offset { 0 };
    for (uint32_t i { 0 }; i < column && offset < line.size(); ++i) {
        offset += utf8_length(line[offset]);
    }
    return offset;
}

auto in_bounds(spice::core::Position position, uint32_t width, uint32_t height) -> bool {
    return position.line < height && position.column < width;
}

//! Index of `position` in the flat, row-major _text_color/_background_color vectors.
auto cell_index(spice::core::Position position, uint32_t width) -> size_t {
    return static_cast<size_t>(position.line) * width + position.column;
}

//! Appends one SGR sequence setting everything a cell needs: reset (to clear
//! the previous cell's flags), style flags, then truecolor fg/bg.
auto append_sgr(std::string& out, Color style, Color background) -> void {
    out += "\x1b[0";
    if (style.style.bold) out += ";1";
    if (style.style.italic) out += ";3";
    if (style.style.underline) out += ";4";
    if (style.style.strikethrought) out += ";9";
    if (style.style.blinking) out += ";5";
    if (style.style.selected) out += ";7";
    std::format_to(
        std::back_inserter(out), ";38;2;{};{};{};48;2;{};{};{}m",
        style.r, style.g, style.b,
        background.r, background.g, background.b
    );
}

}

namespace spice::core {

Grid::Grid(uint32_t width, uint32_t height)
    : _width { width }
    , _height { height }
    , _text(height, std::string(width, ' '))
    , _text_color(static_cast<size_t>(width) * height)
    , _background_color(static_cast<size_t>(width) * height)
{}

auto Grid::width() -> uint32_t {
    return _width;
}

auto Grid::height() -> uint32_t {
    return _height;
}

auto Grid::char_at(Position position) -> std::string_view {
    if (!in_bounds(position, _width, _height)) {
        return {};
    }

    std::string const& line { _text[position.line] };
    size_t const offset { column_offset(line, position.column) };
    if (offset >= line.size()) {
        return {};
    }

    return std::string_view(line).substr(offset, utf8_length(line[offset]));
}

auto Grid::set_text(Position position, std::string_view text) -> bool {
    if (!in_bounds(position, _width, _height) || text.empty()) {
        return false;
    }

    std::string& line { _text[position.line] };
    size_t const offset { column_offset(line, position.column) };
    if (offset >= line.size()) {
        return false;
    }

    line.replace(offset, utf8_length(line[offset]), text);
    return true;
}

auto Grid::line_at(uint32_t lineno) -> std::string_view {
    if (lineno >= _height) {
        return {};
    }
    return _text[lineno];
}

auto Grid::style_at(Position position) -> Color {
    if (!in_bounds(position, _width, _height)) {
        return {};
    }
    return _text_color[cell_index(position, _width)];
}

auto Grid::set_style(Position position, Color color) -> bool {
    if (!in_bounds(position, _width, _height)) {
        return false;
    }
    _text_color[cell_index(position, _width)] = color;
    return true;
}

auto Grid::background_at(Position position) -> Color {
    if (!in_bounds(position, _width, _height)) {
        return {};
    }
    return _background_color[cell_index(position, _width)];
}

auto Grid::set_background(Position position, Color color) -> bool {
    if (!in_bounds(position, _width, _height)) {
        return false;
    }
    _background_color[cell_index(position, _width)] = color;
    return true;
}

auto Grid::render(TermInfo& terminfo, Position position) -> void {
    uint32_t const term_width { terminfo.width() };
    uint32_t const term_height { terminfo.height() };

    std::string frame;
    frame.reserve(static_cast<size_t>(_width) * _height * 4);

    Color last_style {};
    Color last_background {};
    bool first_cell { true };

    for (uint32_t row { 0 }; row < _height; ++row) {
        uint32_t const term_row { position.line + row };
        if (term_row >= term_height) {
            break;
        }

        std::format_to(
            std::back_inserter(frame), "\x1b[{};{}H",
            term_row + 1, position.column + 1
        );

        // walk the line's utf-8 incrementally rather than through char_at,
        // which would re-scan the line from the start for every cell
        std::string const& line { _text[row] };
        size_t offset { 0 };

        for (uint32_t column { 0 }; column < _width && offset < line.size(); ++column) {
            if (position.column + column >= term_width) {
                break;
            }

            size_t const length { utf8_length(line[offset]) };
            size_t const index { cell_index({ row, column, 0 }, _width) };
            Color const style { _text_color[index] };
            Color const background { _background_color[index] };

            if (first_cell || style != last_style || background != last_background) {
                append_sgr(frame, style, background);
                last_style = style;
                last_background = background;
                first_cell = false;
            }

            frame.append(line, offset, length);
            offset += length;
        }
    }

    frame += "\x1b[0m";
    fwrite(frame.data(), 1, frame.size(), stdout);
}

auto Grid::render_cell(TermInfo& terminfo, Position position, Position cell) -> void {
    if (!in_bounds(cell, _width, _height)) {
        return;
    }
    uint32_t const term_row { position.line + cell.line };
    uint32_t const term_column { position.column + cell.column };
    if (term_row >= terminfo.height() || term_column >= terminfo.width()) {
        return;
    }

    std::string out;
    std::format_to(std::back_inserter(out), "\x1b[{};{}H", term_row + 1, term_column + 1);
    append_sgr(out, style_at(cell), background_at(cell));
    out += char_at(cell);
    out += "\x1b[0m";
    fwrite(out.data(), 1, out.size(), stdout);
}

}

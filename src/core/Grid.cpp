#include "spice/core/Grid.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <format>
#include <iterator>

namespace {

using spice::core::Color;

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
    if (style.style.strikethrough) out += ";9";
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
    , _glyphs(static_cast<size_t>(width) * height, " ")
    , _text_color(static_cast<size_t>(width) * height)
    , _background_color(static_cast<size_t>(width) * height)
{}

auto Grid::width() const -> uint32_t {
    return _width;
}

auto Grid::height() const -> uint32_t {
    return _height;
}

auto Grid::char_at(Position position) const -> std::string_view {
    if (!in_bounds(position, _width, _height)) {
        return {};
    }
    return _glyphs[cell_index(position, _width)];
}

auto Grid::set_text(Position position, std::string_view text) -> bool {
    if (!in_bounds(position, _width, _height) || text.empty()) {
        return false;
    }
    _glyphs[cell_index(position, _width)] = text;
    return true;
}

auto Grid::line_text(uint32_t lineno) const -> std::string {
    if (lineno >= _height) {
        return {};
    }
    std::string out;
    out.reserve(_width);
    for (uint32_t column { 0 }; column < _width; ++column) {
        out += _glyphs[cell_index({ lineno, column, 0 }, _width)];
    }
    return out;
}

auto Grid::style_at(Position position) const -> Color {
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

auto Grid::background_at(Position position) const -> Color {
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

auto Grid::render(TermInfo& terminfo, Position position) const -> void {
    render_rect(terminfo, position, Rectangle {
        .position = { 0, 0, 0 },
        .width = _width,
        .height = _height,
    });
}

auto Grid::render_cell(TermInfo& terminfo, Position position, Position cell) const -> void {
    render_rect(terminfo, position, Rectangle {
        .position = { cell.line, cell.column, 0 },
        .width = 1,
        .height = 1,
    });
}

auto Grid::render_rect(TermInfo& terminfo, Position position, Rectangle rect) const -> void {
    uint32_t const term_width { terminfo.width() };
    uint32_t const term_height { terminfo.height() };

    // clip the rectangle to the grid
    if (rect.position.line >= _height || rect.position.column >= _width) {
        return;
    }
    uint32_t const first_line { rect.position.line };
    uint32_t const first_column { rect.position.column };
    uint32_t const end_line { std::min(first_line + rect.height, _height) };
    uint32_t const end_column { std::min(first_column + rect.width, _width) };

    std::string frame;
    frame.reserve(static_cast<size_t>(rect.width) * rect.height * 4);

    Color last_style {};
    Color last_background {};
    bool first_cell { true };

    for (uint32_t row { first_line }; row < end_line; ++row) {
        uint32_t const term_row { position.line + row };
        if (term_row >= term_height) {
            break;
        }

        std::format_to(
            std::back_inserter(frame), "\x1b[{};{}H",
            term_row + 1, position.column + first_column + 1
        );

        for (uint32_t column { first_column }; column < end_column; ++column) {
            if (position.column + column >= term_width) {
                break;
            }

            size_t const index { cell_index({ row, column, 0 }, _width) };
            Color const style { _text_color[index] };
            Color const background { _background_color[index] };

            if (first_cell || style != last_style || background != last_background) {
                append_sgr(frame, style, background);
                last_style = style;
                last_background = background;
                first_cell = false;
            }

            frame += _glyphs[index];
        }
    }

    frame += "\x1b[0m";
    fwrite(frame.data(), 1, frame.size(), stdout);
}

namespace {

auto darken(Color color) -> Color {
    color.r = static_cast<uint8_t>(color.r / 2);
    color.g = static_cast<uint8_t>(color.g / 2);
    color.b = static_cast<uint8_t>(color.b / 2);
    return color;
}

auto darken_cell(Grid& grid, Position cell) -> void {
    grid.set_style(cell, darken(grid.style_at(cell)));
    grid.set_background(cell, darken(grid.background_at(cell)));
}

}

auto drop_shadow(Grid& grid, Rectangle rect) -> void {
    return;
    uint32_t const below { rect.position.line + rect.height };
    uint32_t const beside { rect.position.column + rect.width };
    for (uint32_t column { rect.position.column + 1 }; column <= beside; ++column) {
        darken_cell(grid, { below, column, 0 });
    }
    for (uint32_t line { rect.position.line + 1 }; line < below; ++line) {
        darken_cell(grid, { line, beside, 0 });
    }
}

}

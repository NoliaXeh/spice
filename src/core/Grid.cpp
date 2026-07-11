#include "spice/core/Grid.hpp"

#include <cstddef>

namespace {

//! Number of bytes in the UTF-8 character starting at `lead`.
//! An invalid leading byte is treated as a single byte, so callers always
//! make forward progress instead of looping on malformed input.
auto utf8_length(char lead) -> size_t {
    auto const byte { static_cast<unsigned char>(lead) };
    if ((byte & 0x80) == 0x00) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;
}

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

}

#ifndef SPICE_CORE_GRID_H
#define SPICE_CORE_GRID_H

#include "spice/core/Color.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/TermInfo.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spice::core {

//! The screen model: a 2-D buffer of cells, each holding one UTF-8
//! character plus a foreground and a background Color. Everything drawn on
//! screen is composed into a Grid first, then rendered to the terminal.
//!
//! @invariant _text_color.size() == _background_color.size()
//! @invariant _text_color.size() == _width * _height
//! @invariant _text.size() == _height
//! @invariant _text[*].size() >= _width
class Grid {
public:
    //! A width x height grid of spaces with default (zeroed) colors.
    Grid(uint32_t width, uint32_t height);

    auto width() const -> uint32_t;
    auto height() const -> uint32_t;

    //! The character at a position - a string_view, because UTF-8
    //! characters span one to four bytes. Empty when out of bounds.
    auto char_at(Position position) const -> std::string_view;
    //! Replaces the character at a position (`text` should be exactly one
    //! UTF-8 character). False when out of bounds or `text` is empty.
    auto set_text(Position position, std::string_view text) -> bool;

    //! A whole line's bytes; empty when out of bounds.
    auto line_at(uint32_t lineno) const -> std::string_view;

    //! The text color/style at a position (default Color if out of bounds).
    auto style_at(Position position) const -> Color;
    //! False when the position is out of bounds.
    auto set_style(Position position, Color color) -> bool;

    //! The background color at a position (default Color if out of bounds).
    auto background_at(Position position) const -> Color;
    //! False when the position is out of bounds.
    auto set_background(Position position, Color color) -> bool;

    //! Renders the grid to the current terminal, with the grid's top-left
    //! cell placed at `position` (position.layer is ignored). Anything that
    //! would fall outside terminfo's current width/height is cropped.
    //! The frame is built in one buffer and written in one call, emitting
    //! color codes only when they change between cells.
    auto render(TermInfo& terminfo, Position position) const -> void;

    //! Renders the single cell `cell` of a grid placed at `position`.
    //! This is the cheap path for localized updates (typing, cursor edits):
    //! a handful of bytes instead of a full-screen frame. Cropped like
    //! render(); out-of-grid cells are ignored.
    auto render_cell(TermInfo& terminfo, Position position, Position cell) const -> void;

    //! Renders only the cells of `rect` (grid coordinates, layers ignored)
    //! of a grid placed at `position`: the partial-update path for damage
    //! bigger than one cell but smaller than the screen. The rectangle is
    //! clipped to the grid, then cropped to the terminal like render().
    auto render_rect(TermInfo& terminfo, Position position, Rectangle rect) const -> void;

private:
    uint32_t _width;
    uint32_t _height;

    std::vector<std::string> _text; //!< one string per line: UTF-8 makes byte widths vary
    std::vector<Color> _text_color;       //!< flat, row-major, one per cell
    std::vector<Color> _background_color; //!< flat, row-major, one per cell
};

//! Casts a drop shadow for something drawn at `rect`: the cells one row
//! below and one column right of it are darkened in place (halved RGB),
//! preserving their text - floating panes and the palette use it to lift
//! off the panes beneath. Off-grid cells are ignored.
auto drop_shadow(Grid& grid, Rectangle rect) -> void;

}

#endif // SPICE_CORE_GRID_H
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

//! Most basic form of text buffering for diplaying
//! @invariant _text_color.size() == _background_color.size()
//! @invariant _text_color.size() == _width * _height
//! @invariant _text.size() == _height
//! @invariant _text[*].size() >= _width
class Grid {
public:

    Grid(uint32_t width, uint32_t height);

    auto width() -> uint32_t;
    auto height() -> uint32_t;

    //! get char at position
    //! returns a string_view because utf8 symbols are arbitrary size
    //! if char is out of bound, returns empty string view
    auto char_at(Position position) -> std::string_view;
    auto set_text(Position position, std::string_view text) -> bool;

    //! returns empty string_view of out of bound
    auto line_at(uint32_t lineno) -> std::string_view;

    //! get the text color/style at position
    //! returns a default-constructed Color if out of bound
    auto style_at(Position position) -> Color;
    //! returns false if position is out of bound
    auto set_style(Position position, Color color) -> bool;

    //! get the background color at position
    //! returns a default-constructed Color if out of bound
    auto background_at(Position position) -> Color;
    //! returns false if position is out of bound
    auto set_background(Position position, Color color) -> bool;

    //! Renders the grid to the current terminal, with the grid's top-left
    //! cell placed at `position` (position.layer is ignored). Anything that
    //! would fall outside terminfo's current width/height is cropped.
    //! The frame is built in one buffer and written in one call, emitting
    //! color codes only when they change between cells.
    auto render(TermInfo& terminfo, Position position) -> void;

    //! Renders the single cell `cell` of a grid placed at `position`.
    //! This is the cheap path for localized updates (typing, cursor edits):
    //! a handful of bytes instead of a full-screen frame. Cropped like
    //! render(); out-of-grid cells are ignored.
    auto render_cell(TermInfo& terminfo, Position position, Position cell) -> void;

    //! Renders only the cells of `rect` (grid coordinates, layers ignored)
    //! of a grid placed at `position`: the partial-update path for damage
    //! bigger than one cell but smaller than the screen. The rectangle is
    //! clipped to the grid, then cropped to the terminal like render().
    auto render_rect(TermInfo& terminfo, Position position, Rectangle rect) -> void;

private:
    uint32_t _width;
    uint32_t _height;

    std::vector<std::string> _text; //< vector of lines because line widths can change if using utf-8
    std::vector<Color> _text_color;
    std::vector<Color> _background_color;
};

}

#endif // SPICE_CORE_GRID_H
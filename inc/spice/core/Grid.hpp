#ifndef SPICE_CORE_GRID_H
#define SPICE_CORE_GRID_H

#include "spice/core/Color.hpp"
#include "spice/core/Position.hpp"
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

private:
    uint32_t _width;
    uint32_t _height;

    std::vector<std::string> _text; //< vector of lines because line widths can change if using utf-8
    std::vector<Color> _text_color;
    std::vector<Color> _background_color;
};

}

#endif // SPICE_CORE_GRID_H
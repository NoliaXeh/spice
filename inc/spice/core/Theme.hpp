#ifndef SPICE_CORE_THEME_H
#define SPICE_CORE_THEME_H

#include "spice/core/Color.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace spice::core {

//! Maps every color usage in the UI to a Color, so that what is drawn never
//! hardcodes a color: it asks the theme for its usage instead. Swapping or
//! editing a theme later then recolors everything consistently.
class Theme {
public:
    enum class Usage : uint8_t {
        text,               //!< regular text
        background,         //!< regular text background
        selection_text,     //!< selected text
        selection_background,
        cursor,
        border,             //!< pane border, unfocused
        border_focused,     //!< pane border, focused
        titlebar_text,      //!< pane title bar: dark text...
        titlebar_background, //!< ...on a light bar, unfocused
        titlebar_background_focused,
        titlebar_button_text,       //!< the F / x buttons on the bar
        titlebar_button_background,
        error,
        warning,
        info,
        usage_count_        //!< keep last - not a usage
    };

    //! Builds the default theme.
    Theme();

    auto color(Usage usage) const -> Color;
    auto set_color(Usage usage, Color color) -> void;

private:
    std::array<Color, static_cast<std::size_t>(Usage::usage_count_)> _colors;
};

}

#endif // SPICE_CORE_THEME_H

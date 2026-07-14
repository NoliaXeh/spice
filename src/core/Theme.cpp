#include "spice/core/Theme.hpp"
#include "spice/core/Color.hpp"

#include <cstddef>

namespace {

using spice::core::Color;

auto bold(Color color) -> Color {
    color.style.bold = true;
    return color;
}

}

namespace spice::core {

Theme::Theme() {
    set_color(Usage::text, colors::light_gray);
    set_color(Usage::background, colors::dark_gray);
    set_color(Usage::selection_text, colors::black);
    set_color(Usage::selection_background, colors::cyan);
    set_color(Usage::cursor, colors::white);
    set_color(Usage::border, colors::gray);
    set_color(Usage::border_focused, colors::cyan);
    set_color(Usage::titlebar_text, colors::black);
    set_color(Usage::titlebar_background, colors::gray);
    set_color(Usage::titlebar_background_focused, colors::light_gray);
    set_color(Usage::titlebar_button_text, colors::white);
    set_color(Usage::titlebar_button_background, colors::red);
    set_color(Usage::error, bold(colors::red));
    set_color(Usage::warning, colors::orange);
    set_color(Usage::info, colors::gray);
}

auto Theme::color(Usage usage) const -> Color {
    return _colors[static_cast<size_t>(usage)];
}

auto Theme::set_color(Usage usage, Color color) -> void {
    _colors[static_cast<size_t>(usage)] = color;
}

}

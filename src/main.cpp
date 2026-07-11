#include <cstdint>
#include <print>
#include <string_view>

#include "spice/core/Color.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Spice.hpp"
#include "spice/core/TermInfo.hpp"

using namespace spice;
using core::Color;
using core::Grid;
using core::Position;

namespace {

constexpr uint32_t demo_size { 15 };

//! Builds a 15x15 grid: a letter per cell, a foreground hue and background
//! shade derived from its position, and one of five style flags cycling
//! across cells (never blinking - it's unpleasant on a real terminal).
auto make_demo_grid() -> Grid {
    Grid grid { demo_size, demo_size };

    constexpr std::string_view alphabet { "ABCDEFGHIJKLMNOPQRSTUVWXYZ" };

    for (uint32_t line { 0 }; line < demo_size; ++line) {
        for (uint32_t column { 0 }; column < demo_size; ++column) {
            Position const position { line, column, 0 };

            char const letter { alphabet[(line * demo_size + column) % alphabet.size()] };
            grid.set_text(position, std::string_view(&letter, 1));

            auto const shade { static_cast<uint8_t>(column * 255 / (demo_size - 1)) };
            auto const style_index { (line + column) % 5 };

            grid.set_style(position, Color {
                .r = shade,
                .g = static_cast<uint8_t>(line * 255 / (demo_size - 1)),
                .b = static_cast<uint8_t>(255 - shade),
                .style = {
                    .bold = style_index == 0,
                    .italic = style_index == 1,
                    .underline = style_index == 2,
                    .strikethrought = style_index == 3,
                    .blinking = false,
                    .selected = style_index == 4,
                },
            });

            grid.set_background(position, Color {
                .r = static_cast<uint8_t>(line * 8),
                .g = static_cast<uint8_t>(column * 8),
                .b = 32,
                .style = {},
            });
        }
    }

    return grid;
}

}

int main() {
    auto sp { core::Spice("Spice") };
    std::println("Hello {}!", sp.name());

    auto ti { core::TermInfo() };
    std::println("w={}, h={}, pid={}", ti.width(), ti.height(), ti.pid());

    auto grid { make_demo_grid() };
    grid.render(ti, { 2, 0, 0 });
    std::println("");
}

#include <cstdint>
#include <cstdio>
#include <print>
#include <string_view>

#include "spice/core/Event.hpp"
#include "spice/core/EventReader.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/TermInfo.hpp"
#include "spice/core/Theme.hpp"

using namespace spice;
using core::Grid;
using core::Position;
using core::Theme;

namespace {

constexpr std::string_view hint {
    "Spice POC - click/arrows move, type to overwrite, del/bksp erase, ctrl-w quits"
};

//! A terminal-sized grid, themed text-on-background, with the hint on line 0.
auto make_grid(core::TermInfo& terminfo, Theme const& theme) -> Grid {
    Grid grid { terminfo.width(), terminfo.height() };

    for (uint32_t line { 0 }; line < grid.height(); ++line) {
        for (uint32_t column { 0 }; column < grid.width(); ++column) {
            Position const cell { line, column, 0 };
            grid.set_style(cell, theme.color(Theme::Usage::text));
            grid.set_background(cell, theme.color(Theme::Usage::background));
        }
    }

    for (uint32_t i { 0 }; i < hint.size() && i < grid.width(); ++i) {
        Position const cell { 0, i, 0 };
        grid.set_text(cell, hint.substr(i, 1));
        grid.set_style(cell, theme.color(Theme::Usage::info));
    }

    return grid;
}

//! Puts the terminal cursor on the edited cell.
auto park_cursor(Position cursor) -> void {
    std::print("\x1b[{};{}H", cursor.line + 1, cursor.column + 1);
    std::fflush(stdout);
}

//! One cell forward, wrapping to the next line; stays put on the last cell.
auto advance(Grid& grid, Position& cursor) -> void {
    if (cursor.column + 1 < grid.width()) {
        ++cursor.column;
    } else if (cursor.line + 1 < grid.height()) {
        cursor.column = 0;
        ++cursor.line;
    }
}

//! One cell back, wrapping to the end of the previous line; stays put on (0,0).
auto retreat(Grid& grid, Position& cursor) -> void {
    if (cursor.column > 0) {
        --cursor.column;
    } else if (cursor.line > 0) {
        --cursor.line;
        cursor.column = grid.width() - 1;
    }
}

//! Blanks the cell under the cursor and repaints it.
auto erase(Grid& grid, core::TermInfo& terminfo, Position cursor) -> void {
    grid.set_text(cursor, " ");
    grid.render_cell(terminfo, { 0, 0, 0 }, cursor);
}

}

int main() {
    core::TermInfo terminfo;
    if (terminfo.width() == 0 || terminfo.height() == 0) {
        std::println("spice: no terminal to draw on");
        return 1;
    }

    Theme const theme;
    auto grid { make_grid(terminfo, theme) };
    Position cursor { 1, 0, 0 }; // just below the hint line

    std::print("\x1b[?1049h"); // alternate screen; the shell gets its scrollback back on exit
    std::fflush(stdout);
    core::EventReader reader;

    grid.render(terminfo, { 0, 0, 0 }); // the only full-screen frame
    park_cursor(cursor);

    bool running { true };
    while (running) {
        auto const event { reader.poll(100) };
        if (!event) {
            continue;
        }

        if (event->type == core::EventType::key) {
            auto const& key { event->key };
            switch (key.key) {
            case core::Key::character:
                if (key.mods.ctrl && key.text == "w") {
                    running = false;
                } else if (!key.mods.ctrl && !key.mods.alt) {
                    grid.set_text(cursor, key.text);
                    grid.render_cell(terminfo, { 0, 0, 0 }, cursor); // repaint just that cell
                    advance(grid, cursor);
                    park_cursor(cursor);
                }
                break;
            case core::Key::up:
                if (cursor.line > 0) --cursor.line;
                park_cursor(cursor);
                break;
            case core::Key::down:
                if (cursor.line + 1 < grid.height()) ++cursor.line;
                park_cursor(cursor);
                break;
            case core::Key::left:
                if (cursor.column > 0) --cursor.column;
                park_cursor(cursor);
                break;
            case core::Key::right:
                if (cursor.column + 1 < grid.width()) ++cursor.column;
                park_cursor(cursor);
                break;
            case core::Key::enter:
                cursor.column = 0;
                if (cursor.line + 1 < grid.height()) ++cursor.line;
                park_cursor(cursor);
                break;
            case core::Key::del: // erase under the cursor, stay put
                erase(grid, terminfo, cursor);
                park_cursor(cursor);
                break;
            case core::Key::backspace: // step back, erase what's there
                retreat(grid, cursor);
                erase(grid, terminfo, cursor);
                park_cursor(cursor);
                break;
            default:
                break;
            }
        } else if (event->mouse.action == core::MouseAction::press
                   && event->mouse.button == core::MouseButton::left) {
            cursor = event->mouse.position;
            if (cursor.line >= grid.height()) cursor.line = grid.height() - 1;
            if (cursor.column >= grid.width()) cursor.column = grid.width() - 1;
            park_cursor(cursor); // nothing changed in the grid; just move the cursor
        }
    }

    std::print("\x1b[?1049l");
    std::fflush(stdout);
}

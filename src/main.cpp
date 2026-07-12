#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "spice/core/Event.hpp"
#include "spice/core/EventReader.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
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

// ---------------------------------------------------------------
// Event log box, bottom-right corner
// ---------------------------------------------------------------

constexpr uint32_t log_width { 30 };
constexpr uint32_t log_height { 3 }; // keeps the last 3 events

//! Where the log box sits: bottom-right of the grid, shrunk to fit.
auto log_rect(Grid& grid) -> core::Rectangle {
    uint32_t const width { log_width < grid.width() ? log_width : grid.width() };
    uint32_t const height { log_height < grid.height() ? log_height : grid.height() };
    return { { grid.height() - height, grid.width() - width, 0 }, width, height };
}

auto mods_text(core::Modifiers mods) -> std::string {
    std::string out;
    if (mods.ctrl) out += "C-";
    if (mods.alt) out += "M-";
    if (mods.shift) out += "S-";
    return out;
}

auto key_name(core::Key key) -> std::string_view {
    switch (key) {
        case core::Key::character: return "char";
        case core::Key::enter: return "enter";
        case core::Key::tab: return "tab";
        case core::Key::backspace: return "backspace";
        case core::Key::escape: return "escape";
        case core::Key::up: return "up";
        case core::Key::down: return "down";
        case core::Key::left: return "left";
        case core::Key::right: return "right";
        case core::Key::home: return "home";
        case core::Key::end: return "end";
        case core::Key::page_up: return "page-up";
        case core::Key::page_down: return "page-down";
        case core::Key::insert: return "insert";
        case core::Key::del: return "delete";
        case core::Key::f1: return "f1";
        case core::Key::f2: return "f2";
        case core::Key::f3: return "f3";
        case core::Key::f4: return "f4";
        case core::Key::f5: return "f5";
        case core::Key::f6: return "f6";
        case core::Key::f7: return "f7";
        case core::Key::f8: return "f8";
        case core::Key::f9: return "f9";
        case core::Key::f10: return "f10";
        case core::Key::f11: return "f11";
        case core::Key::f12: return "f12";
    }
    return "?";
}

auto action_name(core::MouseAction action) -> std::string_view {
    switch (action) {
        case core::MouseAction::press: return "press";
        case core::MouseAction::release: return "release";
        case core::MouseAction::move: return "move";
    }
    return "?";
}

auto button_name(core::MouseButton button) -> std::string_view {
    switch (button) {
        case core::MouseButton::none: return "none";
        case core::MouseButton::left: return "left";
        case core::MouseButton::middle: return "middle";
        case core::MouseButton::right: return "right";
        case core::MouseButton::wheel_up: return "wheel-up";
        case core::MouseButton::wheel_down: return "wheel-down";
    }
    return "?";
}

auto describe(core::Event const& event) -> std::string {
    if (event.type == core::EventType::key) {
        auto const& key { event.key };
        if (key.key == core::Key::character) {
            return std::format("key {}'{}'", mods_text(key.mods), key.text);
        }
        return std::format("key {}{}", mods_text(key.mods), key_name(key.key));
    }
    auto const& mouse { event.mouse };
    return std::format(
        "mouse {}{} {} {}:{}",
        mods_text(mouse.mods), action_name(mouse.action), button_name(mouse.button),
        mouse.position.line, mouse.position.column
    );
}

//! Writes the last events into the log box (newest at the bottom) and
//! repaints only that rectangle.
auto show_events(
    Grid& grid, core::TermInfo& terminfo, Theme const& theme,
    std::deque<std::string> const& log
) -> void {
    auto const rect { log_rect(grid) };

    for (uint32_t row { 0 }; row < rect.height; ++row) {
        // bottom row shows the newest entry
        size_t const from_bottom { rect.height - row };
        std::string_view text;
        if (log.size() >= from_bottom) {
            text = log[log.size() - from_bottom];
        }

        for (uint32_t column { 0 }; column < rect.width; ++column) {
            Position const cell {
                rect.position.line + row,
                rect.position.column + column,
                0,
            };
            char const glyph { column < text.size() ? text[column] : ' ' };
            grid.set_text(cell, std::string_view(&glyph, 1));
            grid.set_style(cell, theme.color(Theme::Usage::info));
            grid.set_background(cell, core::colors::black);
        }
    }

    grid.render_rect(terminfo, { 0, 0, 0 }, rect);
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

    std::deque<std::string> event_log;

    bool running { true };
    while (running) {
        auto const event { reader.poll(100) };
        if (!event) {
            continue;
        }

        event_log.push_back(describe(*event));
        while (event_log.size() > log_height) {
            event_log.pop_front();
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
                }
                break;
            case core::Key::up:
                if (cursor.line > 0) --cursor.line;
                break;
            case core::Key::down:
                if (cursor.line + 1 < grid.height()) ++cursor.line;
                break;
            case core::Key::left:
                if (cursor.column > 0) --cursor.column;
                break;
            case core::Key::right:
                if (cursor.column + 1 < grid.width()) ++cursor.column;
                break;
            case core::Key::enter:
                cursor.column = 0;
                if (cursor.line + 1 < grid.height()) ++cursor.line;
                break;
            case core::Key::del: // erase under the cursor, stay put
                erase(grid, terminfo, cursor);
                break;
            case core::Key::backspace: // step back, erase what's there
                retreat(grid, cursor);
                erase(grid, terminfo, cursor);
                break;
            default:
                break;
            }
        } else if (event->mouse.action == core::MouseAction::press
                   && event->mouse.button == core::MouseButton::left) {
            cursor = event->mouse.position;
            if (cursor.line >= grid.height()) cursor.line = grid.height() - 1;
            if (cursor.column >= grid.width()) cursor.column = grid.width() - 1;
        }

        show_events(grid, terminfo, theme, event_log);
        park_cursor(cursor);
    }

    std::print("\x1b[?1049l");
    std::fflush(stdout);
}

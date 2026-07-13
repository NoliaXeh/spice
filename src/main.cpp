#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "spice/core/Command.hpp"
#include "spice/core/Event.hpp"
#include "spice/core/EventReader.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Palette.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/Spice.hpp"
#include "spice/core/TermInfo.hpp"
#include "spice/core/Theme.hpp"

using namespace spice;
using core::Grid;
using core::Position;
using core::Rectangle;
using core::Theme;

namespace {

//! The Master key: a prefix reaching Spice from any pane (CONFIG.md default).
//! Terminals send ctrl-space as NUL, which parses to this id.
constexpr std::string_view master_key { "C-' '" };

// ---------------------------------------------------------------
// Event descriptions (for the floating "events" pane and dispatch)
// ---------------------------------------------------------------

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

//! The position-independent identity of an event, used both as the key of
//! the binding map and inside describe(): "C-'w'", "S-up", "press left"...
auto event_id(core::Event const& event) -> std::string {
    if (event.type == core::EventType::key) {
        auto const& key { event.key };
        if (key.key == core::Key::character) {
            return std::format("{}'{}'", mods_text(key.mods), key.text);
        }
        return std::format("{}{}", mods_text(key.mods), key_name(key.key));
    }
    auto const& mouse { event.mouse };
    return std::format(
        "{}{} {}", mods_text(mouse.mods), action_name(mouse.action), button_name(mouse.button)
    );
}

auto describe(core::Event const& event) -> std::string {
    if (event.type == core::EventType::key) {
        return "key " + event_id(event);
    }
    return std::format(
        "mouse {} {}:{}", event_id(event), event.mouse.position.line, event.mouse.position.column
    );
}

// ---------------------------------------------------------------
// Editing on the focused pane
// ---------------------------------------------------------------

//! Applies an editing key to a pane; returns true if it did anything.
auto edit_pane(core::Pane& pane, core::KeyEvent const& key) -> bool {
    auto& buffer { *pane.buffer() };
    Position const cursor { pane.cursor() };

    switch (key.key) {
    case core::Key::character:
        if (buffer.insert(cursor, key.text)) {
            pane.set_cursor({ cursor.line, cursor.column + 1, 0 });
            return true;
        }
        return false;

    case core::Key::enter:
        if (buffer.split_line(cursor)) {
            pane.set_cursor({ cursor.line + 1, 0, 0 });
            return true;
        }
        return false;

    case core::Key::backspace:
        if (cursor.column > 0) {
            if (buffer.erase({ cursor.line, cursor.column - 1, 0 })) {
                pane.set_cursor({ cursor.line, cursor.column - 1, 0 });
                return true;
            }
        } else if (cursor.line > 0) { // join with the previous line
            uint32_t const column { buffer.line_length(cursor.line - 1) };
            if (buffer.erase({ cursor.line - 1, column, 0 })) {
                pane.set_cursor({ cursor.line - 1, column, 0 });
                return true;
            }
        }
        return false;

    case core::Key::del:
        return buffer.erase(cursor);

    case core::Key::left:
        if (cursor.column > 0) {
            pane.set_cursor({ cursor.line, cursor.column - 1, 0 });
        } else if (cursor.line > 0) {
            pane.set_cursor({ cursor.line - 1, buffer.line_length(cursor.line - 1), 0 });
        }
        return true;

    case core::Key::right:
        if (cursor.column < buffer.line_length(cursor.line)) {
            pane.set_cursor({ cursor.line, cursor.column + 1, 0 });
        } else if (cursor.line + 1 < buffer.line_count()) {
            pane.set_cursor({ cursor.line + 1, 0, 0 });
        }
        return true;

    case core::Key::up:
        if (cursor.line > 0) {
            pane.set_cursor({ cursor.line - 1, cursor.column, 0 });
        }
        return true;

    case core::Key::down:
        pane.set_cursor({ cursor.line + 1, cursor.column, 0 }); // set_cursor clamps
        return true;

    default:
        return false;
    }
}

}

int main() {
    core::TermInfo terminfo;
    if (terminfo.width() == 0 || terminfo.height() == 0) {
        std::println("spice: no terminal to draw on");
        return 1;
    }

    Theme const theme;
    Grid grid { terminfo.width(), terminfo.height() };
    Rectangle const screen { { 0, 0, 0 }, terminfo.width(), terminfo.height() };

    core::Spice session { "Spice" };
    session.set_screen(screen);
    uint32_t const welcome { session.open_welcome_pane() };

    // the event log: an append-only buffer in a floating grid pane,
    // bottom-right, on top of the split tree
    auto log_buffer {
        session.create_buffer("events", core::BufferCapability::append_only, "events")
    };
    uint32_t const log_width { screen.width / 3 > 20 ? screen.width / 3 : 20 };
    Rectangle const log_area {
        { screen.height - screen.height / 3, screen.width - log_width, 0 },
        log_width,
        screen.height / 3,
    };
    uint32_t const log_pane { session.open_float(core::PaneType::grid, log_buffer, log_area) };
    session.focus(welcome);

    std::print("\x1b[?1049h"); // alternate screen
    std::fflush(stdout);
    core::EventReader reader;

    // ---------------------------------------------------------------
    // State the commands and handlers mutate. `damage`/`full_repaint`
    // are reset each iteration and consumed by repaint().
    // ---------------------------------------------------------------

    bool running { true };
    uint32_t scratch_count { 0 };
    std::vector<Rectangle> damage;
    bool full_repaint { false };

    core::CommandRegistry registry;
    core::Palette palette;

    auto const mark_focused = [&] {
        if (auto const area { session.pane_area(session.focused_id()) }) {
            damage.push_back(*area);
        }
    };

    // ---------------------------------------------------------------
    // Built-in commands (README's list, minus what needs plugins/PTY)
    // ---------------------------------------------------------------

    auto const open_list_float = [&](std::string&& name, std::string const& content) {
        auto buffer {
            session.create_buffer(std::move(name), core::BufferCapability::append_only, content)
        };
        Rectangle const rect {
            { screen.position.line + 2, screen.position.column + 4, 0 },
            screen.width / 2,
            screen.height / 2,
        };
        session.open_float(core::PaneType::grid, std::move(buffer), rect);
    };

    registry.add({ "session.close", "Close Spice", [&] { running = false; } });
    registry.add({ "session.welcome", "Open Welcome Pane", [&] {
        session.open_welcome_pane();
    } });
    registry.add({ "buffer.new", "New buffer", [&] {
        auto buffer { session.create_buffer(
            std::format("scratch-{}", ++scratch_count), core::BufferCapability::editable
        ) };
        session.open_pane(core::PaneType::edit, std::move(buffer));
    } });
    registry.add({ "buffer.list", "List buffers", [&] {
        std::string content { "buffers:" };
        for (auto const& buffer : session.buffers()) {
            content += std::format(
                "\n  {} ({})", buffer->name(),
                buffer->capability() == core::BufferCapability::editable
                    ? "editable" : "append-only"
            );
        }
        open_list_float("buffers", content);
    } });
    registry.add({ "pane.list", "List panes", [&] {
        std::string content { "panes:" };
        for (uint32_t const id : session.pane_ids()) {
            content += std::format(
                "\n  #{} {}{}", id, session.pane(id)->buffer()->name(),
                id == session.focused_id() ? " (focused)" : ""
            );
        }
        open_list_float("panes", content);
    } });
    registry.add({ "pane.close", "Close current pane", [&] {
        session.close_focused_pane();
        if (session.pane_count() == 0) {
            running = false; // all panes closed: the program ends
        }
    } });
    registry.add({ "pane.focus_left", "Move focus left", [&] {
        session.move_focus(core::Direction::left);
    } });
    registry.add({ "pane.focus_right", "Move focus right", [&] {
        session.move_focus(core::Direction::right);
    } });
    registry.add({ "pane.focus_up", "Move focus up", [&] {
        session.move_focus(core::Direction::up);
    } });
    registry.add({ "pane.focus_down", "Move focus down", [&] {
        session.move_focus(core::Direction::down);
    } });
    registry.add({ "pane.float", "Float pane", [&] { session.float_focused(); } });
    registry.add({ "pane.dock", "Dock pane", [&] { session.dock_focused(); } });

    // ---------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------

    //! Clears the grid, draws every pane (palette on top), renders `rects`
    //! (all when empty), and parks the terminal cursor.
    auto const repaint = [&](std::vector<Rectangle> const& rects) {
        for (uint32_t line { 0 }; line < grid.height(); ++line) {
            for (uint32_t column { 0 }; column < grid.width(); ++column) {
                Position const cell { line, column, 0 };
                grid.set_text(cell, " ");
                grid.set_style(cell, theme.color(Theme::Usage::text));
                grid.set_background(cell, theme.color(Theme::Usage::background));
            }
        }
        session.draw(grid, theme);
        palette.draw(grid, screen, theme); // no-op while closed

        if (rects.empty()) {
            grid.render(terminfo, { 0, 0, 0 });
        } else {
            for (Rectangle const& rect : rects) {
                grid.render_rect(terminfo, { 0, 0, 0 }, rect);
            }
        }

        Position park { 0, 0, 0 };
        if (palette.is_open()) {
            park = palette.cursor_screen_position(screen);
        } else if (auto* focused { session.focused_pane() }) {
            if (auto const area { session.pane_area(session.focused_id()) }) {
                park = focused->cursor_screen_position(*area);
            }
        }
        std::print("\x1b[{};{}H", park.line + 1, park.column + 1);
        std::fflush(stdout);
    };

    // ---------------------------------------------------------------
    // Key bindings: ids map to commands by name (as config keybinds
    // will) or to interactive handlers (focus damage, clicks, palette).
    // ---------------------------------------------------------------

    auto const run_command = [&](std::string_view name) {
        registry.run(name);
        full_repaint = true;
    };

    auto const move_focus = [&](core::Direction direction) {
        mark_focused(); // old pane loses the focused border
        session.move_focus(direction);
        mark_focused(); // new pane gains it
    };

    std::function<void(core::Event const&)> const click = [&](core::Event const& event) {
        Position const point { event.mouse.position };
        if (auto const id { session.pane_at(point) }) {
            mark_focused(); // old focus border
            session.focus(*id);
            if (auto* pane { session.pane(*id) }) {
                if (auto const area { session.pane_area(*id) }) {
                    pane->set_cursor(pane->position_from_screen(*area, point));
                }
            }
            mark_focused(); // new focus border + cursor
        }
    };

    auto const open_palette = [&](core::Event const&) {
        std::vector<core::Palette::Item> items;
        for (auto const& command : registry.commands()) {
            items.push_back({ command.name, command.title });
        }
        palette.open(std::move(items));
        damage.push_back(core::Palette::area(screen));
    };

    std::unordered_map<std::string, std::function<void(core::Event const&)>> const bindings {
        { std::string(master_key), open_palette },
        { "C-'p'", open_palette }, // some terminals swallow ctrl-space
        { "C-'w'", [&](auto const&) { run_command("session.close"); } },
        { "C-'n'", [&](auto const&) { run_command("buffer.new"); } },
        { "C-'x'", [&](auto const&) { run_command("pane.close"); } },
        { "C-'f'", [&](auto const&) { run_command("pane.float"); } },
        { "C-'d'", [&](auto const&) { run_command("pane.dock"); } },
        { "C-up", [&](auto const&) { move_focus(core::Direction::up); } },
        { "C-down", [&](auto const&) { move_focus(core::Direction::down); } },
        { "C-left", [&](auto const&) { move_focus(core::Direction::left); } },
        { "C-right", [&](auto const&) { move_focus(core::Direction::right); } },
        { "press left", click },
        { "C-press left", click },
    };

    repaint({}); // first full frame

    while (running) {
        auto const event { reader.poll(100) };
        if (!event) {
            continue;
        }
        damage.clear();
        full_repaint = false;

        // every event lands in the log buffer, pane or no pane
        log_buffer->append("\n" + describe(*event));
        if (auto* pane { session.pane(log_pane) }) {
            if (auto const area { session.pane_area(log_pane) }) {
                pane->scroll_to_bottom(*area);
                damage.push_back(*area);
            }
        }

        if (palette.is_open()) {
            // modal: the palette takes every key; Master closes it again
            if (event_id(*event) == master_key) {
                palette.close();
                damage.push_back(core::Palette::area(screen));
            } else if (event->type == core::EventType::key) {
                switch (palette.handle(event->key)) {
                case core::Palette::Outcome::updated:
                    damage.push_back(core::Palette::area(screen));
                    break;
                case core::Palette::Outcome::closed:
                    damage.push_back(core::Palette::area(screen));
                    break;
                case core::Palette::Outcome::picked:
                    registry.run(palette.selected_name());
                    full_repaint = true;
                    break;
                case core::Palette::Outcome::ignored:
                    break;
                }
            }
        } else if (auto const bound { bindings.find(event_id(*event)) };
                   bound != bindings.end()) {
            bound->second(*event);
        } else if (event->type == core::EventType::key
                   && !event->key.mods.ctrl && !event->key.mods.alt) {
            // unbound plain keys go to the focused pane as editing
            if (auto* pane { session.focused_pane() }) {
                if (edit_pane(*pane, event->key)) {
                    mark_focused();
                }
            }
        }

        if (running) {
            repaint(full_repaint ? std::vector<Rectangle> {} : damage);
        }
    }

    std::print("\x1b[?1049l");
    std::fflush(stdout);
}

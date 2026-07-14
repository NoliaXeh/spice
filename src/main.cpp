#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "spice/core/FileIo.hpp"
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
    if (event.type == core::EventType::resize) {
        return "resize";
    }
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
    switch (event.type) {
    case core::EventType::key:
        return "key " + event_id(event);
    case core::EventType::resize:
        return event_id(event);
    default:
        return std::format(
            "mouse {} {}:{}",
            event_id(event), event.mouse.position.line, event.mouse.position.column
        );
    }
}

//! The bytes a terminal would send for this key - for forwarding into a PTY.
//! Empty when the key has no terminal encoding here.
auto key_to_bytes(core::KeyEvent const& key) -> std::string {
    using core::Key;
    if (key.key == Key::character) {
        if (key.mods.ctrl && key.text.size() == 1
            && key.text[0] >= 'a' && key.text[0] <= 'z') {
            return std::string(1, static_cast<char>(key.text[0] - 'a' + 1));
        }
        if (key.mods.alt) {
            return "\x1b" + key.text;
        }
        return key.text;
    }
    switch (key.key) {
        case Key::enter: return "\r";
        case Key::tab: return "\t";
        case Key::backspace: return "\x7f";
        case Key::escape: return "\x1b";
        case Key::up: return "\x1b[A";
        case Key::down: return "\x1b[B";
        case Key::right: return "\x1b[C";
        case Key::left: return "\x1b[D";
        case Key::home: return "\x1b[H";
        case Key::end: return "\x1b[F";
        case Key::del: return "\x1b[3~";
        case Key::insert: return "\x1b[2~";
        case Key::page_up: return "\x1b[5~";
        case Key::page_down: return "\x1b[6~";
        default: return {};
    }
}

// ---------------------------------------------------------------
// Editing on the focused pane
// ---------------------------------------------------------------

//! Standard base64, for pushing the clipboard to the terminal via OSC 52.
auto base64(std::string_view input) -> std::string {
    constexpr std::string_view table {
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    };
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);

    size_t i { 0 };
    for (; i + 3 <= input.size(); i += 3) {
        uint32_t const n {
            static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16
            | static_cast<uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8
            | static_cast<uint32_t>(static_cast<unsigned char>(input[i + 2]))
        };
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (input.size() - i == 1) {
        uint32_t const n { static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16 };
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += "==";
    } else if (input.size() - i == 2) {
        uint32_t const n {
            static_cast<uint32_t>(static_cast<unsigned char>(input[i])) << 16
            | static_cast<uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8
        };
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

//! True for keys that move the cursor (and so extend or drop a selection).
auto is_movement(core::Key key) -> bool {
    switch (key) {
        case core::Key::up:
        case core::Key::down:
        case core::Key::left:
        case core::Key::right:
        case core::Key::home:
        case core::Key::end:
        case core::Key::page_up:
        case core::Key::page_down:
            return true;
        default:
            return false;
    }
}

//! Applies an editing key to a pane; returns true if it did anything.
//! `page` is how many lines page-up/down jump (the pane's content height);
//! `clipboard` receives cut text (shift-delete).
auto edit_pane(
    core::Pane& pane, core::KeyEvent const& key, uint32_t page, std::string& clipboard
) -> bool {
    auto& buffer { *pane.buffer() };

    // shift + movement extends a selection from the current spot; plain
    // movement drops it
    if (is_movement(key.key)) {
        if (key.mods.shift) {
            if (!pane.has_anchor()) {
                pane.set_anchor(pane.cursor());
            }
        } else {
            pane.clear_anchor();
        }
    }

    //! Removes the selected range (one undo step); cursor lands at its start.
    auto const erase_selection = [&]() -> bool {
        auto const range { pane.selection() };
        if (!range || !buffer.erase_range(range->first, range->second)) {
            return false;
        }
        pane.set_cursor(range->first);
        pane.clear_anchor();
        return true;
    };

    Position const cursor { pane.cursor() };

    switch (key.key) {
    case core::Key::character: {
        erase_selection(); // typing replaces the selection
        Position const at { pane.cursor() };
        if (buffer.insert(at, key.text)) {
            pane.set_cursor({ at.line, at.column + 1, 0 });
            return true;
        }
        return false;
    }

    case core::Key::enter: {
        erase_selection();
        Position const at { pane.cursor() };
        if (buffer.split_line(at)) {
            pane.set_cursor({ at.line + 1, 0, 0 });
            return true;
        }
        return false;
    }

    case core::Key::backspace:
        if (erase_selection()) {
            return true;
        }
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
        if (key.mods.shift) { // shift-delete: cut
            if (auto const range { pane.selection() }) {
                clipboard = buffer.text_between(range->first, range->second);
            }
        }
        if (erase_selection()) {
            return true;
        }
        return buffer.erase(cursor);

    case core::Key::escape:
        pane.clear_anchor();
        return true;

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

    case core::Key::home:
        pane.set_cursor({ cursor.line, 0, 0 });
        return true;

    case core::Key::end:
        pane.set_cursor({ cursor.line, buffer.line_length(cursor.line), 0 });
        return true;

    case core::Key::page_up:
        pane.set_cursor({ cursor.line > page ? cursor.line - page : 0, cursor.column, 0 });
        return true;

    case core::Key::page_down:
        pane.set_cursor({ cursor.line + page, cursor.column, 0 }); // clamped
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
    Rectangle screen { { 0, 0, 0 }, terminfo.width(), terminfo.height() }; // updated on resize

    core::Spice session { "Spice" };
    session.set_screen(screen);
    uint32_t const welcome { session.open_welcome_pane() };

    // the event log: an append-only buffer in a floating grid pane,
    // bottom-right, on top of the split tree
    auto log_buffer {
        session.create_buffer("events", core::BufferCapability::append_only, "events")
    };
    auto const log_rect = [&screen]() -> Rectangle {
        uint32_t const width { screen.width / 3 > 20 ? screen.width / 3 : 20 };
        return {
            { screen.height - screen.height / 3, screen.width - width, 0 },
            width,
            screen.height / 3,
        };
    };
    uint32_t const log_pane { session.open_float(core::PaneType::grid, log_buffer, log_rect()) };
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
    auto const make_scratch = [&] {
        return session.create_buffer(
            std::format("scratch-{}", ++scratch_count), core::BufferCapability::editable
        );
    };

    registry.add({ "buffer.new", "New buffer", [&] {
        session.open_pane(core::PaneType::edit, make_scratch());
    } });
    registry.add({ "pane.split_vertical", "Split vertical (side by side)", [&] {
        session.open_pane(core::PaneType::edit, make_scratch(), true);
    } });
    registry.add({ "pane.split_horizontal", "Split horizontal (stacked)", [&] {
        session.open_pane(core::PaneType::edit, make_scratch(), false);
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
    registry.add({ "pty.shell", "Run a shell in a new PTY", [&] {
        char const* shell { std::getenv("SHELL") };
        session.open_pty_pane({ shell != nullptr ? shell : "/bin/sh" });
    } });
    // ---------------------------------------------------------------
    // File commands. Open/Save-as need a path: they reuse the palette
    // as a free-text prompt, and prompt_action receives what was typed.
    // ---------------------------------------------------------------

    std::function<void(std::string const&)> prompt_action;

    auto const open_prompt = [&](std::string title,
                                 std::function<void(std::string const&)> action) {
        prompt_action = std::move(action);
        palette.open_input(std::move(title));
        damage.push_back(core::Palette::area(screen));
    };

    auto const save_focused_to = [&](std::string const& path) {
        auto* pane { session.focused_pane() };
        if (pane == nullptr || path.empty()) {
            return;
        }
        auto& buffer { *pane->buffer() };
        buffer.set_path(path);
        if (core::write_file(path, buffer)) {
            buffer.mark_saved();
        }
    };

    registry.add({ "file.open", "Open file", [&] {
        open_prompt("Open file", [&](std::string const& path) {
            if (path.empty()) {
                return;
            }
            auto const content { core::read_file(path) };
            auto buffer { session.create_buffer(
                std::string(path), core::BufferCapability::editable, content.value_or("")
            ) };
            buffer->set_path(path);
            session.open_pane(core::PaneType::edit, std::move(buffer));
        });
    } });
    registry.add({ "file.save", "Save", [&] {
        auto* pane { session.focused_pane() };
        if (pane == nullptr
            || pane->buffer()->capability() != core::BufferCapability::editable) {
            return;
        }
        if (pane->buffer()->path().empty()) { // never saved: ask where
            open_prompt("Save as", save_focused_to);
        } else {
            save_focused_to(pane->buffer()->path());
        }
    } });
    registry.add({ "file.save_as", "Save as", [&] {
        auto* pane { session.focused_pane() };
        if (pane == nullptr
            || pane->buffer()->capability() != core::BufferCapability::editable) {
            return;
        }
        open_prompt("Save as", save_focused_to);
    } });

    // ---------------------------------------------------------------
    // Clipboard. Copies also go to the system clipboard via OSC 52
    // (write-only; terminals that support it pick it up).
    // ---------------------------------------------------------------

    std::string clipboard;

    auto const push_system_clipboard = [&](std::string const& text) {
        std::print("\x1b]52;c;{}\x07", base64(text));
        std::fflush(stdout);
    };

    registry.add({ "edit.copy", "Copy selection", [&] {
        if (auto* pane { session.focused_pane() }) {
            if (auto const range { pane->selection() }) {
                clipboard = pane->buffer()->text_between(range->first, range->second);
                push_system_clipboard(clipboard);
            }
        }
    } });
    registry.add({ "edit.cut", "Cut selection", [&] {
        if (auto* pane { session.focused_pane() }) {
            if (auto const range { pane->selection() }) {
                clipboard = pane->buffer()->text_between(range->first, range->second);
                push_system_clipboard(clipboard);
                if (pane->buffer()->erase_range(range->first, range->second)) {
                    pane->set_cursor(range->first);
                    pane->clear_anchor();
                }
            }
        }
    } });
    registry.add({ "edit.paste", "Paste", [&] {
        auto* pane { session.focused_pane() };
        if (pane == nullptr || clipboard.empty()) {
            return;
        }
        if (auto const range { pane->selection() }) { // paste replaces it
            if (pane->buffer()->erase_range(range->first, range->second)) {
                pane->set_cursor(range->first);
                pane->clear_anchor();
            }
        }
        if (auto const end { pane->buffer()->insert_block(pane->cursor(), clipboard) }) {
            pane->set_cursor(*end);
        }
    } });

    registry.add({ "buffer.undo", "Undo", [&] {
        if (auto* pane { session.focused_pane() }) {
            if (auto const position { pane->buffer()->undo() }) {
                pane->set_cursor(*position);
            }
        }
    } });
    registry.add({ "buffer.redo", "Redo", [&] {
        if (auto* pane { session.focused_pane() }) {
            if (auto const position { pane->buffer()->redo() }) {
                pane->set_cursor(*position);
            }
        }
    } });

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

    //! An in-progress mouse drag. Grabbed by the border, a pane moves:
    //! floats follow the pointer, docked panes swap with the drop target.
    //! Grabbed by the content, the drag selects text under the cursor.
    struct Drag {
        enum class Kind : uint8_t { pane, text };
        bool active { false };
        Kind kind { Kind::pane };
        uint32_t pane { 0 };
        uint32_t grab_line { 0 };   //!< press offset inside the pane
        uint32_t grab_column { 0 };
    };
    Drag drag;

    std::function<void(core::Event const&)> const click = [&](core::Event const& event) {
        Position const point { event.mouse.position };
        if (auto const id { session.pane_at(point) }) {
            mark_focused(); // old focus border
            session.focus(*id);
            if (auto* pane { session.pane(*id) }) {
                if (auto const area { session.pane_area(*id) }) {
                    if (!core::Pane::content_area(*area).contains(point)) {
                        // grabbed by the border: start moving the pane
                        drag = Drag {
                            true, Drag::Kind::pane, *id,
                            point.line - area->position.line,
                            point.column - area->position.column,
                        };
                    } else {
                        // content press: place the cursor and arm text
                        // selection (it materializes once the drag moves)
                        pane->set_cursor(pane->position_from_screen(*area, point));
                        pane->clear_anchor();
                        pane->set_anchor(pane->cursor());
                        drag = Drag { true, Drag::Kind::text, *id, 0, 0 };
                    }
                }
            }
            mark_focused(); // new focus border + cursor
        }
    };

    auto const drag_update = [&](core::MouseEvent const& mouse) {
        if (mouse.action == core::MouseAction::move) {
            if (drag.kind == Drag::Kind::text) {
                // extend the selection to the cell under the pointer
                if (auto* pane { session.pane(drag.pane) }) {
                    if (auto const area { session.pane_area(drag.pane) }) {
                        pane->set_cursor(pane->position_from_screen(*area, mouse.position));
                        damage.push_back(*area);
                    }
                }
            } else if (session.is_floating(drag.pane)) {
                // a float follows the pointer, keeping the grab point under it
                if (auto const area { session.pane_area(drag.pane) }) {
                    damage.push_back(*area); // reveal what it uncovers
                    long line { static_cast<long>(mouse.position.line) - drag.grab_line };
                    long column { static_cast<long>(mouse.position.column) - drag.grab_column };
                    long const max_line { static_cast<long>(screen.height) - 2 };
                    long const max_column { static_cast<long>(screen.width) - 2 };
                    if (line < 0) line = 0;
                    if (line > max_line) line = max_line;
                    if (column < 0) column = 0;
                    if (column > max_column) column = max_column;

                    Rectangle const moved {
                        { static_cast<uint32_t>(line), static_cast<uint32_t>(column), 0 },
                        area->width,
                        area->height,
                    };
                    session.move_float(drag.pane, moved);
                    damage.push_back(moved);
                }
            }
        } else if (mouse.action == core::MouseAction::release) {
            if (drag.kind == Drag::Kind::pane && !session.is_floating(drag.pane)) {
                // a docked pane swaps with whatever it is dropped on
                if (auto const target { session.pane_at(mouse.position) };
                    target && *target != drag.pane) {
                    session.swap_panes(drag.pane, *target);
                    full_repaint = true;
                }
            }
            drag.active = false;
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
        { "M-'p'", open_palette }, // some terminals swallow ctrl-space
        { "C-'w'", [&](auto const&) { run_command("session.close"); } },
        { "C-'n'", [&](auto const&) { run_command("buffer.new"); } },
        { "C-'x'", [&](auto const&) { run_command("pane.close"); } },
        { "C-'f'", [&](auto const&) { run_command("pane.float"); } },
        { "C-'d'", [&](auto const&) { run_command("pane.dock"); } },
        { "C-'z'", [&](auto const&) { registry.run("buffer.undo"); mark_focused(); } },
        { "C-'y'", [&](auto const&) { registry.run("buffer.redo"); mark_focused(); } },
        { "C-'s'", [&](auto const&) { run_command("file.save"); } },
        { "C-'o'", [&](auto const&) { run_command("file.open"); } },
        // copy/paste in edit panes; PTY panes still get the control bytes
        { "C-'c'", [&](auto const&) {
            if (auto* pane { session.focused_pane() };
                pane != nullptr && pane->type() == core::PaneType::pty) {
                session.write_to_pty(session.focused_id(), "\x03");
            } else {
                registry.run("edit.copy");
            }
        } },
        { "C-'v'", [&](auto const&) {
            if (auto* pane { session.focused_pane() };
                pane != nullptr && pane->type() == core::PaneType::pty) {
                session.write_to_pty(session.focused_id(), "\x16");
            } else {
                registry.run("edit.paste");
                mark_focused();
            }
        } },
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
        damage.clear();
        full_repaint = false;

        // drain running shells into their scrollback, event or not
        for (uint32_t const id : session.pump_ptys()) {
            if (auto* pane { session.pane(id) }) {
                if (auto const area { session.pane_area(id) }) {
                    pane->scroll_to_bottom(*area);
                    damage.push_back(*area);
                }
            }
        }

        if (!event) {
            if (!damage.empty()) {
                repaint(damage);
            }
            continue;
        }

        // every event lands in the log buffer, pane or no pane
        log_buffer->append("\n" + describe(*event));
        if (auto* pane { session.pane(log_pane) }) {
            if (auto const area { session.pane_area(log_pane) }) {
                pane->scroll_to_bottom(*area);
                damage.push_back(*area);
            }
        }

        auto const cancel_prompt = [&] { prompt_action = nullptr; };

        if (event->type == core::EventType::resize) {
            // rebuild the world at the new size; floats keep their absolute
            // rects, so glue the event log back onto the bottom-right corner
            grid = Grid { terminfo.width(), terminfo.height() };
            screen = { { 0, 0, 0 }, terminfo.width(), terminfo.height() };
            session.set_screen(screen);
            session.move_float(log_pane, log_rect());
            session.resize_ptys(); // children get SIGWINCH for their new areas
            full_repaint = true;
        } else if (palette.is_open()) {
            // modal: the palette takes every key; Master closes it again
            if (event_id(*event) == master_key) {
                palette.close();
                cancel_prompt();
                damage.push_back(core::Palette::area(screen));
            } else if (event->type == core::EventType::key) {
                switch (palette.handle(event->key)) {
                case core::Palette::Outcome::updated:
                    damage.push_back(core::Palette::area(screen));
                    break;
                case core::Palette::Outcome::closed:
                    cancel_prompt();
                    damage.push_back(core::Palette::area(screen));
                    break;
                case core::Palette::Outcome::picked:
                    if (prompt_action) { // a prompt gets the typed text...
                        auto const action { std::move(prompt_action) };
                        prompt_action = nullptr;
                        action(palette.query());
                    } else { // ...the command palette runs the pick
                        registry.run(palette.selected_name());
                    }
                    full_repaint = true;
                    break;
                case core::Palette::Outcome::ignored:
                    break;
                }
            }
        } else if (event->type == core::EventType::mouse && drag.active) {
            drag_update(event->mouse); // takes every mouse event mid-drag
        } else if (auto const bound { bindings.find(event_id(*event)) };
                   bound != bindings.end()) {
            bound->second(*event);
        } else if (event->type == core::EventType::key) {
            auto* pane { session.focused_pane() };
            if (pane != nullptr && pane->type() == core::PaneType::pty) {
                // unbound keys - modifiers included, so ctrl-c reaches the
                // child - are forwarded to the pty; output comes back via
                // the pump
                if (auto const bytes { key_to_bytes(event->key) }; !bytes.empty()) {
                    session.write_to_pty(session.focused_id(), bytes);
                }
            } else if (pane != nullptr
                       && !event->key.mods.ctrl && !event->key.mods.alt) {
                // unbound plain keys go to the focused pane as editing
                uint32_t page { 1 };
                if (auto const area { session.pane_area(session.focused_id()) }) {
                    uint32_t const height { core::Pane::content_area(*area).height };
                    page = height > 0 ? height : 1;
                }
                if (edit_pane(*pane, event->key, page, clipboard)) {
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

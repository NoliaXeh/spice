//! \file main.cpp
//! Spice's composition root: wires the core into the interactive terminal
//! application - session, built-in commands, palette, key bindings and the
//! event loop. Everything reusable lives in core; this file only assembles.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "spice/core/Base64.hpp"
#include "spice/core/Buffer.hpp"
#include "spice/core/Command.hpp"
#include "spice/core/Editor.hpp"
#include "spice/core/Event.hpp"
#include "spice/core/EventReader.hpp"
#include "spice/core/EventText.hpp"
#include "spice/core/FileIo.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/KeyBytes.hpp"
#include "spice/core/Palette.hpp"
#include "spice/core/PathCompletion.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/Spice.hpp"
#include "spice/core/TermInfo.hpp"
#include "spice/core/Theme.hpp"

namespace {

using namespace spice;
using core::Grid;
using core::Position;
using core::Rectangle;
using core::Theme;

//! The Master key: a prefix reaching Spice from any pane (CONFIG.md default).
//! Terminals send ctrl-space as NUL, which parses to this id.
constexpr std::string_view master_key { "C-' '" };

//! An in-progress mouse drag. Grabbed by the border, a pane moves: floats
//! follow the pointer, docked panes swap with the drop target. Grabbed by
//! the content, the drag selects text under the cursor.
struct Drag {
    enum class Kind : uint8_t { pane, text };

    bool active { false };
    Kind kind { Kind::pane };
    uint32_t pane { 0 };
    uint32_t grab_line { 0 };   //!< press offset inside the pane
    uint32_t grab_column { 0 };
};

//! The interactive application: owns the session, the command registry, the
//! palette and the render state, and runs the event loop.
class App {
public:
    App();

    //! False when there is no terminal to draw on.
    auto ready() const -> bool;

    //! Enters the alternate screen and runs the event loop until quit.
    auto run() -> void;

private:
    // -- startup ----------------------------------------------------
    auto open_startup_panes() -> void;
    auto register_commands() -> void;
    auto make_bindings() -> void;

    // -- pane and buffer helpers ------------------------------------
    //! Where the event log float sits: bottom-right corner of the screen.
    auto log_rect() const -> Rectangle;
    //! A fresh editable scratch buffer with a unique name.
    auto make_scratch() -> std::shared_ptr<core::Buffer>;
    //! Shows `content` in a floating, append-only grid pane.
    auto open_list_float(std::string&& name, std::string const& content) -> void;

    // -- damage tracking --------------------------------------------
    //! Queues the focused pane's area for repaint.
    auto mark_focused() -> void;
    //! Runs a command by name and requests a full frame.
    auto run_command(std::string_view name) -> void;

    // -- prompts, the file picker and the clipboard -------------------
    //! Opens the palette as a free-text prompt; `action` gets the answer.
    auto open_prompt(std::string title, std::function<void(std::string const&)> action)
        -> void;
    //! Opens the "Open file" picker: path completion by default, fuzzy
    //! finding when the query starts with a space.
    auto open_file_picker() -> void;
    //! The picker's item source for one query.
    auto file_picker_items(std::string const& query) -> std::vector<core::Palette::Item>;
    //! Loads (or starts) `path` in a new edit pane.
    auto open_file(std::string const& path) -> void;
    //! Writes the focused buffer to `path` and marks it saved.
    auto save_focused_to(std::string const& path) -> void;
    //! OSC 52: hands the copied text to the terminal's clipboard.
    auto push_system_clipboard(std::string const& text) -> void;

    // -- event handling ---------------------------------------------
    //! Appends every pty's pending output to its scrollback, with damage.
    auto pump_ptys() -> void;
    //! Rebuilds grid and layout at the terminal's new size.
    auto on_resize() -> void;
    //! Routes a key to the open palette (modal).
    auto on_palette_key(core::Event const& event) -> void;
    //! A left press: focus, then border drag / cursor + selection arm.
    auto on_click(core::Event const& event) -> void;
    //! Pointer moved or released while dragging.
    auto on_drag(core::MouseEvent const& mouse) -> void;
    //! Focus movement with border damage on both sides.
    auto move_focus(core::Direction direction) -> void;
    //! Opens the command palette over the registered commands.
    auto open_palette() -> void;
    //! An unbound key: forwarded to a pty pane, or applied as editing.
    auto on_unbound_key(core::KeyEvent const& key) -> void;
    //! One polled event through the whole dispatch chain.
    auto handle(core::Event const& event) -> void;

    // -- rendering ---------------------------------------------------
    //! Clears the grid, draws panes and palette, renders the damage
    //! (everything when `rects` is empty) and parks the terminal cursor.
    auto repaint(std::vector<Rectangle> const& rects) -> void;

    core::TermInfo _terminfo;
    Theme _theme;
    Rectangle _screen; //!< updated on resize
    Grid _grid;

    core::Spice _session { "Spice" };
    std::shared_ptr<core::Buffer> _log_buffer;
    uint32_t _log_pane { 0 };

    core::CommandRegistry _registry;
    core::Palette _palette;
    std::unordered_map<std::string, std::function<void(core::Event const&)>> _bindings;

    bool _running { true };
    uint32_t _scratch_count { 0 };
    std::string _clipboard;
    std::function<void(std::string const&)> _prompt_action;
    std::function<void(std::string const&)> _picker_action; //!< gets the picked path
    Drag _drag;

    std::vector<Rectangle> _damage; //!< rectangles to render this iteration
    bool _full_repaint { false };
};

// -- startup ---------------------------------------------------------

App::App()
    : _screen { { 0, 0, 0 }, _terminfo.width(), _terminfo.height() }
    , _grid { _terminfo.width(), _terminfo.height() }
{
    if (!ready()) {
        return;
    }
    _session.set_screen(_screen);
    open_startup_panes();
    register_commands();
    make_bindings();
}

auto App::ready() const -> bool {
    return _screen.width > 0 && _screen.height > 0;
}

auto App::open_startup_panes() -> void {
    uint32_t const welcome { _session.open_welcome_pane() };

    // the event log: an append-only buffer in a floating grid pane,
    // bottom-right, on top of the split tree
    _log_buffer = _session.create_buffer(
        "events", core::BufferCapability::append_only, "events"
    );
    _log_pane = _session.open_float(core::PaneType::grid, _log_buffer, log_rect());
    _session.pane(_log_pane)->set_read_only(true);
    _session.focus(welcome);
}

auto App::register_commands() -> void {
    // session
    _registry.add({ "session.close", "Close Spice", [this] { _running = false; } });
    _registry.add({ "session.welcome", "Open Welcome Pane", [this] {
        _session.open_welcome_pane();
    } });

    // buffers and splits
    _registry.add({ "buffer.new", "New buffer", [this] {
        _session.open_pane(core::PaneType::edit, make_scratch());
    } });
    _registry.add({ "pane.split_vertical", "Split vertical (side by side)", [this] {
        _session.open_pane(core::PaneType::edit, make_scratch(), true);
    } });
    _registry.add({ "pane.split_horizontal", "Split horizontal (stacked)", [this] {
        _session.open_pane(core::PaneType::edit, make_scratch(), false);
    } });
    _registry.add({ "buffer.list", "List buffers", [this] {
        std::string content { "buffers:" };
        for (auto const& buffer : _session.buffers()) {
            content += std::format(
                "\n  {} ({})", buffer->name(),
                buffer->capability() == core::BufferCapability::editable
                    ? "editable" : "append-only"
            );
        }
        open_list_float("buffers", content);
    } });

    // panes
    _registry.add({ "pane.list", "List panes", [this] {
        std::string content { "panes:" };
        for (uint32_t const id : _session.pane_ids()) {
            content += std::format(
                "\n  #{} {}{}", id, _session.pane(id)->buffer()->name(),
                id == _session.focused_id() ? " (focused)" : ""
            );
        }
        open_list_float("panes", content);
    } });
    _registry.add({ "pane.close", "Close current pane", [this] {
        _session.close_focused_pane();
        if (_session.pane_count() == 0) {
            _running = false; // all panes closed: the program ends
        }
    } });
    _registry.add({ "pane.focus_left", "Move focus left", [this] {
        _session.move_focus(core::Direction::left);
    } });
    _registry.add({ "pane.focus_right", "Move focus right", [this] {
        _session.move_focus(core::Direction::right);
    } });
    _registry.add({ "pane.focus_up", "Move focus up", [this] {
        _session.move_focus(core::Direction::up);
    } });
    _registry.add({ "pane.focus_down", "Move focus down", [this] {
        _session.move_focus(core::Direction::down);
    } });
    _registry.add({ "pane.float", "Float pane", [this] { _session.float_focused(); } });
    _registry.add({ "pane.dock", "Dock pane", [this] { _session.dock_focused(); } });

    // pty
    _registry.add({ "pty.shell", "Run a shell in a new PTY", [this] {
        char const* shell { std::getenv("SHELL") };
        _session.open_pty_pane({ shell != nullptr ? shell : "/bin/sh" });
    } });

    // files (Open picks its path through the picker, Save-as via a prompt)
    _registry.add({ "file.open", "Open file", [this] { open_file_picker(); } });
    _registry.add({ "file.save", "Save", [this] {
        auto* pane { _session.focused_pane() };
        if (pane == nullptr
            || pane->buffer()->capability() != core::BufferCapability::editable) {
            return;
        }
        if (pane->buffer()->path().empty()) { // never saved: ask where
            open_prompt("Save as", [this](std::string const& path) { save_focused_to(path); });
        } else {
            save_focused_to(pane->buffer()->path());
        }
    } });
    _registry.add({ "file.save_as", "Save as", [this] {
        auto* pane { _session.focused_pane() };
        if (pane == nullptr
            || pane->buffer()->capability() != core::BufferCapability::editable) {
            return;
        }
        open_prompt("Save as", [this](std::string const& path) { save_focused_to(path); });
    } });

    // editing
    _registry.add({ "edit.copy", "Copy selection", [this] {
        if (auto* pane { _session.focused_pane() }) {
            if (auto const range { pane->selection() }) {
                _clipboard = pane->buffer()->text_between(range->first, range->second);
                push_system_clipboard(_clipboard);
            }
        }
    } });
    _registry.add({ "edit.cut", "Cut selection", [this] {
        if (auto* pane { _session.focused_pane() }; pane != nullptr && !pane->read_only()) {
            if (auto const range { pane->selection() }) {
                _clipboard = pane->buffer()->text_between(range->first, range->second);
                push_system_clipboard(_clipboard);
                if (pane->buffer()->erase_range(range->first, range->second)) {
                    pane->set_cursor(range->first);
                    pane->clear_anchor();
                }
            }
        }
    } });
    _registry.add({ "edit.paste", "Paste", [this] {
        auto* pane { _session.focused_pane() };
        if (pane == nullptr || pane->read_only() || _clipboard.empty()) {
            return;
        }
        if (auto const range { pane->selection() }) { // paste replaces it
            if (pane->buffer()->erase_range(range->first, range->second)) {
                pane->set_cursor(range->first);
                pane->clear_anchor();
            }
        }
        if (auto const end { pane->buffer()->insert_block(pane->cursor(), _clipboard) }) {
            pane->set_cursor(*end);
        }
    } });
    _registry.add({ "buffer.undo", "Undo", [this] {
        if (auto* pane { _session.focused_pane() }; pane != nullptr && !pane->read_only()) {
            if (auto const position { pane->buffer()->undo() }) {
                pane->set_cursor(*position);
            }
        }
    } });
    _registry.add({ "buffer.redo", "Redo", [this] {
        if (auto* pane { _session.focused_pane() }; pane != nullptr && !pane->read_only()) {
            if (auto const position { pane->buffer()->redo() }) {
                pane->set_cursor(*position);
            }
        }
    } });
}

auto App::make_bindings() -> void {
    auto const open = [this](core::Event const&) { open_palette(); };
    _bindings.emplace(std::string(master_key), open);
    _bindings.emplace("C-'p'", open); // some terminals swallow ctrl-space
    _bindings.emplace("M-'p'", open); // ...or reserve both

    auto const command = [this](std::string_view name) {
        return [this, name](core::Event const&) { run_command(name); };
    };
    _bindings.emplace("C-'w'", command("session.close"));
    _bindings.emplace("C-'n'", command("buffer.new"));
    _bindings.emplace("C-'x'", command("pane.close"));
    _bindings.emplace("C-'f'", command("pane.float"));
    _bindings.emplace("C-'d'", command("pane.dock"));
    _bindings.emplace("C-'s'", command("file.save"));
    _bindings.emplace("C-'o'", command("file.open"));

    _bindings.emplace("C-'z'", [this](core::Event const&) {
        _registry.run("buffer.undo");
        mark_focused();
    });
    _bindings.emplace("C-'y'", [this](core::Event const&) {
        _registry.run("buffer.redo");
        mark_focused();
    });

    // copy/paste in edit panes; PTY panes still get the control bytes
    _bindings.emplace("C-'c'", [this](core::Event const&) {
        if (auto* pane { _session.focused_pane() };
            pane != nullptr && pane->type() == core::PaneType::pty) {
            _session.write_to_pty(_session.focused_id(), "\x03");
        } else {
            _registry.run("edit.copy");
        }
    });
    _bindings.emplace("C-'v'", [this](core::Event const&) {
        if (auto* pane { _session.focused_pane() };
            pane != nullptr && pane->type() == core::PaneType::pty) {
            _session.write_to_pty(_session.focused_id(), "\x16");
        } else {
            _registry.run("edit.paste");
            mark_focused();
        }
    });

    _bindings.emplace("C-up", [this](core::Event const&) {
        move_focus(core::Direction::up);
    });
    _bindings.emplace("C-down", [this](core::Event const&) {
        move_focus(core::Direction::down);
    });
    _bindings.emplace("C-left", [this](core::Event const&) {
        move_focus(core::Direction::left);
    });
    _bindings.emplace("C-right", [this](core::Event const&) {
        move_focus(core::Direction::right);
    });

    auto const click = [this](core::Event const& event) { on_click(event); };
    _bindings.emplace("press left", click);
    _bindings.emplace("C-press left", click);
}

// -- pane and buffer helpers ------------------------------------------

auto App::log_rect() const -> Rectangle {
    uint32_t const width { _screen.width / 3 > 20 ? _screen.width / 3 : 20 };
    return {
        { _screen.height - _screen.height / 3, _screen.width - width, 0 },
        width,
        _screen.height / 3,
    };
}

auto App::make_scratch() -> std::shared_ptr<core::Buffer> {
    return _session.create_buffer(
        std::format("scratch-{}", ++_scratch_count), core::BufferCapability::editable
    );
}

auto App::open_list_float(std::string&& name, std::string const& content) -> void {
    auto buffer {
        _session.create_buffer(std::move(name), core::BufferCapability::append_only, content)
    };
    Rectangle const rect {
        { _screen.position.line + 2, _screen.position.column + 4, 0 },
        _screen.width / 2,
        _screen.height / 2,
    };
    uint32_t const id { _session.open_float(core::PaneType::grid, std::move(buffer), rect) };
    _session.pane(id)->set_read_only(true);
}

// -- damage tracking ---------------------------------------------------

auto App::mark_focused() -> void {
    if (auto const area { _session.pane_area(_session.focused_id()) }) {
        _damage.push_back(*area);
    }
}

auto App::run_command(std::string_view name) -> void {
    _registry.run(name);
    _full_repaint = true;
}

// -- prompts and clipboard ----------------------------------------------

auto App::open_prompt(std::string title, std::function<void(std::string const&)> action)
    -> void {
    _prompt_action = std::move(action);
    _palette.open_input(std::move(title));
    _damage.push_back(core::Palette::area(_screen));
}

auto App::open_file_picker() -> void {
    _picker_action = [this](std::string const& path) { open_file(path); };
    _palette.open_picker("Open file", [this](std::string const& query) {
        return file_picker_items(query);
    });
    _damage.push_back(core::Palette::area(_screen));
}

auto App::file_picker_items(std::string const& query) -> std::vector<core::Palette::Item> {
    std::vector<core::Palette::Item> items;
    if (!query.empty() && query.front() == ' ') { // leading space: fuzzy find
        for (auto& path : core::fuzzy_find_files(query.substr(1), 50)) {
            items.push_back({ path, path });
        }
        return items;
    }
    for (auto const& entry : core::complete_path(query)) {
        std::string const path { entry.directory ? entry.path + "/" : entry.path };
        items.push_back({ path, path });
    }
    return items;
}

auto App::open_file(std::string const& path) -> void {
    auto const content { core::read_file(path) };
    auto buffer { _session.create_buffer(
        std::string(path), core::BufferCapability::editable, content.value_or("")
    ) };
    buffer->set_path(path);
    _session.open_pane(core::PaneType::edit, std::move(buffer));
}

auto App::save_focused_to(std::string const& path) -> void {
    auto* pane { _session.focused_pane() };
    if (pane == nullptr || path.empty()) {
        return;
    }
    auto& buffer { *pane->buffer() };
    buffer.set_path(path);
    if (core::write_file(path, buffer)) {
        buffer.mark_saved();
    }
}

auto App::push_system_clipboard(std::string const& text) -> void {
    std::print("\x1b]52;c;{}\x07", core::base64_encode(text));
    std::fflush(stdout);
}

// -- event handling ------------------------------------------------------

auto App::pump_ptys() -> void {
    for (uint32_t const id : _session.pump_ptys()) {
        if (auto* pane { _session.pane(id) }) {
            if (auto const area { _session.pane_area(id) }) {
                pane->scroll_to_bottom(*area);
                _damage.push_back(*area);
            }
        }
    }
}

auto App::on_resize() -> void {
    // rebuild the world at the new size; floats keep their absolute
    // rects, so glue the event log back onto the bottom-right corner
    _grid = Grid { _terminfo.width(), _terminfo.height() };
    _screen = { { 0, 0, 0 }, _terminfo.width(), _terminfo.height() };
    _session.set_screen(_screen);
    _session.move_float(_log_pane, log_rect());
    _session.resize_ptys(); // children get SIGWINCH for their new areas
    _full_repaint = true;
}

auto App::on_palette_key(core::Event const& event) -> void {
    // modal: the palette takes every key; Master closes it again
    if (core::event_id(event) == master_key) {
        _palette.close();
        _prompt_action = nullptr;
        _picker_action = nullptr;
        _damage.push_back(core::Palette::area(_screen));
        return;
    }
    if (event.type != core::EventType::key) {
        return;
    }
    switch (_palette.handle(event.key)) {
    case core::Palette::Outcome::updated:
        _damage.push_back(core::Palette::area(_screen));
        break;
    case core::Palette::Outcome::closed:
        _prompt_action = nullptr;
        _picker_action = nullptr;
        _damage.push_back(core::Palette::area(_screen));
        break;
    case core::Palette::Outcome::picked:
        if (_picker_action) { // the file picker gets the picked path...
            std::string value { _palette.selected_name() };
            if (value.empty()) {
                value = _palette.query(); // nothing listed: the text stands
            }
            if (!value.empty() && value.back() == '/') {
                // a directory: descend, keeping the picker open inside it
                _palette.open_picker("Open file", [this](std::string const& query) {
                    return file_picker_items(query);
                });
                _palette.set_query(value);
                _damage.push_back(core::Palette::area(_screen));
                break;
            }
            auto const action { std::move(_picker_action) };
            _picker_action = nullptr;
            if (!value.empty()) {
                action(value);
            }
            _full_repaint = true;
        } else if (_prompt_action) { // ...a prompt gets the typed text...
            auto const action { std::move(_prompt_action) };
            _prompt_action = nullptr;
            action(_palette.query());
            _full_repaint = true;
        } else { // ...and the command palette runs the pick
            _registry.run(_palette.selected_name());
            _full_repaint = true;
        }
        break;
    case core::Palette::Outcome::ignored:
        break;
    }
}

auto App::on_click(core::Event const& event) -> void {
    Position const point { event.mouse.position };
    auto const id { _session.pane_at(point) };
    if (!id) {
        return;
    }
    mark_focused(); // old focus border
    _session.focus(*id);
    if (auto* pane { _session.pane(*id) }) {
        if (auto const area { _session.pane_area(*id) }) {
            if (!core::Pane::content_area(*area).contains(point)) {
                // grabbed by the border: start moving the pane
                _drag = Drag {
                    true, Drag::Kind::pane, *id,
                    point.line - area->position.line,
                    point.column - area->position.column,
                };
            } else {
                // content press: place the cursor and arm text selection
                // (it materializes once the drag moves)
                pane->set_cursor(pane->position_from_screen(*area, point));
                pane->clear_anchor();
                pane->set_anchor(pane->cursor());
                _drag = Drag { true, Drag::Kind::text, *id, 0, 0 };
            }
        }
    }
    mark_focused(); // new focus border + cursor
}

auto App::on_drag(core::MouseEvent const& mouse) -> void {
    if (mouse.action == core::MouseAction::move) {
        if (_drag.kind == Drag::Kind::text) {
            // extend the selection to the cell under the pointer
            if (auto* pane { _session.pane(_drag.pane) }) {
                if (auto const area { _session.pane_area(_drag.pane) }) {
                    pane->set_cursor(pane->position_from_screen(*area, mouse.position));
                    _damage.push_back(*area);
                }
            }
        } else if (_session.is_floating(_drag.pane)) {
            // a float follows the pointer, keeping the grab point under it
            if (auto const area { _session.pane_area(_drag.pane) }) {
                _damage.push_back(*area); // reveal what it uncovers
                long line { static_cast<long>(mouse.position.line) - _drag.grab_line };
                long column { static_cast<long>(mouse.position.column) - _drag.grab_column };
                long const max_line { static_cast<long>(_screen.height) - 2 };
                long const max_column { static_cast<long>(_screen.width) - 2 };
                if (line < 0) line = 0;
                if (line > max_line) line = max_line;
                if (column < 0) column = 0;
                if (column > max_column) column = max_column;

                Rectangle const moved {
                    { static_cast<uint32_t>(line), static_cast<uint32_t>(column), 0 },
                    area->width,
                    area->height,
                };
                _session.move_float(_drag.pane, moved);
                _damage.push_back(moved);
            }
        }
        return;
    }
    if (mouse.action == core::MouseAction::release) {
        if (_drag.kind == Drag::Kind::pane && !_session.is_floating(_drag.pane)) {
            // a docked pane swaps with whatever it is dropped on
            if (auto const target { _session.pane_at(mouse.position) };
                target && *target != _drag.pane) {
                _session.swap_panes(_drag.pane, *target);
                _full_repaint = true;
            }
        }
        _drag.active = false;
    }
}

auto App::move_focus(core::Direction direction) -> void {
    mark_focused(); // old pane loses the focused border
    _session.move_focus(direction);
    mark_focused(); // new pane gains it
}

auto App::open_palette() -> void {
    std::vector<core::Palette::Item> items;
    items.reserve(_registry.commands().size());
    for (auto const& command : _registry.commands()) {
        items.push_back({ command.name, command.title });
    }
    _palette.open(std::move(items));
    _damage.push_back(core::Palette::area(_screen));
}

auto App::on_unbound_key(core::KeyEvent const& key) -> void {
    auto* pane { _session.focused_pane() };
    if (pane == nullptr) {
        return;
    }
    if (pane->type() == core::PaneType::pty) {
        // unbound keys - modifiers included, so ctrl-c reaches the child -
        // are forwarded to the pty; output comes back through the pump
        if (auto const bytes { core::key_to_bytes(key) }; !bytes.empty()) {
            _session.write_to_pty(_session.focused_id(), bytes);
        }
        return;
    }
    if (key.mods.ctrl || key.mods.alt) {
        return;
    }
    // unbound plain keys go to the focused pane as editing
    uint32_t page { 1 };
    if (auto const area { _session.pane_area(_session.focused_id()) }) {
        uint32_t const height { core::Pane::content_area(*area).height };
        page = height > 0 ? height : 1;
    }
    if (core::apply_editing_key(*pane, key, page, _clipboard)) {
        mark_focused();
    }
}

auto App::handle(core::Event const& event) -> void {
    // every event lands in the log buffer, pane or no pane
    _log_buffer->append("\n" + core::describe(event));
    if (auto* pane { _session.pane(_log_pane) }) {
        if (auto const area { _session.pane_area(_log_pane) }) {
            pane->scroll_to_bottom(*area);
            _damage.push_back(*area);
        }
    }

    if (event.type == core::EventType::resize) {
        on_resize();
    } else if (_palette.is_open()) {
        on_palette_key(event);
    } else if (event.type == core::EventType::mouse && _drag.active) {
        on_drag(event.mouse); // takes every mouse event mid-drag
    } else if (auto const bound { _bindings.find(core::event_id(event)) };
               bound != _bindings.end()) {
        bound->second(event);
    } else if (event.type == core::EventType::key) {
        on_unbound_key(event.key);
    }
}

// -- rendering -----------------------------------------------------------

auto App::repaint(std::vector<Rectangle> const& rects) -> void {
    for (uint32_t line { 0 }; line < _grid.height(); ++line) {
        for (uint32_t column { 0 }; column < _grid.width(); ++column) {
            Position const cell { line, column, 0 };
            _grid.set_text(cell, " ");
            _grid.set_style(cell, _theme.color(Theme::Usage::text));
            _grid.set_background(cell, _theme.color(Theme::Usage::background));
        }
    }
    _session.draw(_grid, _theme);
    _palette.draw(_grid, _screen, _theme); // no-op while closed

    if (rects.empty()) {
        _grid.render(_terminfo, { 0, 0, 0 });
    } else {
        for (Rectangle const& rect : rects) {
            _grid.render_rect(_terminfo, { 0, 0, 0 }, rect);
        }
    }

    Position park { 0, 0, 0 };
    if (_palette.is_open()) {
        park = _palette.cursor_screen_position(_screen);
    } else if (auto* focused { _session.focused_pane() }) {
        if (auto const area { _session.pane_area(_session.focused_id()) }) {
            park = focused->cursor_screen_position(*area);
        }
    }
    std::print("\x1b[{};{}H", park.line + 1, park.column + 1);
    std::fflush(stdout);
}

// -- the loop --------------------------------------------------------------

auto App::run() -> void {
    std::print("\x1b[?1049h"); // alternate screen
    std::fflush(stdout);
    core::EventReader reader;

    repaint({}); // first full frame

    while (_running) {
        auto const event { reader.poll(100) };
        _damage.clear();
        _full_repaint = false;

        pump_ptys(); // shells produce output with or without user input

        if (event) {
            handle(*event);
        }
        if (_running && (_full_repaint || !_damage.empty() || event)) {
            repaint(_full_repaint ? std::vector<Rectangle> {} : _damage);
        }
    }

    std::print("\x1b[?1049l");
    std::fflush(stdout);
}

}

int main() {
    App app;
    if (!app.ready()) {
        std::println("spice: no terminal to draw on");
        return 1;
    }
    app.run();
    return 0;
}

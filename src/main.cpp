//! \file main.cpp
//! Spice's composition root: wires the core into the interactive terminal
//! application - session, built-in commands, palette, key bindings and the
//! event loop. Everything reusable lives in core; this file only assembles.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "spice/config/Config.hpp"
#include "spice/config/KeyName.hpp"
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
#include "spice/core/Utf8.hpp"
#include "spice/plugin/HostServices.hpp"
#include "spice/plugin/MsgPack.hpp"
#include "spice/plugin/PluginHost.hpp"

#include <chrono>
#include <memory>
#include <thread>

#ifndef SPICE_VERSION
#define SPICE_VERSION "dev"
#endif

namespace {

using namespace spice;
using core::Grid;
using core::Position;
using core::Rectangle;
using core::Theme;

// ---------------------------------------------------------------
// Command line
// ---------------------------------------------------------------

constexpr std::string_view usage_text {
    "Usage: spice [options] [file...]\n"
    "\n"
    "The buffer manager made to match any taste.\n"
    "\n"
    "  file...              open these files in panes (instead of the Welcome pane)\n"
    "\n"
    "Options:\n"
    "  -h, --help           show this help and exit\n"
    "  -V, --version        print the version and exit\n"
    "      --config <file>  use this config.toml instead of the XDG one\n"
    "      --no-config      skip config files entirely, using built-in defaults\n"
    "      --list-commands  print every command (for config.toml keybinds) and exit\n"
};

//! What the command line asked for. `error` set means it made no sense.
struct Options {
    bool help { false };
    bool version { false };
    bool list_commands { false };
    bool no_config { false };
    std::optional<std::string> config_file;
    std::vector<std::string> files;
    std::optional<std::string> error;
};

auto parse_options(int argc, char** argv) -> Options {
    Options options;
    bool only_files { false }; // set by "--"

    for (int i { 1 }; i < argc; ++i) {
        std::string_view const argument { argv[i] };
        if (only_files || !argument.starts_with('-')) {
            options.files.emplace_back(argument);
        } else if (argument == "--") {
            only_files = true;
        } else if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "-V" || argument == "--version") {
            options.version = true;
        } else if (argument == "--list-commands") {
            options.list_commands = true;
        } else if (argument == "--no-config") {
            options.no_config = true;
        } else if (argument == "--config") {
            if (i + 1 >= argc) {
                options.error = "--config needs a file argument";
                return options;
            }
            options.config_file = argv[++i];
        } else if (argument.starts_with("--config=")) {
            options.config_file = std::string(argument.substr(9));
        } else {
            options.error = std::format("unknown option '{}'", argument);
            return options;
        }
    }
    return options;
}

//! The built-in key bindings, as data so config keybinds can override any
//! of them: config-style key name -> command. The Master key (configurable,
//! ctrl-space by default per CONFIG.md) always opens the palette on top.
constexpr std::pair<std::string_view, std::string_view> default_bindings[] {
    { "ctrl-p", "palette.open" }, // some terminals swallow ctrl-space
    { "alt-p", "palette.open" },  // ...or reserve both
    { "ctrl-w", "session.close" },
    { "ctrl-n", "buffer.new" },
    { "ctrl-x", "pane.close" },
    { "ctrl-f", "pane.float" },
    { "ctrl-d", "pane.dock" },
    { "ctrl-s", "file.save" },
    { "ctrl-o", "file.open" },
    { "ctrl-z", "buffer.undo" },
    { "ctrl-y", "buffer.redo" },
    { "ctrl-c", "edit.copy" },  // still reaches the child in PTY panes
    { "ctrl-v", "edit.paste" },
    { "ctrl-up", "pane.focus_up" },
    { "ctrl-down", "pane.focus_down" },
    { "ctrl-left", "pane.focus_left" },
    { "ctrl-right", "pane.focus_right" },
    { "alt-right", "pane.grow_horizontal" },
    { "alt-left", "pane.shrink_horizontal" },
    { "alt-down", "pane.grow_vertical" },
    { "alt-up", "pane.shrink_vertical" },
};

//! An in-progress mouse drag. Grabbed by the border, a pane moves: floats
//! follow the pointer, docked panes swap with the drop target. Grabbed by
//! the content, the drag selects text under the cursor. Grabbed by the
//! bottom-right corner, the drag resizes the pane.
struct Drag {
    enum class Kind : uint8_t { pane, text, resize };

    bool active { false };
    Kind kind { Kind::pane };
    uint32_t pane { 0 };
    uint32_t grab_line { 0 };   //!< press offset inside the pane
    uint32_t grab_column { 0 };
};

//! The interactive application: owns the session, the command registry, the
//! palette and the render state, and runs the event loop. It also serves as
//! the plugin protocol's HostServices, so plugins act on the live session.
class App : public plugin::HostServices {
public:
    explicit App(Options const& options);

    //! False when there is no terminal to draw on.
    auto ready() const -> bool;

    //! Enters the alternate screen and runs the event loop until quit.
    auto run() -> void;

    //! Prints every registered command, for --list-commands (works without
    //! a terminal: commands are registered on demand).
    auto print_commands() -> void;

    // -- plugin::HostServices (the core surface plugins act on) -----
    auto register_commands(
        std::string const& plugin, std::vector<plugin::PluginCommand> const& commands
    ) -> void override;
    auto unregister_commands(
        std::string const& plugin, std::vector<std::string> const& names
    ) -> void override;
    auto set_keybind(
        std::string const& plugin, std::string const& command, std::string const& key
    ) -> void override;
    auto status_message(std::string const& plugin, std::string const& text) -> void override;
    auto status_error(
        std::string const& plugin, std::string const& code, std::string const& message
    ) -> void override;
    auto open_palette() -> void override;
    auto log(
        std::string const& plugin, std::string const& level, std::string const& message
    ) -> void override;
    auto open_pane(
        std::string const& kind, std::optional<uint64_t> buffer, std::string const& split
    ) -> void override;
    auto close_pane(uint64_t pane) -> void override;
    auto focus_pane(uint64_t pane) -> void override;
    auto float_pane(uint64_t pane) -> void override;
    auto dock_pane(uint64_t pane) -> void override;
    auto set_pane_buffer(uint64_t pane, uint64_t buffer) -> void override;
    auto buffer_info(uint64_t buffer) -> std::optional<plugin::BufferInfo> override;
    auto buffer_lines(uint64_t buffer, uint64_t start, uint64_t end)
        -> std::optional<std::pair<std::vector<std::string>, uint64_t>> override;
    auto create_buffer(bool editable, std::string const& name) -> uint64_t override;
    auto kill_buffer(uint64_t buffer) -> void override;
    auto splice(
        uint64_t buffer, plugin::BufferRange range, std::string const& text,
        uint64_t version, uint64_t& new_version
    ) -> plugin::SpliceStatus override;

private:
    // -- startup ----------------------------------------------------
    auto open_startup_panes(std::vector<std::string> const& files) -> void;
    auto register_builtin_commands() -> void;
    auto make_bindings() -> void;
    auto start_plugins() -> void;

    // -- plugin helpers ---------------------------------------------
    //! A stable protocol id for a core buffer (assigned on first use).
    auto buffer_id_for(std::shared_ptr<core::Buffer> const& buffer) -> uint64_t;
    auto buffer_by_id(uint64_t id) -> std::shared_ptr<core::Buffer>;
    //! Emits a core event to subscribed plugins, if the host is running.
    auto emit(std::string topic, plugin::msgpack::Value params) -> void;

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
    //! run_command, except copy/paste on a PTY pane forward the raw bytes.
    auto run_bound_command(std::string const& name) -> void;
    //! Appends a note to the event log pane.
    auto log_note(std::string const& note) -> void;
    //! The key just pressed while a palette bind was pending.
    auto finish_bind(core::Event const& event) -> void;

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

    config::Config _config;
    std::string _master_id;       //!< event id of the (configurable) Master key
    std::string _palette_run_id;  //!< runs the palette selection
    std::string _palette_bind_id; //!< starts binding a key to the selection
    std::string _pending_bind;    //!< command waiting for a key to bind to

    core::Spice _session { "Spice" };
    std::shared_ptr<core::Buffer> _log_buffer;
    uint32_t _log_pane { 0 };

    core::CommandRegistry _registry;
    core::Palette _palette;
    std::unordered_map<std::string, std::function<void(core::Event const&)>> _bindings;
    std::unordered_map<std::string, std::string> _command_keys; //!< command -> its key name

    std::unique_ptr<plugin::PluginHost> _host;
    //! registry command name -> (plugin name, plugin's command name)
    std::unordered_map<std::string, std::pair<std::string, std::string>> _plugin_commands;
    std::vector<std::weak_ptr<core::Buffer>> _plugin_buffers; //!< index = id - 1
    uint32_t _last_focused { 0 }; //!< to emit spice.pane.focused on change

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

App::App(Options const& options)
    : _screen { { 0, 0, 0 }, _terminfo.width(), _terminfo.height() }
    , _grid { _terminfo.width(), _terminfo.height() }
{
    if (!ready()) {
        return;
    }
    if (options.no_config) {
        _config = config::Config {}; // built-in defaults, no files read
    } else if (options.config_file) {
        _config = config::load(*options.config_file, config::keybinds_file_path());
    } else {
        _config = config::load();
    }
    _master_id = config::parse_key(_config.master).value_or("C-' '");
    _palette_run_id = config::parse_key(_config.palette_run).value_or("enter");
    _palette_bind_id = config::parse_key(_config.palette_bind).value_or("S-enter");

    _session.set_screen(_screen);
    open_startup_panes(options.files);
    register_builtin_commands();
    make_bindings();

    for (auto const& warning : _config.warnings) {
        log_note("config: " + warning);
    }

    _host = std::make_unique<plugin::PluginHost>(*this);
    start_plugins();
}

auto App::start_plugins() -> void {
    for (auto const& entry : _config.plugins) {
        _host->start({ entry.name, entry.command, entry.pane_mode });
    }
}

// -- plugin helpers --------------------------------------------------

auto App::buffer_id_for(std::shared_ptr<core::Buffer> const& buffer) -> uint64_t {
    for (size_t i { 0 }; i < _plugin_buffers.size(); ++i) {
        if (_plugin_buffers[i].lock() == buffer) {
            return i + 1;
        }
    }
    _plugin_buffers.push_back(buffer);
    return _plugin_buffers.size();
}

auto App::buffer_by_id(uint64_t id) -> std::shared_ptr<core::Buffer> {
    if (id == 0 || id > _plugin_buffers.size()) {
        return nullptr;
    }
    return _plugin_buffers[id - 1].lock();
}

auto App::emit(std::string topic, plugin::msgpack::Value params) -> void {
    if (_host) {
        _host->emit(topic, std::move(params));
    }
}

// -- plugin::HostServices --------------------------------------------

auto App::register_commands(
    std::string const& plugin, std::vector<plugin::PluginCommand> const& commands
) -> void {
    for (auto const& command : commands) {
        std::string const registry_name { plugin + "/" + command.name };
        _plugin_commands[registry_name] = { plugin, command.name };
        _registry.add({ registry_name, command.title, [this, registry_name] {
            auto const& [plugin_name, command_name] { _plugin_commands.at(registry_name) };
            plugin::msgpack::Value::Map fields {
                { "plugin", plugin::msgpack::Value { plugin_name } },
                { "command", plugin::msgpack::Value { command_name } },
                { "pane", plugin::msgpack::Value {
                    static_cast<uint64_t>(_session.focused_id()) } },
            };
            if (auto* pane { _session.focused_pane() }) {
                fields.emplace_back("buffer", plugin::msgpack::Value {
                    buffer_id_for(pane->buffer()) });
            }
            emit("spice.palette.command_invoked", plugin::msgpack::Value::make_map(fields));
        } });
    }
    _full_repaint = true;
}

auto App::unregister_commands(
    std::string const& plugin, std::vector<std::string> const& names
) -> void {
    auto remove = [this](std::string const& registry_name) {
        _registry.remove(registry_name);
        _plugin_commands.erase(registry_name);
        std::erase_if(_command_keys, [&](auto const& kv) { return kv.first == registry_name; });
    };
    if (names.empty()) { // all of the plugin's commands
        std::vector<std::string> mine;
        for (auto const& [registry_name, owner] : _plugin_commands) {
            if (owner.first == plugin) {
                mine.push_back(registry_name);
            }
        }
        for (auto const& registry_name : mine) {
            remove(registry_name);
        }
    } else {
        for (auto const& name : names) {
            remove(plugin + "/" + name);
        }
    }
    _full_repaint = true;
}

auto App::set_keybind(
    std::string const& plugin, std::string const& command, std::string const& key
) -> void {
    auto const id { config::parse_key(key) };
    if (!id) {
        return;
    }
    std::string const registry_name { plugin + "/" + command };
    _bindings[*id] = [this, registry_name](core::Event const&) {
        run_command(registry_name);
    };
    _command_keys[registry_name] = key;
}

auto App::status_message(std::string const& plugin, std::string const& text) -> void {
    log_note(plugin + ": " + text);
}

auto App::status_error(
    std::string const& plugin, std::string const& code, std::string const& message
) -> void {
    log_note(std::format("{}: [{}] {}", plugin, code, message));
}

auto App::log(
    std::string const& plugin, std::string const& level, std::string const& message
) -> void {
    log_note(std::format("{} [{}] {}", plugin, level, message));
}

auto App::open_pane(
    std::string const& kind, std::optional<uint64_t> buffer, std::string const& split
) -> void {
    auto content { buffer ? buffer_by_id(*buffer) : nullptr };
    if (!content) {
        content = _session.create_buffer("scratch", core::BufferCapability::editable);
    }
    core::PaneType const type {
        kind == "grid" ? core::PaneType::grid
        : kind == "pty" ? core::PaneType::pty
        : core::PaneType::edit
    };
    if (split == "h" || split == "v") {
        _session.open_pane(type, std::move(content), split == "h");
    } else {
        _session.open_pane(type, std::move(content));
    }
    _full_repaint = true;
}

auto App::close_pane(uint64_t pane) -> void {
    _session.focus(static_cast<uint32_t>(pane));
    _session.close_focused_pane();
    _full_repaint = true;
}

auto App::focus_pane(uint64_t pane) -> void {
    _session.focus(static_cast<uint32_t>(pane));
    _full_repaint = true;
}

auto App::float_pane(uint64_t pane) -> void {
    _session.focus(static_cast<uint32_t>(pane));
    _session.float_focused();
    _full_repaint = true;
}

auto App::dock_pane(uint64_t pane) -> void {
    _session.focus(static_cast<uint32_t>(pane));
    _session.dock_focused();
    _full_repaint = true;
}

auto App::set_pane_buffer(uint64_t pane, uint64_t buffer) -> void {
    (void)pane;
    (void)buffer; // the session has no rebind-pane-to-buffer op yet
}

auto App::buffer_info(uint64_t buffer) -> std::optional<plugin::BufferInfo> {
    auto const target { buffer_by_id(buffer) };
    if (!target) {
        return std::nullopt;
    }
    return plugin::BufferInfo {
        target->version(),
        target->line_count(),
        target->capability() == core::BufferCapability::editable,
        target->dirty(),
        target->name(),
    };
}

auto App::buffer_lines(uint64_t buffer, uint64_t start, uint64_t end)
    -> std::optional<std::pair<std::vector<std::string>, uint64_t>> {
    auto const target { buffer_by_id(buffer) };
    if (!target) {
        return std::nullopt;
    }
    uint64_t const last { end == static_cast<uint64_t>(-1) || end > target->line_count()
        ? target->line_count() : end };
    std::vector<std::string> lines;
    for (uint64_t i { start }; i < last; ++i) {
        lines.emplace_back(target->line(static_cast<uint32_t>(i)));
    }
    return std::pair { std::move(lines), target->version() };
}

auto App::create_buffer(bool editable, std::string const& name) -> uint64_t {
    auto buffer { _session.create_buffer(
        name.empty() ? "plugin-buffer" : std::string(name),
        editable ? core::BufferCapability::editable : core::BufferCapability::append_only
    ) };
    return buffer_id_for(buffer);
}

auto App::kill_buffer(uint64_t buffer) -> void {
    (void)buffer; // buffers are owned by the session for the whole run
}

auto App::splice(
    uint64_t buffer, plugin::BufferRange range, std::string const& text,
    uint64_t version, uint64_t& new_version
) -> plugin::SpliceStatus {
    auto const target { buffer_by_id(buffer) };
    if (!target) {
        return plugin::SpliceStatus::no_such_id;
    }
    if (target->capability() != core::BufferCapability::editable) {
        return plugin::SpliceStatus::capability_denied;
    }
    if (version != target->version()) { // staleness is loud, never silent
        new_version = target->version();
        return plugin::SpliceStatus::stale_version;
    }

    // the protocol addresses columns as byte offsets; the core in characters
    auto to_position = [&](plugin::BufferPosition p) -> core::Position {
        std::string_view const line { target->line(static_cast<uint32_t>(p.line)) };
        uint32_t const column { static_cast<uint32_t>(
            core::utf8_count(line.substr(0, std::min<size_t>(p.column, line.size())))) };
        return { static_cast<uint32_t>(p.line), column, 0 };
    };
    core::Position const begin { to_position(range.start) };
    core::Position const end { to_position(range.end) };

    if (!(begin == end)) {
        target->erase_range(begin, end);
    }
    if (!text.empty()) {
        target->insert_block(begin, text);
    }
    new_version = target->version();
    emit("spice.buffer.changed", plugin::msgpack::Value::object({
        { "buffer", plugin::msgpack::Value { buffer } },
        { "to_version", plugin::msgpack::Value { new_version } },
    }));
    return plugin::SpliceStatus::ok;
}

auto App::ready() const -> bool {
    return _screen.width > 0 && _screen.height > 0;
}

auto App::print_commands() -> void {
    if (_registry.commands().empty()) {
        register_builtin_commands(); // enumeration needs no terminal
    }
    for (auto const& command : _registry.commands()) {
        std::println("{:<24} {}", command.name, command.title);
    }
}

auto App::open_startup_panes(std::vector<std::string> const& files) -> void {
    // files from the command line take the Welcome pane's place
    if (files.empty()) {
        _session.open_welcome_pane();
    } else {
        for (auto const& file : files) {
            open_file(file);
        }
    }
    uint32_t const first { _session.focused_id() };

    // the event log: an append-only buffer in a floating grid pane,
    // bottom-right, on top of the split tree
    _log_buffer = _session.create_buffer(
        "events", core::BufferCapability::append_only, "events"
    );
    _log_pane = _session.open_float(core::PaneType::grid, _log_buffer, log_rect());
    _session.pane(_log_pane)->set_read_only(true);
    _session.focus(first);
}

auto App::register_builtin_commands() -> void {
    // session
    _registry.add({ "palette.open", "Open the command palette", [this] {
        open_palette();
    } });
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
    _registry.add({ "pane.grow_horizontal", "Grow pane horizontally", [this] {
        _session.resize_focused(2, 0);
    } });
    _registry.add({ "pane.shrink_horizontal", "Shrink pane horizontally", [this] {
        _session.resize_focused(-2, 0);
    } });
    _registry.add({ "pane.grow_vertical", "Grow pane vertically", [this] {
        _session.resize_focused(0, 1);
    } });
    _registry.add({ "pane.shrink_vertical", "Shrink pane vertically", [this] {
        _session.resize_focused(0, -1);
    } });

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
    auto const bind = [this](std::string_view name, std::string_view command) {
        if (auto const id { config::parse_key(std::string(name)) }) {
            _bindings[*id] = [this, cmd = std::string(command)](core::Event const&) {
                run_bound_command(cmd);
            };
            _command_keys[std::string(command)] = std::string(name); // palette hint
        }
    };

    // defaults first, then Spice-written keybinds, then the user's - each
    // layer overriding the one below (config.toml always wins)
    for (auto const& [key, command] : default_bindings) {
        bind(key, command);
    }
    for (auto const& keybind : _config.state_keybinds) {
        bind(keybind.key, keybind.command);
    }
    for (auto const& keybind : _config.user_keybinds) {
        bind(keybind.key, keybind.command);
    }

    // the Master key always reaches Spice
    _bindings[_master_id] = [this](core::Event const&) { open_palette(); };
    _command_keys["palette.open"] = _config.master;

    auto const click = [this](core::Event const& event) { on_click(event); };
    _bindings["press left"] = click;
    _bindings["C-press left"] = click;
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

auto App::run_bound_command(std::string const& name) -> void {
    // in a PTY pane, copy/paste keys keep their terminal meaning
    if (auto* pane { _session.focused_pane() };
        pane != nullptr && pane->type() == core::PaneType::pty) {
        if (name == "edit.copy") {
            _session.write_to_pty(_session.focused_id(), "\x03");
            return;
        }
        if (name == "edit.paste") {
            _session.write_to_pty(_session.focused_id(), "\x16");
            return;
        }
    }
    run_command(name);
}

auto App::log_note(std::string const& note) -> void {
    _log_buffer->append("\n" + note);
    if (auto* pane { _session.pane(_log_pane) }) {
        if (auto const area { _session.pane_area(_log_pane) }) {
            pane->scroll_to_bottom(*area);
            _damage.push_back(*area);
        }
    }
}

auto App::finish_bind(core::Event const& event) -> void {
    std::string const command { std::move(_pending_bind) };
    _pending_bind.clear();
    _full_repaint = true;

    std::string const id { core::event_id(event) };
    if (id == "escape") {
        log_note("bind cancelled");
        return;
    }
    auto const name { config::key_name(id) };
    if (!name) {
        log_note("cannot bind that key");
        return;
    }
    if (id == _master_id) {
        log_note(std::format("refused: {} is the Master key", *name));
        return;
    }
    for (auto const& keybind : _config.user_keybinds) { // config.toml wins
        if (config::parse_key(keybind.key) == id) {
            log_note(std::format("refused: {} is bound in config.toml", *name));
            return;
        }
    }

    // record it in the Spice-owned file and make it live right away
    std::erase_if(_config.state_keybinds, [&](config::Keybind const& keybind) {
        return config::parse_key(keybind.key) == id;
    });
    _config.state_keybinds.push_back({ *name, command });
    if (!config::save_keybinds(config::keybinds_file_path(), _config.state_keybinds)) {
        log_note("could not write keybinds.toml");
    }
    _bindings[id] = [this, command](core::Event const&) { run_bound_command(command); };
    log_note(std::format("bound {} to {}", *name, command));
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
            items.push_back({ path, path, {} });
        }
        return items;
    }
    for (auto const& entry : core::complete_path(query)) {
        std::string const path { entry.directory ? entry.path + "/" : entry.path };
        items.push_back({ path, path, {} });
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
    std::string const id { core::event_id(event) };

    // modal: the palette takes every key; Master closes it again
    if (id == _master_id) {
        _palette.close();
        _prompt_action = nullptr;
        _picker_action = nullptr;
        _damage.push_back(core::Palette::area(_screen));
        return;
    }
    if (event.type != core::EventType::key) {
        return;
    }

    // the (configurable) bind key: the next keypress binds the selection
    if (id == _palette_bind_id && !_palette.is_input() && !_palette.is_picker()) {
        if (auto const name { _palette.selected_name() }; !name.empty()) {
            _palette.close();
            _pending_bind = name;
            log_note(std::format("press a key to bind '{}' (escape cancels)", name));
            _damage.push_back(core::Palette::area(_screen));
        }
        return;
    }

    // the (configurable) run key acts like RETURN
    core::KeyEvent key { event.key };
    if (id == _palette_run_id) {
        key = { core::Key::enter, {}, {} };
    }

    switch (_palette.handle(key)) {
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
            if (core::Pane::close_button(*area).contains(point)) {
                // the x button closes the pane
                _registry.run("pane.close");
                _full_repaint = true;
            } else if (core::Pane::float_button(*area).contains(point)) {
                // the F button floats a tiled pane (and docks a floating one)
                if (_session.is_floating(*id)) {
                    _session.dock_focused();
                } else {
                    _session.float_focused();
                }
                _full_repaint = true;
            } else if (core::Pane::resize_corner(*area).contains(point)) {
                // the bottom-right corner: drag to resize
                _drag = Drag { true, Drag::Kind::resize, *id, 0, 0 };
            } else if (!core::Pane::content_area(*area).contains(point)) {
                // grabbed by the bar or a border: start moving the pane
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
        if (_drag.kind == Drag::Kind::resize) {
            // the pane's bottom-right corner follows the pointer
            if (auto const area { _session.pane_area(_drag.pane) }) {
                int const width_delta {
                    static_cast<int>(mouse.position.column)
                    - static_cast<int>(area->position.column + area->width - 1)
                };
                int const height_delta {
                    static_cast<int>(mouse.position.line)
                    - static_cast<int>(area->position.line + area->height - 1)
                };
                if ((width_delta != 0 || height_delta != 0)
                    && _session.resize_focused(width_delta, height_delta)) {
                    _full_repaint = true; // dividers move the neighbours too
                }
            }
        } else if (_drag.kind == Drag::Kind::text) {
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

auto App::open_palette() -> void {
    std::vector<core::Palette::Item> items;
    items.reserve(_registry.commands().size());
    for (auto const& command : _registry.commands()) {
        auto const key { _command_keys.find(command.name) };
        items.push_back({
            command.name,
            command.title,
            key != _command_keys.end() ? key->second : std::string(),
        });
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
    // every event lands in the log buffer, pane or no pane - except mouse
    // motion, which floods by the dozen during drags and is pure noise
    bool const motion {
        event.type == core::EventType::mouse
        && event.mouse.action == core::MouseAction::move
    };
    if (!motion) {
        log_note(core::describe(event));
    }

    if (event.type == core::EventType::resize) {
        on_resize();
    } else if (!_pending_bind.empty() && event.type == core::EventType::key) {
        finish_bind(event); // the next key belongs to the palette's bind flow
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
    // wipe only what will be re-rendered: panes overwrite their own cells,
    // the clear matters for cells no pane covers any more (a moved float)
    auto const clear = [this](Rectangle rect) {
        core::Color const text { _theme.color(Theme::Usage::text) };
        core::Color const background { _theme.color(Theme::Usage::background) };
        uint32_t const end_line {
            std::min(rect.position.line + rect.height, _grid.height())
        };
        uint32_t const end_column {
            std::min(rect.position.column + rect.width, _grid.width())
        };
        for (uint32_t line { rect.position.line }; line < end_line; ++line) {
            for (uint32_t column { rect.position.column }; column < end_column; ++column) {
                Position const cell { line, column, 0 };
                _grid.set_text(cell, " ");
                _grid.set_style(cell, text);
                _grid.set_background(cell, background);
            }
        }
    };
    if (rects.empty()) {
        clear({ { 0, 0, 0 }, _grid.width(), _grid.height() });
    } else {
        for (Rectangle const& rect : rects) {
            clear(rect);
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

    repaint({}); // repaint again once plugins have had a moment (harmless)

    while (_running) {
        auto const event { reader.poll(100) };
        _damage.clear();
        _full_repaint = false;

        pump_ptys();  // shells produce output with or without user input
        _host->poll(); // and so do plugins (their callbacks queue damage)

        if (event) {
            handle(*event);
            // drain the rest of the burst before painting: a fast mouse
            // drag delivers dozens of events per read, one frame covers all
            while (_running) {
                auto const more { reader.poll(0) };
                if (!more) {
                    break;
                }
                handle(*more);
            }
        }

        // let plugins observe a focus change (informational, per PROTOCOL.md)
        if (_session.focused_id() != _last_focused) {
            _last_focused = _session.focused_id();
            emit("spice.pane.focused", plugin::msgpack::Value::object({
                { "pane", plugin::msgpack::Value {
                    static_cast<uint64_t>(_last_focused) } },
            }));
        }

        if (_damage.size() > 8) {
            _full_repaint = true; // one frame beats re-rendering many rects
        }
        if (_running && (_full_repaint || !_damage.empty() || event)) {
            repaint(_full_repaint ? std::vector<Rectangle> {} : _damage);
        }
    }

    // graceful plugin shutdown, in PROTOCOL.md's order: the shutdown event,
    // a grace period during which the core keeps processing (a plugin
    // flushing work on the way out must complete), then SIGTERM, a further
    // grace, and finally the destructor's SIGKILL for whatever remains
    auto pump_plugins_for = [this](std::chrono::milliseconds window) {
        auto const deadline { std::chrono::steady_clock::now() + window };
        while (_host->plugin_count() > 0 && std::chrono::steady_clock::now() < deadline) {
            _host->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    _host->shutdown(static_cast<int>(_config.shutdown_grace.count()));
    pump_plugins_for(_config.shutdown_grace);
    if (_host->plugin_count() > 0) {
        _host->stop_all();
        pump_plugins_for(_config.sigterm_grace);
    }

    std::print("\x1b[?1049l");
    std::fflush(stdout);
}

}

int main(int argc, char** argv) {
    auto const options { parse_options(argc, argv) };
    if (options.error) {
        std::println(stderr, "spice: {}", *options.error);
        std::print(stderr, "{}", usage_text);
        return 2;
    }
    if (options.help) {
        std::print("{}", usage_text);
        return 0;
    }
    if (options.version) {
        std::println("spice {}", SPICE_VERSION);
        return 0;
    }

    App app { options };
    if (options.list_commands) {
        app.print_commands();
        return 0;
    }
    if (!app.ready()) {
        std::println("spice: no terminal to draw on");
        return 1;
    }
    app.run();
    return 0;
}

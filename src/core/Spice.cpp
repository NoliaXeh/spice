#include "spice/core/Spice.hpp"

#include <algorithm>
#include <utility>

namespace {

constexpr std::string_view welcome_text {
    "Welcome to Spice!\n"
    "\n"
    "The buffer manager made to match any taste.\n"
    "\n"
    "  ctrl-space / ctrl-p  open the command palette\n"
    "  drag a pane border  move it (drop on a tile to swap)\n"
    "\n"
    "  shift-arrows / mouse drag  select\n"
    "  tab / shift-tab  indent / dedent (the selection too)\n"
    "  ctrl-c / ctrl-v  copy / paste\n"
    "  ctrl-o / ctrl-s  open / save a file\n"
    "  ctrl-z / ctrl-y  undo / redo\n"
    "  ctrl-n  open a new pane\n"
    "  ctrl-x  close the current pane\n"
    "  ctrl-arrows  move focus\n"
    "  ctrl-f  float the current pane\n"
    "  ctrl-d  dock it back\n"
    "  ctrl-w  close Spice\n"
};

}

namespace spice::core {

Spice::Spice(std::string&& name)
    : _name { std::move(name) }
{}

auto Spice::name() const -> std::string const& {
    return _name;
}

auto Spice::set_screen(Rectangle screen) -> void {
    _screen = screen;
}

auto Spice::create_buffer(
    std::string&& name, BufferCapability capability, std::string_view content
) -> std::shared_ptr<Buffer> {
    return _buffers.emplace_back(
        std::make_shared<Buffer>(std::move(name), capability, content)
    );
}

auto Spice::buffers() const -> std::vector<std::shared_ptr<Buffer>> const& {
    return _buffers;
}

auto Spice::kill_buffer(std::shared_ptr<Buffer> const& buffer) -> bool {
    if (buffer == nullptr) {
        return false;
    }
    for (auto const& [id, pane] : _panes) {
        if (pane.buffer() == buffer) {
            return false; // still on screen: the pane keeps it alive
        }
    }
    return std::erase(_buffers, buffer) > 0;
}

auto Spice::set_pane_buffer(uint32_t id, std::shared_ptr<Buffer> buffer) -> bool {
    auto const target { pane(id) };
    if (!target || buffer == nullptr
        || std::ranges::find(_buffers, buffer) == _buffers.end()) {
        return false;
    }
    target->set_buffer(std::move(buffer));
    return true;
}

auto Spice::split_is_horizontal(Rectangle rect) const -> bool {
    // terminal cells are about twice as tall as wide, so a tile is only
    // "wide" once its width passes twice its height
    return rect.width >= rect.height * 2;
}

auto Spice::insertion_target() const -> uint32_t {
    if (_focused != 0 && _layout.contains(_focused) && !_layout.is_floating(_focused)) {
        return _focused;
    }
    uint32_t best { 0 };
    uint64_t best_area { 0 };
    for (auto const& [id, rect] : _layout.tiles(_screen)) {
        uint64_t const area { static_cast<uint64_t>(rect.width) * rect.height };
        if (area >= best_area) {
            best = id;
            best_area = area;
        }
    }
    return best;
}

auto Spice::open_pane(PaneType type, std::shared_ptr<Buffer> buffer) -> uint32_t {
    bool horizontal { true };
    if (auto const rect { pane_area(insertion_target()) }) {
        horizontal = split_is_horizontal(*rect);
    }
    return open_pane(type, std::move(buffer), horizontal);
}

auto Spice::open_pane(PaneType type, std::shared_ptr<Buffer> buffer, bool horizontal)
    -> uint32_t {
    uint32_t const id { _next_pane_id++ };
    _layout.insert(id, insertion_target(), horizontal);
    _panes.emplace(id, Pane { type, std::move(buffer) });
    _focused = id;
    return id;
}

auto Spice::open_float(PaneType type, std::shared_ptr<Buffer> buffer, Rectangle rect)
    -> uint32_t {
    uint32_t const id { _next_pane_id++ };
    _layout.float_pane(id, rect);
    _panes.emplace(id, Pane { type, std::move(buffer) });
    _focused = id;
    return id;
}

auto Spice::open_welcome_pane() -> uint32_t {
    auto buffer { create_buffer("Welcome", BufferCapability::editable, welcome_text) };
    uint32_t const id { open_pane(PaneType::edit, std::move(buffer)) };
    pane(id)->set_read_only(true); // a greeting, not a scratchpad
    return id;
}

auto Spice::open_pty_pane(std::vector<std::string> const& argv) -> uint32_t {
    if (argv.empty()) {
        return 0;
    }
    auto buffer { create_buffer(std::string(argv[0]), BufferCapability::append_only) };
    uint32_t const id { open_pane(PaneType::pty, std::move(buffer)) };

    auto const content { Pane::content_area(pane_area(id).value_or(_screen)) };
    uint32_t const columns { std::max(content.width, 1u) };
    uint32_t const rows { std::max(content.height, 1u) };
    PtyEntry entry {
        Pty {},
        Terminal { columns, rows, colors::light_gray, colors::dark_gray },
    };
    if (!entry.pty.spawn(argv, columns, rows)) {
        close_focused_pane(); // could not start: no pane to show
        return 0;
    }
    auto const [slot, inserted] { _ptys.emplace(id, std::move(entry)) };
    pane(id)->set_terminal(slot->second.terminal);
    return id;
}

auto Spice::pump_ptys() -> std::vector<uint32_t> {
    std::vector<uint32_t> changed;
    std::vector<uint32_t> dead;
    for (auto& [id, entry] : _ptys) {
        auto p { pane(id) };
        std::string const raw { entry.pty.read_output() };
        if (!raw.empty() && p) {
            entry.terminal.feed(raw);
            if (auto const answers { entry.terminal.take_responses() }; !answers.empty()) {
                entry.pty.write_input(answers); // the child asked; reply
            }
            for (auto const& line : entry.terminal.take_scrollback()) {
                p->buffer()->append("\n" + line); // history keeps growing
            }
            changed.push_back(id);
        }
        if (!entry.pty.running()) {
            if (p) {
                // the live screen goes away: preserve it in the scrollback
                for (auto const& line : entry.terminal.screen_text()) {
                    p->buffer()->append("\n" + line);
                }
                p->buffer()->append("\n[exited]");
                p->set_terminal({});
                if (std::ranges::find(changed, id) == changed.end()) {
                    changed.push_back(id);
                }
            }
            dead.push_back(id);
        }
    }
    for (uint32_t const id : dead) {
        _ptys.erase(id);
    }
    return changed;
}

auto Spice::write_to_pty(uint32_t id, std::string_view bytes) -> bool {
    auto const found { _ptys.find(id) };
    return found != _ptys.end() && found->second.pty.write_input(bytes);
}

auto Spice::resize_ptys() -> void {
    for (auto& [id, entry] : _ptys) {
        if (auto const area { pane_area(id) }) {
            auto const content { Pane::content_area(*area) };
            uint32_t const columns { std::max(content.width, 1u) };
            uint32_t const rows { std::max(content.height, 1u) };
            entry.terminal.resize(columns, rows);
            entry.pty.resize(columns, rows);
        }
    }
    resize_surfaces(); // plugin surfaces track their pane's size too
}

auto Spice::attach_surface(uint32_t id) -> OptRef<Grid> {
    if (auto existing { surface(id) }) {
        return existing;
    }
    auto const target { pane(id) };
    if (!target || target->type() != PaneType::grid) {
        return {};
    }
    auto const content { Pane::content_area(pane_area(id).value_or(_screen)) };
    auto const [slot, inserted] { _surfaces.emplace(
        id, Grid { std::max(content.width, 1u), std::max(content.height, 1u) }
    ) };
    target->set_surface(slot->second);
    return slot->second;
}

auto Spice::surface(uint32_t id) -> OptRef<Grid> {
    auto const found { _surfaces.find(id) };
    if (found == _surfaces.end()) {
        return {};
    }
    return found->second;
}

auto Spice::clear_surface(uint32_t id) -> void {
    if (auto const target { pane(id) }) {
        target->set_surface({});
    }
    _surfaces.erase(id);
}

auto Spice::set_surface_cursor(uint32_t id, Position position, bool visible) -> void {
    auto const target { pane(id) };
    if (target && _surfaces.contains(id)) {
        target->set_surface_cursor(
            visible ? std::optional { position } : std::nullopt
        );
    }
}

auto Spice::resize_surfaces() -> void {
    for (auto& [id, grid] : _surfaces) {
        auto const content { Pane::content_area(pane_area(id).value_or(_screen)) };
        uint32_t const width { std::max(content.width, 1u) };
        uint32_t const height { std::max(content.height, 1u) };
        if (grid.width() == width && grid.height() == height) {
            continue;
        }
        grid = Grid { width, height }; // blank: the plugin redraws
        if (auto const target { pane(id) }) {
            target->set_surface(grid); // same node, fresh contents
        }
    }
}

auto Spice::close_focused_pane() -> void {
    if (_focused == 0) {
        return;
    }
    if (auto p { pane(_focused) }) {
        p->set_terminal({}); // the entry (and its screen) goes away
        p->set_surface({});
    }
    _ptys.erase(_focused); // kills the child; the scrollback buffer stays
    _surfaces.erase(_focused);
    _layout.remove(_focused);
    _panes.erase(_focused); // the buffer stays in _buffers

    _focused = 0;
    auto const floats { _layout.floats() };
    if (!floats.empty()) {
        _focused = floats.back().first; // topmost float
    } else if (auto const tiles { _layout.tiles(_screen) }; !tiles.empty()) {
        _focused = tiles.front().first;
    }
}

auto Spice::pane_count() const -> size_t {
    return _panes.size();
}

auto Spice::pane_ids() const -> std::vector<uint32_t> {
    std::vector<uint32_t> ids;
    ids.reserve(_panes.size());
    for (auto const& [id, pane] : _panes) {
        ids.push_back(id);
    }
    return ids;
}

auto Spice::pane(uint32_t id) -> OptRef<Pane> {
    auto const found { _panes.find(id) };
    if (found == _panes.end()) {
        return {};
    }
    return found->second;
}

auto Spice::focused_id() const -> uint32_t {
    return _focused;
}

auto Spice::focused_pane() -> OptRef<Pane> {
    return pane(_focused);
}

auto Spice::focus(uint32_t id) -> void {
    if (_panes.contains(id)) {
        _focused = id;
        _layout.raise_float(id); // a focused float goes above all other floats
    }
}

auto Spice::move_focus(Direction direction) -> void {
    if (auto const next { _layout.neighbor(_screen, _focused, direction) }) {
        _focused = *next;
    }
}

auto Spice::float_focused() -> void {
    if (_focused == 0) {
        return;
    }
    Rectangle const rect { // centered, half the screen each way
        {
            _screen.position.line + _screen.height / 4,
            _screen.position.column + _screen.width / 4,
            0,
        },
        _screen.width / 2,
        _screen.height / 2,
    };
    _layout.float_pane(_focused, rect);
}

auto Spice::dock_focused() -> void {
    if (_focused == 0 || !_layout.is_floating(_focused)) {
        return;
    }
    uint32_t const at { insertion_target() };
    bool horizontal { true };
    if (auto const rect { pane_area(at) }) {
        horizontal = split_is_horizontal(*rect);
    }
    _layout.dock_pane(_focused, at, horizontal);
}

auto Spice::move_float(uint32_t id, Rectangle rect) -> bool {
    return _layout.move_float(id, rect);
}

auto Spice::is_floating(uint32_t id) const -> bool {
    return _layout.is_floating(id);
}

auto Spice::resize_focused(int width_delta, int height_delta) -> bool {
    if (_focused == 0) {
        return false;
    }
    bool const changed { _layout.resize_pane(_focused, width_delta, height_delta, _screen) };
    if (changed) {
        resize_ptys(); // pane content sizes moved with the dividers
    }
    return changed;
}

auto Spice::swap_panes(uint32_t a, uint32_t b) -> bool {
    if (!_panes.contains(a) || !_panes.contains(b)) {
        return false;
    }
    return _layout.swap(a, b);
}

auto Spice::pane_at(Position point) const -> std::optional<uint32_t> {
    return _layout.pane_at(_screen, point);
}

auto Spice::pane_area(uint32_t id) const -> std::optional<Rectangle> {
    for (auto const& [pane, rect] : _layout.floats()) {
        if (pane == id) {
            return rect;
        }
    }
    for (auto const& [pane, rect] : _layout.tiles(_screen)) {
        if (pane == id) {
            return rect;
        }
    }
    return std::nullopt;
}

auto Spice::draw(Grid& grid, Theme const& theme) -> void {
    for (auto const& [id, rect] : _layout.tiles(_screen)) {
        if (auto p { pane(id) }) {
            p->draw(grid, rect, id == _focused, theme);
        }
    }
    for (auto const& [id, rect] : _layout.floats()) { // bottom to top
        if (auto p { pane(id) }) {
            p->draw(grid, rect, id == _focused, theme);
            drop_shadow(grid, rect); // lift the float off what is beneath
        }
    }
}

}

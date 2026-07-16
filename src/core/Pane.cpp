#include "spice/core/Pane.hpp"
#include "spice/core/Terminal.hpp"
#include "spice/core/Utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace {

using namespace spice::core;

constexpr std::string_view corner_bl { "╰" }; //!< rounded bottom-left
constexpr std::string_view corner_br { "╯" }; //!< rounded bottom-right
constexpr std::string_view edge_h { "─" };
constexpr std::string_view edge_v { "│" };

//! Buttons are 3 cells wide (" F ", " x "); panes narrower than this
//! show a bare bar instead.
constexpr uint32_t min_width_for_buttons { 12 };

auto paint_cell(
    Grid& grid, Position cell, std::string_view glyph, Color style, Color background
) -> void {
    grid.set_text(cell, glyph);
    grid.set_style(cell, style);
    grid.set_background(cell, background);
}

}

namespace spice::core {

Pane::Pane(PaneType type, std::shared_ptr<Buffer> buffer)
    : _type { type }
    , _buffer { std::move(buffer) }
{}

auto Pane::type() const -> PaneType {
    return _type;
}

auto Pane::buffer() const -> std::shared_ptr<Buffer> const& {
    return _buffer;
}

auto Pane::cursor() const -> Position {
    return _cursor;
}

auto Pane::set_cursor(Position position) -> void {
    _cursor = clamp(position);
}

auto Pane::read_only() const -> bool {
    return _read_only;
}

auto Pane::set_read_only(bool read_only) -> void {
    _read_only = read_only;
}

auto Pane::set_terminal(OptRef<Terminal const> terminal) -> void {
    _terminal = terminal;
}

auto Pane::set_anchor(Position position) -> void {
    _anchor = clamp(position);
}

auto Pane::clear_anchor() -> void {
    _anchor.reset();
}

auto Pane::has_anchor() const -> bool {
    return _anchor.has_value();
}

auto Pane::selection() const -> std::optional<std::pair<Position, Position>> {
    if (!_anchor) {
        return std::nullopt;
    }
    Position const anchor { clamp(*_anchor) }; // the buffer may have changed
    Position const cursor { clamp(_cursor) };
    if (anchor == cursor) {
        return std::nullopt;
    }
    if (document_order(anchor, cursor)) {
        return std::pair { anchor, cursor };
    }
    return std::pair { cursor, anchor };
}

auto Pane::scroll() const -> Position {
    return _scroll;
}

auto Pane::scroll_to_bottom(Rectangle area) -> void {
    auto const content { content_area(area) };
    uint32_t const lines { _buffer->line_count() };
    _scroll.line = lines > content.height ? lines - content.height : 0;
    _scroll.column = 0;
}

auto Pane::content_area(Rectangle area) -> Rectangle {
    if (area.width <= 2 || area.height <= 2) {
        return { area.position, 0, 0 };
    }
    return {
        { area.position.line + 1, area.position.column + 1, area.position.layer },
        area.width - 2,
        area.height - 2,
    };
}

auto Pane::float_button(Rectangle area) -> Rectangle {
    if (area.width < min_width_for_buttons) {
        return { area.position, 0, 0 };
    }
    // " F " sits left of " x ", one cell apart: ... F  x ┃
    return { { area.position.line, area.position.column + area.width - 8, 0 }, 3, 1 };
}

auto Pane::close_button(Rectangle area) -> Rectangle {
    if (area.width < min_width_for_buttons) {
        return { area.position, 0, 0 };
    }
    return { { area.position.line, area.position.column + area.width - 4, 0 }, 3, 1 };
}

auto Pane::resize_corner(Rectangle area) -> Rectangle {
    if (area.width < 2 || area.height < 2) {
        return { area.position, 0, 0 };
    }
    return {
        { area.position.line + area.height - 1, area.position.column + area.width - 2, 0 },
        2,
        1,
    };
}

auto Pane::clamp(Position position) const -> Position {
    uint32_t const last_line { _buffer->line_count() - 1 };
    uint32_t const line { position.line < last_line ? position.line : last_line };
    uint32_t const length { _buffer->line_length(line) };
    uint32_t const column { position.column < length ? position.column : length };
    return { line, column, 0 };
}

auto Pane::scroll_to_cursor(Rectangle content) -> void {
    if (content.width == 0 || content.height == 0) {
        return;
    }
    if (_cursor.line < _scroll.line) {
        _scroll.line = _cursor.line;
    } else if (_cursor.line >= _scroll.line + content.height) {
        _scroll.line = _cursor.line - content.height + 1;
    }
    if (_cursor.column < _scroll.column) {
        _scroll.column = _cursor.column;
    } else if (_cursor.column >= _scroll.column + content.width) {
        _scroll.column = _cursor.column - content.width + 1;
    }
}

auto Pane::draw(Grid& grid, Rectangle area, bool focused, Theme const& theme) -> void {
    if (area.width == 0 || area.height == 0) {
        return;
    }
    // the buffer may have shrunk under us (undo, edits from another pane
    // sharing it): pull the cursor back inside before using it to scroll
    _cursor = clamp(_cursor);

    draw_title_bar(grid, area, focused, theme);
    draw_border(grid, area, focused, theme);

    // content; edit panes scroll to keep the cursor in view, other pane
    // types keep the scroll they were given (scrollback, plugin-driven)
    auto const content { content_area(area) };
    if (_type == PaneType::edit) {
        scroll_to_cursor(content);
    }

    if (_terminal) { // a live terminal paints its own screen
        draw_terminal_content(grid, content, theme);
        return;
    }
    draw_buffer_content(grid, content, focused, theme);
    draw_scrollbar(grid, area, focused, theme);
}

//! The title bar: dark on light across the whole top row - a pane-type
//! dot, the buffer's name (bold when focused, with * for unsaved edits
//! and [ro] for read-only), and the " F " (float) and " x " (close)
//! buttons on the right, in red.
auto Pane::draw_title_bar(Grid& grid, Rectangle area, bool focused, Theme const& theme)
    -> void {
    Color const bar_text { theme.color(Theme::Usage::titlebar_text) };
    Color const bar_background { theme.color(
        focused ? Theme::Usage::titlebar_background_focused : Theme::Usage::titlebar_background
    ) };
    Color const button_text { theme.color(Theme::Usage::titlebar_button_text) };
    Color const button_background { theme.color(Theme::Usage::titlebar_button_background) };

    uint32_t const top { area.position.line };
    uint32_t const left { area.position.column };
    uint32_t const right { area.position.column + area.width - 1 };

    std::string title { _buffer->name() };
    if (_buffer->capability() == BufferCapability::editable && _buffer->dirty()) {
        title += " *";
    }
    if (_read_only) {
        title += " [ro]";
    }
    Color title_text { bar_text };
    title_text.style.bold = focused;
    Color const dot {
        _type == PaneType::edit ? colors::soft_green
        : _type == PaneType::pty ? colors::amber
        : colors::steel_blue
    };
    Rectangle const float_at { float_button(area) };
    Rectangle const close_at { close_button(area) };
    uint32_t const title_start { left + 3 }; // after " • "
    uint32_t const title_end { // don't run into the buttons
        float_at.width > 0 ? float_at.position.column : right + 1
    };
    for (uint32_t column { left }; column <= right; ++column) {
        Position const cell { top, column, 0 };
        if (float_at.contains(cell)) {
            uint32_t const index { column - float_at.position.column };
            paint_cell(grid, cell, index == 1 ? "F" : " ", button_text, button_background);
        } else if (close_at.contains(cell)) {
            uint32_t const index { column - close_at.position.column };
            paint_cell(grid, cell, index == 1 ? "x" : " ", button_text, button_background);
        } else if (column == left + 1) {
            paint_cell(grid, cell, "•", dot, bar_background);
        } else if (column >= title_start && column - title_start < title.size()
                   && column < title_end) {
            paint_cell(
                grid, cell, std::string_view(title).substr(column - title_start, 1),
                title_text, bar_background
            );
        } else {
            paint_cell(grid, cell, " ", bar_text, bar_background);
        }
    }
}

//! Side borders and the rounded bottom.
auto Pane::draw_border(Grid& grid, Rectangle area, bool focused, Theme const& theme) -> void {
    Color const border {
        theme.color(focused ? Theme::Usage::border_focused : Theme::Usage::border)
    };
    Color const background { theme.color(Theme::Usage::background) };

    uint32_t const top { area.position.line };
    uint32_t const bottom { area.position.line + area.height - 1 };
    uint32_t const left { area.position.column };
    uint32_t const right { area.position.column + area.width - 1 };

    for (uint32_t line { top + 1 }; line < bottom; ++line) {
        paint_cell(grid, { line, left, 0 }, edge_v, border, background);
        paint_cell(grid, { line, right, 0 }, edge_v, border, background);
    }
    for (uint32_t column { left + 1 }; column < right; ++column) {
        paint_cell(grid, { bottom, column, 0 }, edge_h, border, background);
    }
    paint_cell(grid, { bottom, left, 0 }, corner_bl, border, background);
    paint_cell(grid, { bottom, right, 0 }, corner_br, border, background);
}

auto Pane::draw_terminal_content(Grid& grid, Rectangle content, Theme const& theme) -> void {
    Color const text { theme.color(Theme::Usage::text) };
    Color const background { theme.color(Theme::Usage::background) };

    for (uint32_t row { 0 }; row < content.height; ++row) {
        for (uint32_t column { 0 }; column < content.width; ++column) {
            auto const& terminal_cell { _terminal->cell(row, column) };
            bool const in_screen {
                row < _terminal->height() && column < _terminal->width()
            };
            paint_cell(
                grid,
                { content.position.line + row, content.position.column + column, 0 },
                in_screen ? std::string_view(terminal_cell.glyph) : " ",
                in_screen ? terminal_cell.foreground : text,
                in_screen ? terminal_cell.background : background
            );
        }
    }
}

auto Pane::draw_buffer_content(Grid& grid, Rectangle content, bool focused, Theme const& theme)
    -> void {
    Color const text { theme.color(Theme::Usage::text) };
    Color const background { theme.color(Theme::Usage::background) };
    Color const selection_text { theme.color(Theme::Usage::selection_text) };
    Color const selection_background { theme.color(Theme::Usage::selection_background) };
    Color const cursor_line { theme.color(Theme::Usage::cursor_line) };

    auto const selected_range { selection() };
    auto const is_selected = [&](Position in_buffer) -> bool {
        return selected_range
            && !document_order(in_buffer, selected_range->first)
            && document_order(in_buffer, selected_range->second);
    };

    for (uint32_t row { 0 }; row < content.height; ++row) {
        std::string_view const line { _buffer->line(_scroll.line + row) };
        size_t offset { utf8_offset(line, _scroll.column) };

        // the cursor's line gets a subtly lifted background when focused
        bool const at_cursor {
            focused && _type == PaneType::edit && _scroll.line + row == _cursor.line
        };
        Color const row_background { at_cursor ? cursor_line : background };

        for (uint32_t column { 0 }; column < content.width; ++column) {
            Position const cell {
                content.position.line + row,
                content.position.column + column,
                0,
            };
            std::string_view glyph { " " };
            if (offset < line.size()) {
                glyph = line.substr(offset, utf8_length(line[offset]));
                offset += glyph.size();
            }
            Position const in_buffer {
                _scroll.line + row, _scroll.column + column, 0
            };
            if (is_selected(in_buffer)) {
                paint_cell(grid, cell, glyph, selection_text, selection_background);
            } else {
                paint_cell(grid, cell, glyph, text, row_background);
            }
        }
    }
}

auto Pane::draw_scrollbar(Grid& grid, Rectangle area, bool focused, Theme const& theme)
    -> void {
    auto const content { content_area(area) };
    uint32_t const lines { _buffer->line_count() };
    if (content.height == 0 || lines <= content.height) {
        return;
    }
    Color const border {
        theme.color(focused ? Theme::Usage::border_focused : Theme::Usage::border)
    };
    Color const background { theme.color(Theme::Usage::background) };

    uint32_t const track { content.height };
    uint32_t const thumb { std::max(1u, track * content.height / lines) };
    uint32_t const max_scroll { lines - content.height };
    uint32_t const offset {
        max_scroll > 0 ? _scroll.line * (track - thumb) / max_scroll : 0
    };
    uint32_t const top { area.position.line };
    uint32_t const right { area.position.column + area.width - 1 };
    for (uint32_t i { 0 }; i < thumb; ++i) {
        paint_cell(grid, { top + 1 + offset + i, right, 0 }, "█", border, background);
    }
}

auto Pane::position_from_screen(Rectangle area, Position screen) const -> Position {
    auto const content { content_area(area) };
    uint32_t const line {
        screen.line > content.position.line ? screen.line - content.position.line : 0
    };
    uint32_t const column {
        screen.column > content.position.column ? screen.column - content.position.column : 0
    };
    return clamp({ _scroll.line + line, _scroll.column + column, 0 });
}

auto Pane::cursor_screen_position(Rectangle area) const -> Position {
    auto const content { content_area(area) };
    if (_terminal && content.width > 0 && content.height > 0) {
        // the emulator knows where its cursor is
        Position const cursor { _terminal->cursor() };
        return {
            content.position.line + std::min(cursor.line, content.height - 1),
            content.position.column + std::min(cursor.column, content.width - 1),
            0,
        };
    }
    uint32_t const line { _cursor.line - _scroll.line };
    uint32_t const column { _cursor.column - _scroll.column };
    return {
        content.position.line + (line < content.height ? line : 0),
        content.position.column + (column < content.width ? column : 0),
        0,
    };
}

}

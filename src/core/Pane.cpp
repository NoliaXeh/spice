#include "spice/core/Pane.hpp"
#include "spice/core/Terminal.hpp"
#include "spice/core/Utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace spice::core;

constexpr std::string_view corner_bl { "╰" }; //!< rounded bottom-left
constexpr std::string_view corner_br { "╯" }; //!< rounded bottom-right
constexpr std::string_view edge_h { "─" };
constexpr std::string_view edge_v { "│" };

//! Buttons are 3 cells wide (" F ", " x "); panes narrower than this
//! show a bare bar instead.
constexpr uint32_t min_width_for_buttons { 12 };

//! Text keeps at least this many columns before a gutter is worth having.
constexpr uint32_t min_text_columns { 8 };

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

auto Pane::set_buffer(std::shared_ptr<Buffer> buffer) -> void {
    if (buffer == nullptr) {
        return;
    }
    _buffer = std::move(buffer);
    _cursor = { 0, 0, 0 };
    _scroll = { 0, 0, 0 };
    _anchor.reset();
}

auto Pane::cursor() const -> Position {
    return _cursor;
}

auto Pane::set_cursor(Position position) -> void {
    _cursor = clamp(position);
    _free_scroll = false; // moving the cursor re-engages view following
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

auto Pane::set_surface(OptRef<Grid const> surface) -> void {
    _surface = surface;
    if (!surface) {
        _surface_cursor.reset();
    }
}

auto Pane::set_surface_cursor(std::optional<Position> cursor) -> void {
    _surface_cursor = cursor;
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

auto Pane::scroll_by(int lines) -> void {
    auto const last { static_cast<int>(_buffer->line_count()) - 1 };
    auto const target { std::clamp(static_cast<int>(_scroll.line) + lines, 0, last) };
    _scroll.line = static_cast<uint32_t>(target);
    _free_scroll = true;
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

auto Pane::gutter_width(Rectangle content) const -> uint32_t {
    if (_type != PaneType::edit) {
        return 0;
    }
    uint32_t digits { 1 };
    for (uint32_t lines { _buffer->line_count() }; lines >= 10; lines /= 10) {
        ++digits;
    }
    uint32_t const width { digits + 1 }; // the numbers and a breathing space
    if (content.width < width + min_text_columns) {
        return 0; // a tiny pane keeps every column for its text
    }
    return width;
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
    if (_type == PaneType::edit && !_free_scroll) {
        Rectangle text_area { content }; // what remains beside the gutter
        uint32_t const gutter { gutter_width(content) };
        text_area.position.column += gutter;
        text_area.width -= gutter;
        scroll_to_cursor(text_area);
    }

    if (_terminal) { // a live terminal paints its own screen
        draw_terminal_content(grid, content, theme);
        return;
    }
    if (_surface) { // a plugin paints this pane
        draw_surface_content(grid, content, theme);
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
    draw_cursor_indicator(grid, area, focused, theme);
}

//! line:column, right-aligned on the bar, left of the buttons - skipped
//! when the title already needs that room.
auto Pane::draw_cursor_indicator(Grid& grid, Rectangle area, bool focused, Theme const& theme)
    -> void {
    if (_type != PaneType::edit) {
        return;
    }
    std::string const text {
        std::to_string(_cursor.line + 1) + ":" + std::to_string(_cursor.column + 1)
    };
    Rectangle const float_at { float_button(area) };
    uint32_t const end { // the first column the indicator must not touch
        float_at.width > 0 ? float_at.position.column
                           : area.position.column + area.width - 1
    };
    uint32_t const title_end { // where the title's text stops
        area.position.column + 3 + static_cast<uint32_t>(_buffer->name().size()) + 6
    };
    if (end < area.position.column + area.width
        && end >= title_end + text.size() + 2) {
        uint32_t const start { end - static_cast<uint32_t>(text.size()) - 1 };
        Color const bar_text { theme.color(Theme::Usage::titlebar_text) };
        Color const bar_background { theme.color(
            focused ? Theme::Usage::titlebar_background_focused
                    : Theme::Usage::titlebar_background
        ) };
        for (uint32_t i { 0 }; i < text.size(); ++i) {
            paint_cell(
                grid, { area.position.line, start + i, 0 },
                std::string_view(text).substr(i, 1), bar_text, bar_background
            );
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

auto Pane::draw_surface_content(Grid& grid, Rectangle content, Theme const& theme) -> void {
    Color const text { theme.color(Theme::Usage::text) };
    Color const background { theme.color(Theme::Usage::background) };

    for (uint32_t row { 0 }; row < content.height; ++row) {
        for (uint32_t column { 0 }; column < content.width; ++column) {
            Position const source { row, column, 0 };
            bool const in_surface {
                row < _surface->height() && column < _surface->width()
            };
            std::string_view const glyph { in_surface ? _surface->char_at(source) : " " };
            paint_cell(
                grid,
                { content.position.line + row, content.position.column + column, 0 },
                glyph.empty() ? " " : glyph,
                in_surface ? _surface->style_at(source) : text,
                in_surface ? _surface->background_at(source) : background
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
    Color const gutter_text { theme.color(Theme::Usage::border) };

    uint32_t const gutter { gutter_width(content) };
    uint32_t const text_width { content.width - gutter };

    auto const selected_range { selection() };
    auto const is_selected = [&](Position in_buffer) -> bool {
        return selected_range
            && !document_order(in_buffer, selected_range->first)
            && document_order(in_buffer, selected_range->second);
    };

    for (uint32_t row { 0 }; row < content.height; ++row) {
        uint32_t const buffer_line { _scroll.line + row };
        std::string_view const line { _buffer->line(buffer_line) };
        size_t offset { utf8_offset(line, _scroll.column) };

        // the cursor's line gets a subtly lifted background when focused
        bool const at_cursor {
            focused && _type == PaneType::edit && buffer_line == _cursor.line
        };
        Color const row_background { at_cursor ? cursor_line : background };

        // only this line's highlight spans, gathered bottom layer first,
        // so the cell loop stays cheap
        std::vector<std::reference_wrapper<Buffer::Highlight const>> row_spans;
        for (auto const& [layer, spans] : _buffer->highlights()) {
            for (Buffer::Highlight const& span : spans) {
                if (span.start_line <= buffer_line && buffer_line <= span.end_line) {
                    row_spans.emplace_back(span);
                }
            }
        }
        auto const highlighted = [&](size_t byte) -> std::optional<Color> {
            std::optional<Color> top; // the last hit is the topmost layer's
            for (Buffer::Highlight const& span : row_spans) {
                bool const after_start {
                    buffer_line > span.start_line || byte >= span.start_byte
                };
                bool const before_end {
                    buffer_line < span.end_line || byte < span.end_byte
                };
                if (after_start && before_end) {
                    top = Color {
                        static_cast<uint8_t>(span.rgb >> 16U),
                        static_cast<uint8_t>(span.rgb >> 8U),
                        static_cast<uint8_t>(span.rgb),
                        text.style,
                    };
                }
            }
            return top;
        };

        // the gutter: this row's line number, right-aligned, dimmed -
        // except the cursor's, which reads in the regular text color
        if (gutter > 0) {
            std::string const number {
                buffer_line < _buffer->line_count() ? std::to_string(buffer_line + 1)
                                                    : std::string()
            };
            uint32_t const digits { gutter - 1 };
            Color const number_color { at_cursor ? text : gutter_text };
            for (uint32_t column { 0 }; column < gutter; ++column) {
                uint32_t const align { digits - static_cast<uint32_t>(number.size()) };
                std::string_view const glyph {
                    column >= align && column < digits
                        ? std::string_view(number).substr(column - align, 1)
                        : std::string_view(" ")
                };
                paint_cell(
                    grid,
                    { content.position.line + row, content.position.column + column, 0 },
                    glyph, number_color, row_background
                );
            }
        }

        for (uint32_t column { 0 }; column < text_width; ++column) {
            Position const cell {
                content.position.line + row,
                content.position.column + gutter + column,
                0,
            };
            std::string_view glyph { " " };
            size_t const glyph_byte { offset };
            bool const in_line { offset < line.size() };
            if (in_line) {
                glyph = line.substr(offset, utf8_length(line[offset]));
                offset += glyph.size();
            }
            Position const in_buffer { buffer_line, _scroll.column + column, 0 };
            if (is_selected(in_buffer)) { // selection beats decoration
                paint_cell(grid, cell, glyph, selection_text, selection_background);
            } else {
                Color style { text };
                if (in_line && !row_spans.empty()) {
                    if (auto const color { highlighted(glyph_byte) }) {
                        style = *color;
                    }
                }
                paint_cell(grid, cell, glyph, style, row_background);
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
    uint32_t const text_start { content.position.column + gutter_width(content) };
    uint32_t const line {
        screen.line > content.position.line ? screen.line - content.position.line : 0
    };
    uint32_t const column {
        screen.column > text_start ? screen.column - text_start : 0
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
    if (_surface && _surface_cursor && content.width > 0 && content.height > 0) {
        return { // wherever the plugin parked it, clamped into the view
            content.position.line + std::min(_surface_cursor->line, content.height - 1),
            content.position.column + std::min(_surface_cursor->column, content.width - 1),
            0,
        };
    }
    uint32_t const gutter { gutter_width(content) };
    uint32_t const line { _cursor.line - _scroll.line };
    uint32_t const column { _cursor.column - _scroll.column };
    return {
        content.position.line + (line < content.height ? line : 0),
        content.position.column + gutter
            + (column < content.width - gutter ? column : 0),
        0,
    };
}

}

#include "spice/core/Editor.hpp"

namespace {

using namespace spice::core;

//! The line's leading spaces, capped at `limit`.
auto leading_spaces(Buffer const& buffer, uint32_t line, uint32_t limit) -> uint32_t {
    std::string_view const text { buffer.line(line) };
    uint32_t count { 0 };
    while (count < text.size() && count < limit && text[count] == ' ') {
        ++count;
    }
    return count;
}

//! TAB and SHIFT-TAB: indentation. A bare TAB inserts spaces at the
//! cursor; with a selection it indents every line the selection touches.
//! SHIFT-TAB removes up to one indent of leading spaces from those lines.
auto apply_tab_key(Pane& pane, Buffer& buffer, bool dedent, uint32_t indent_width) -> bool {
    Position const cursor { pane.cursor() };
    auto const range { pane.selection() };
    if (!dedent && !range) {
        if (!buffer.insert(cursor, std::string(indent_width, ' '))) {
            return false;
        }
        pane.set_cursor({ cursor.line, cursor.column + indent_width, 0 });
        return true;
    }

    uint32_t const first { range ? range->first.line : cursor.line };
    uint32_t last { range ? range->second.line : cursor.line };
    if (range && last > first && range->second.column == 0) {
        --last; // the selection ends before that line's first character
    }
    bool changed { false };
    uint32_t cursor_shift { 0 };
    for (uint32_t line { first }; line <= last; ++line) {
        if (dedent) {
            uint32_t const remove { leading_spaces(buffer, line, indent_width) };
            if (remove > 0 && buffer.erase_range({ line, 0, 0 }, { line, remove, 0 })) {
                changed = true;
                if (line == cursor.line) {
                    cursor_shift = remove;
                }
            }
        } else {
            changed |= buffer.insert({ line, 0, 0 }, std::string(indent_width, ' '));
        }
    }
    if (changed && dedent) { // the cursor stays on its character
        pane.set_cursor({
            cursor.line,
            cursor.column > cursor_shift ? cursor.column - cursor_shift : 0,
            0,
        });
    }
    return changed;
}

//! True for keys that move the cursor (and so extend or drop a selection).
auto is_movement(Key key) -> bool {
    switch (key) {
        case Key::up:
        case Key::down:
        case Key::left:
        case Key::right:
        case Key::home:
        case Key::end:
        case Key::page_up:
        case Key::page_down:
            return true;
        default:
            return false;
    }
}

//! Moves the cursor for a movement key (set_cursor clamps into the buffer).
auto apply_movement_key(
    Pane& pane, Buffer const& buffer, Key key, uint32_t page_rows
) -> void {
    Position const cursor { pane.cursor() };
    switch (key) {
    case Key::left:
        if (cursor.column > 0) {
            pane.set_cursor({ cursor.line, cursor.column - 1, 0 });
        } else if (cursor.line > 0) {
            pane.set_cursor({ cursor.line - 1, buffer.line_length(cursor.line - 1), 0 });
        }
        break;

    case Key::right:
        if (cursor.column < buffer.line_length(cursor.line)) {
            pane.set_cursor({ cursor.line, cursor.column + 1, 0 });
        } else if (cursor.line + 1 < buffer.line_count()) {
            pane.set_cursor({ cursor.line + 1, 0, 0 });
        }
        break;

    case Key::up:
        if (cursor.line > 0) {
            pane.set_cursor({ cursor.line - 1, cursor.column, 0 });
        }
        break;

    case Key::down:
        pane.set_cursor({ cursor.line + 1, cursor.column, 0 });
        break;

    case Key::home:
        pane.set_cursor({ cursor.line, 0, 0 });
        break;

    case Key::end:
        pane.set_cursor({ cursor.line, buffer.line_length(cursor.line), 0 });
        break;

    case Key::page_up:
        pane.set_cursor({
            cursor.line > page_rows ? cursor.line - page_rows : 0, cursor.column, 0
        });
        break;

    case Key::page_down:
        pane.set_cursor({ cursor.line + page_rows, cursor.column, 0 });
        break;

    default:
        break;
    }
}

//! Removes the selected range (one undo step); cursor lands at its start.
auto erase_selection(Pane& pane, Buffer& buffer) -> bool {
    auto const range { pane.selection() };
    if (!range || !buffer.erase_range(range->first, range->second)) {
        return false;
    }
    pane.set_cursor(range->first);
    pane.clear_anchor();
    return true;
}

//! Erases the character before the cursor; at a line start it joins with
//! the previous line.
auto apply_backspace(Pane& pane, Buffer& buffer) -> bool {
    Position const cursor { pane.cursor() };
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
}

//! Applies a buffer-mutating key (typing, enter, backspace, delete);
//! false when nothing changed or `key` is not one of them.
auto apply_mutation_key(
    Pane& pane, Buffer& buffer, KeyEvent const& key, std::string& clipboard,
    uint32_t indent_width
) -> bool {
    switch (key.key) {
    case Key::character: {
        erase_selection(pane, buffer); // typing replaces the selection
        Position const at { pane.cursor() };
        if (buffer.insert(at, key.text)) {
            pane.set_cursor({ at.line, at.column + 1, 0 });
            return true;
        }
        return false;
    }

    case Key::enter: {
        erase_selection(pane, buffer);
        Position const at { pane.cursor() };
        if (!buffer.split_line(at)) {
            return false;
        }
        // auto-indent: the new line opens where the old one's text began
        uint32_t const indent {
            leading_spaces(buffer, at.line, buffer.line_length(at.line))
        };
        uint32_t applied { 0 };
        if (indent > 0
            && buffer.insert({ at.line + 1, 0, 0 }, std::string(indent, ' '))) {
            applied = indent;
        }
        pane.set_cursor({ at.line + 1, applied, 0 });
        return true;
    }

    case Key::tab:
        return apply_tab_key(pane, buffer, key.mods.shift, indent_width);

    case Key::backspace:
        return erase_selection(pane, buffer) || apply_backspace(pane, buffer);

    case Key::del:
        if (key.mods.shift) { // shift-delete: cut
            if (auto const range { pane.selection() }) {
                clipboard = buffer.text_between(range->first, range->second);
            }
        }
        return erase_selection(pane, buffer) || buffer.erase(pane.cursor());

    default:
        return false;
    }
}

}

namespace spice::core {

auto apply_editing_key(
    Pane& pane, KeyEvent const& key, uint32_t page_rows, std::string& clipboard,
    uint32_t indent_width
) -> bool {
    auto& buffer { *pane.buffer() };

    // a read-only pane still navigates and selects, but never mutates
    // (not even shift-delete's cut)
    if (pane.read_only()) {
        switch (key.key) {
        case Key::character:
        case Key::enter:
        case Key::tab:
        case Key::backspace:
        case Key::del:
            return false;
        default:
            break;
        }
    }

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
        apply_movement_key(pane, buffer, key.key, page_rows);
        return true;
    }

    if (key.key == Key::escape) {
        pane.clear_anchor();
        return true;
    }
    return apply_mutation_key(pane, buffer, key, clipboard, indent_width);
}

}

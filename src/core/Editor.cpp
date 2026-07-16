#include "spice/core/Editor.hpp"

namespace {

using namespace spice::core;

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
    Pane& pane, Buffer& buffer, KeyEvent const& key, std::string& clipboard
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
        if (buffer.split_line(at)) {
            pane.set_cursor({ at.line + 1, 0, 0 });
            return true;
        }
        return false;
    }

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
    Pane& pane, KeyEvent const& key, uint32_t page_rows, std::string& clipboard
) -> bool {
    auto& buffer { *pane.buffer() };

    // a read-only pane still navigates and selects, but never mutates
    // (not even shift-delete's cut)
    if (pane.read_only()) {
        switch (key.key) {
        case Key::character:
        case Key::enter:
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
    return apply_mutation_key(pane, buffer, key, clipboard);
}

}

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
    case Key::character: {
        erase_selection(); // typing replaces the selection
        Position const at { pane.cursor() };
        if (buffer.insert(at, key.text)) {
            pane.set_cursor({ at.line, at.column + 1, 0 });
            return true;
        }
        return false;
    }

    case Key::enter: {
        erase_selection();
        Position const at { pane.cursor() };
        if (buffer.split_line(at)) {
            pane.set_cursor({ at.line + 1, 0, 0 });
            return true;
        }
        return false;
    }

    case Key::backspace:
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

    case Key::del:
        if (key.mods.shift) { // shift-delete: cut
            if (auto const range { pane.selection() }) {
                clipboard = buffer.text_between(range->first, range->second);
            }
        }
        if (erase_selection()) {
            return true;
        }
        return buffer.erase(cursor);

    case Key::escape:
        pane.clear_anchor();
        return true;

    case Key::left:
        if (cursor.column > 0) {
            pane.set_cursor({ cursor.line, cursor.column - 1, 0 });
        } else if (cursor.line > 0) {
            pane.set_cursor({ cursor.line - 1, buffer.line_length(cursor.line - 1), 0 });
        }
        return true;

    case Key::right:
        if (cursor.column < buffer.line_length(cursor.line)) {
            pane.set_cursor({ cursor.line, cursor.column + 1, 0 });
        } else if (cursor.line + 1 < buffer.line_count()) {
            pane.set_cursor({ cursor.line + 1, 0, 0 });
        }
        return true;

    case Key::up:
        if (cursor.line > 0) {
            pane.set_cursor({ cursor.line - 1, cursor.column, 0 });
        }
        return true;

    case Key::down:
        pane.set_cursor({ cursor.line + 1, cursor.column, 0 }); // set_cursor clamps
        return true;

    case Key::home:
        pane.set_cursor({ cursor.line, 0, 0 });
        return true;

    case Key::end:
        pane.set_cursor({ cursor.line, buffer.line_length(cursor.line), 0 });
        return true;

    case Key::page_up:
        pane.set_cursor({
            cursor.line > page_rows ? cursor.line - page_rows : 0, cursor.column, 0
        });
        return true;

    case Key::page_down:
        pane.set_cursor({ cursor.line + page_rows, cursor.column, 0 }); // clamped
        return true;

    default:
        return false;
    }
}

}

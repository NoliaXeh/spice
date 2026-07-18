#include "doctest.h"

#include "spice/core/Editor.hpp"

#include <memory>

using namespace spice::core;

namespace {

struct Setup {
    std::shared_ptr<Buffer> buffer;
    Pane pane;
    std::string clipboard;

    explicit Setup(std::string_view content)
        : buffer { std::make_shared<Buffer>("b", BufferCapability::editable, content) }
        , pane { PaneType::edit, buffer }
    {}

    auto key(Key key, std::string text = {}, bool shift = false) -> bool {
        return apply_editing_key(
            pane, { key, { .shift = shift, .alt = false, .ctrl = false }, std::move(text) },
            4, clipboard
        );
    }
};

}

TEST_CASE("core::apply_editing_key types, splits and erases") {
    Setup s { "ab" };
    CHECK(s.key(Key::right));
    CHECK(s.key(Key::character, "X"));
    CHECK_EQ(s.buffer->line(0), "aXb");
    CHECK(s.key(Key::enter));
    CHECK_EQ(s.buffer->line(0), "aX");
    CHECK_EQ(s.buffer->line(1), "b");
    CHECK(s.key(Key::backspace)); // at column 0: joins back
    CHECK_EQ(s.buffer->line(0), "aXb");
}

TEST_CASE("core::apply_editing_key shift-movement selects, plain drops") {
    Setup s { "hello" };
    CHECK(s.key(Key::right, {}, true));
    CHECK(s.key(Key::right, {}, true));
    auto const range { s.pane.selection() };
    REQUIRE(range.has_value());
    CHECK_EQ(range->first, Position { 0, 0, 0 });
    CHECK_EQ(range->second, Position { 0, 2, 0 });

    CHECK(s.key(Key::left)); // plain movement drops the selection
    CHECK_FALSE(s.pane.selection().has_value());
}

TEST_CASE("core::apply_editing_key typing replaces the selection in one step") {
    Setup s { "hello" };
    s.key(Key::right, {}, true);
    s.key(Key::right, {}, true);
    s.key(Key::character, "H");
    CHECK_EQ(s.buffer->line(0), "Hllo");

    s.buffer->undo(); // the typed character
    s.buffer->undo(); // the selection removal, as a single edit
    CHECK_EQ(s.buffer->line(0), "hello");
}

TEST_CASE("core::apply_editing_key shift-delete cuts into the clipboard") {
    Setup s { "hello" };
    s.key(Key::right, {}, true);
    s.key(Key::right, {}, true);
    CHECK(s.key(Key::del, {}, true));
    CHECK_EQ(s.clipboard, "he");
    CHECK_EQ(s.buffer->line(0), "llo");
}

TEST_CASE("core::apply_editing_key escape clears the selection") {
    Setup s { "hello" };
    s.key(Key::right, {}, true);
    CHECK(s.pane.selection().has_value());
    CHECK(s.key(Key::escape));
    CHECK_FALSE(s.pane.selection().has_value());
    CHECK_EQ(s.buffer->line(0), "hello"); // nothing erased
}

TEST_CASE("core::apply_editing_key home, end and paging") {
    Setup s { "one\ntwo\nthree\nfour\nfive\nsix" };
    CHECK(s.key(Key::end));
    CHECK_EQ(s.pane.cursor(), Position { 0, 3, 0 });
    CHECK(s.key(Key::page_down)); // page is 4 in this harness
    CHECK_EQ(s.pane.cursor().line, 4u);
    CHECK(s.key(Key::home));
    CHECK_EQ(s.pane.cursor(), Position { 4, 0, 0 });
    CHECK(s.key(Key::page_up));
    CHECK_EQ(s.pane.cursor().line, 0u);
}

TEST_CASE("core::apply_editing_key on a read-only pane navigates but never mutates") {
    Setup s { "hello\nworld" };
    s.pane.set_read_only(true);

    CHECK_FALSE(s.key(Key::character, "X"));
    CHECK_FALSE(s.key(Key::enter));
    CHECK_FALSE(s.key(Key::backspace));
    CHECK_FALSE(s.key(Key::del));
    CHECK_EQ(s.buffer->line(0), "hello");
    CHECK_EQ(s.buffer->line_count(), 2u);

    CHECK(s.key(Key::right, {}, true)); // selecting still works
    CHECK(s.pane.selection().has_value());
    CHECK_FALSE(s.key(Key::del, {}, true)); // shift-delete: no cut either
    CHECK_EQ(s.clipboard, "");
    CHECK_EQ(s.buffer->line(0), "hello");

    CHECK(s.key(Key::down)); // plain movement still works
    CHECK_EQ(s.pane.cursor().line, 1u);
}

TEST_CASE("core::apply_editing_key rejects what the buffer rejects") {
    auto log { std::make_shared<Buffer>("l", BufferCapability::append_only, "ro") };
    Pane pane { PaneType::grid, log };
    std::string clipboard;
    CHECK_FALSE(apply_editing_key(
        pane, { Key::character, {}, "x" }, 1, clipboard
    ));
    CHECK_EQ(log->line(0), "ro");
}

TEST_CASE("core::apply_editing_key TAB indents, SHIFT-TAB dedents") {
    Setup s { "hello" };
    CHECK(s.key(Key::tab)); // at the cursor: four spaces in
    CHECK_EQ(s.buffer->line(0), "    hello");
    CHECK_EQ(s.pane.cursor().column, 4u);

    CHECK(s.key(Key::tab, {}, true)); // shift: the indent comes back off
    CHECK_EQ(s.buffer->line(0), "hello");
    CHECK_EQ(s.pane.cursor().column, 0u);

    CHECK_FALSE(s.key(Key::tab, {}, true)); // nothing left to dedent
}

TEST_CASE("core::apply_editing_key TAB indents every selected line") {
    Setup s { "one\ntwo\nthree" };
    s.pane.set_anchor({ 0, 1, 0 });
    s.pane.set_cursor({ 1, 2, 0 }); // selection touches lines 0 and 1

    CHECK(s.key(Key::tab));
    CHECK_EQ(s.buffer->line(0), "    one");
    CHECK_EQ(s.buffer->line(1), "    two");
    CHECK_EQ(s.buffer->line(2), "three"); // untouched

    CHECK(s.key(Key::tab, {}, true)); // and back
    CHECK_EQ(s.buffer->line(0), "one");
    CHECK_EQ(s.buffer->line(1), "two");
}

TEST_CASE("core::apply_editing_key ENTER inherits the line's indent") {
    Setup s { "    body" };
    s.pane.set_cursor({ 0, 8, 0 }); // end of line
    CHECK(s.key(Key::enter));
    CHECK_EQ(s.buffer->line(0), "    body");
    CHECK_EQ(s.buffer->line(1), "    ");
    CHECK_EQ(s.pane.cursor(), Position { 1, 4, 0 });

    // at column 0 there is nothing above to inherit
    s.pane.set_cursor({ 0, 0, 0 });
    CHECK(s.key(Key::enter));
    CHECK_EQ(s.buffer->line(0), "");
    CHECK_EQ(s.pane.cursor(), Position { 1, 0, 0 });
}

TEST_CASE("core::apply_editing_key TAB respects read-only panes") {
    Setup s { "hello" };
    s.pane.set_read_only(true);
    CHECK_FALSE(s.key(Key::tab));
    CHECK_EQ(s.buffer->line(0), "hello");
}

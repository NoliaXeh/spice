#include "doctest.h"

#include "spice/core/Buffer.hpp"

using namespace spice::core;

TEST_CASE("core::Buffer starts with one empty line") {
    Buffer buffer { "b", BufferCapability::editable };
    CHECK_EQ(buffer.line_count(), 1u);
    CHECK_EQ(buffer.line(0), "");
    CHECK_EQ(buffer.line_length(0), 0u);
}

TEST_CASE("core::Buffer parses initial content into lines") {
    Buffer buffer { "b", BufferCapability::editable, "one\ntwo\nthree" };
    CHECK_EQ(buffer.line_count(), 3u);
    CHECK_EQ(buffer.line(0), "one");
    CHECK_EQ(buffer.line(1), "two");
    CHECK_EQ(buffer.line(2), "three");
    CHECK(buffer.line(3).empty()); // out of bounds
}

TEST_CASE("core::Buffer::insert() inserts and shifts within a line") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    CHECK(buffer.insert({ 0, 1, 0 }, "X"));
    CHECK_EQ(buffer.line(0), "aXbc");
    CHECK(buffer.insert({ 0, 4, 0 }, "!")); // at end of line
    CHECK_EQ(buffer.line(0), "aXbc!");
}

TEST_CASE("core::Buffer::insert() rejects bad input") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    CHECK_FALSE(buffer.insert({ 5, 0, 0 }, "X"));  // line out of bounds
    CHECK_FALSE(buffer.insert({ 0, 9, 0 }, "X"));  // column past the end
    CHECK_FALSE(buffer.insert({ 0, 0, 0 }, "a\nb")); // newlines: use split_line
}

TEST_CASE("core::Buffer counts columns in utf-8 characters") {
    Buffer buffer { "b", BufferCapability::editable, "a\xc3\xa9z" }; // aéz
    CHECK_EQ(buffer.line_length(0), 3u);
    CHECK(buffer.insert({ 0, 2, 0 }, "X")); // between é and z
    CHECK_EQ(buffer.line(0), "a\xc3\xa9Xz");
    CHECK(buffer.erase({ 0, 1, 0 })); // erase é as one character
    CHECK_EQ(buffer.line(0), "aXz");
}

TEST_CASE("core::Buffer::erase() removes a character or joins lines") {
    Buffer buffer { "b", BufferCapability::editable, "ab\ncd" };
    CHECK(buffer.erase({ 0, 0, 0 }));
    CHECK_EQ(buffer.line(0), "b");
    CHECK(buffer.erase({ 0, 1, 0 })); // end of line: joins the next line
    CHECK_EQ(buffer.line_count(), 1u);
    CHECK_EQ(buffer.line(0), "bcd");
    CHECK_FALSE(buffer.erase({ 0, 3, 0 })); // very end of buffer
}

TEST_CASE("core::Buffer::split_line() splits at the column") {
    Buffer buffer { "b", BufferCapability::editable, "hello" };
    CHECK(buffer.split_line({ 0, 2, 0 }));
    CHECK_EQ(buffer.line_count(), 2u);
    CHECK_EQ(buffer.line(0), "he");
    CHECK_EQ(buffer.line(1), "llo");
}

TEST_CASE("core::Buffer append-only rejects edits but accepts appends") {
    Buffer buffer { "log", BufferCapability::append_only, "history" };
    CHECK_FALSE(buffer.insert({ 0, 0, 0 }, "X"));
    CHECK_FALSE(buffer.erase({ 0, 0, 0 }));
    CHECK_FALSE(buffer.split_line({ 0, 0, 0 }));
    CHECK_EQ(buffer.line(0), "history"); // untouched

    buffer.append(" grows\nmore");
    CHECK_EQ(buffer.line(0), "history grows");
    CHECK_EQ(buffer.line(1), "more");
}

TEST_CASE("core::Buffer::capability() reports the flag") {
    Buffer editable { "e", BufferCapability::editable };
    Buffer log { "l", BufferCapability::append_only };
    CHECK_EQ(editable.capability(), BufferCapability::editable);
    CHECK_EQ(log.capability(), BufferCapability::append_only);
}

TEST_CASE("core::Buffer tracks a path and dirtiness") {
    Buffer buffer { "b", BufferCapability::editable, "content" };
    CHECK_EQ(buffer.path(), "");
    CHECK_FALSE(buffer.dirty()); // initial content is not an edit

    buffer.set_path("/tmp/somewhere");
    CHECK_EQ(buffer.path(), "/tmp/somewhere");

    buffer.insert({ 0, 0, 0 }, "x");
    CHECK(buffer.dirty());
    buffer.mark_saved();
    CHECK_FALSE(buffer.dirty());

    buffer.undo(); // undoing is also a change relative to the saved state
    CHECK(buffer.dirty());
}

TEST_CASE("core::Buffer::undo() reverses an insert") {
    Buffer buffer { "b", BufferCapability::editable, "hello" };
    buffer.insert({ 0, 2, 0 }, "X");
    CHECK_EQ(buffer.line(0), "heXllo");

    auto const position { buffer.undo() };
    REQUIRE(position.has_value());
    CHECK_EQ(*position, Position { 0, 2, 0 });
    CHECK_EQ(buffer.line(0), "hello");
    CHECK_FALSE(buffer.undo().has_value()); // history exhausted
}

TEST_CASE("core::Buffer::undo() reverses an erase, restoring the text") {
    Buffer buffer { "b", BufferCapability::editable, "caf\xc3\xa9!" };
    buffer.erase({ 0, 3, 0 }); // the é, one character, two bytes
    CHECK_EQ(buffer.line(0), "caf!");

    auto const position { buffer.undo() };
    REQUIRE(position.has_value());
    CHECK_EQ(buffer.line(0), "caf\xc3\xa9!");
    CHECK_EQ(*position, Position { 0, 4, 0 }); // just past the restored text
}

TEST_CASE("core::Buffer::undo() reverses split and join") {
    Buffer buffer { "b", BufferCapability::editable, "hello" };
    buffer.split_line({ 0, 2, 0 });
    CHECK_EQ(buffer.line_count(), 2u);
    buffer.undo();
    CHECK_EQ(buffer.line_count(), 1u);
    CHECK_EQ(buffer.line(0), "hello");

    Buffer joined { "b", BufferCapability::editable, "ab\ncd" };
    joined.erase({ 0, 2, 0 }); // join at end of first line
    CHECK_EQ(joined.line_count(), 1u);
    joined.undo();
    CHECK_EQ(joined.line_count(), 2u);
    CHECK_EQ(joined.line(0), "ab");
    CHECK_EQ(joined.line(1), "cd");
}

TEST_CASE("core::Buffer a typing run undoes as one edit") {
    Buffer buffer { "b", BufferCapability::editable };
    buffer.insert({ 0, 0, 0 }, "a");
    buffer.insert({ 0, 1, 0 }, "b");
    buffer.insert({ 0, 2, 0 }, "c");
    CHECK_EQ(buffer.line(0), "abc");

    buffer.undo(); // coalesced: one undo removes the whole run
    CHECK_EQ(buffer.line(0), "");
    CHECK_FALSE(buffer.undo().has_value());
}

TEST_CASE("core::Buffer a backspace run undoes as one edit") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    buffer.erase({ 0, 2, 0 }); // backspacing: c, then b, then a
    buffer.erase({ 0, 1, 0 });
    buffer.erase({ 0, 0, 0 });
    CHECK_EQ(buffer.line(0), "");

    buffer.undo();
    CHECK_EQ(buffer.line(0), "abc");
    CHECK_FALSE(buffer.undo().has_value());
}

TEST_CASE("core::Buffer a delete-forward run undoes as one edit") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    buffer.erase({ 0, 0, 0 });
    buffer.erase({ 0, 0, 0 });
    buffer.erase({ 0, 0, 0 });
    CHECK_EQ(buffer.line(0), "");

    buffer.undo();
    CHECK_EQ(buffer.line(0), "abc");
    CHECK_FALSE(buffer.undo().has_value());
}

TEST_CASE("core::Buffer typing at different spots stays separate edits") {
    Buffer buffer { "b", BufferCapability::editable, "xy" };
    buffer.insert({ 0, 0, 0 }, "a"); // "axy"
    buffer.insert({ 0, 3, 0 }, "b"); // "axyb": not adjacent to the first
    buffer.undo();
    CHECK_EQ(buffer.line(0), "axy");
    buffer.undo();
    CHECK_EQ(buffer.line(0), "xy");
}

TEST_CASE("core::Buffer::redo() re-applies, and a new edit clears redo") {
    Buffer buffer { "b", BufferCapability::editable, "hello" };
    buffer.insert({ 0, 5, 0 }, "!");
    buffer.undo();
    CHECK_EQ(buffer.line(0), "hello");

    auto const position { buffer.redo() };
    REQUIRE(position.has_value());
    CHECK_EQ(buffer.line(0), "hello!");
    CHECK_EQ(*position, Position { 0, 6, 0 });

    buffer.undo();
    buffer.insert({ 0, 0, 0 }, "?"); // editing forks history
    CHECK_FALSE(buffer.redo().has_value());
}

TEST_CASE("core::Buffer appends are not undoable") {
    Buffer buffer { "log", BufferCapability::editable, "start" };
    buffer.append(" more");
    CHECK_FALSE(buffer.undo().has_value());
}

TEST_CASE("core::Buffer::text_between() extracts ranges") {
    Buffer buffer { "b", BufferCapability::editable, "hello\nworld\nlast" };
    CHECK_EQ(buffer.text_between({ 0, 1, 0 }, { 0, 4, 0 }), "ell");
    CHECK_EQ(buffer.text_between({ 0, 3, 0 }, { 2, 2, 0 }), "lo\nworld\nla");
    CHECK_EQ(buffer.text_between({ 2, 2, 0 }, { 0, 3, 0 }), "lo\nworld\nla"); // normalized
    CHECK_EQ(buffer.text_between({ 0, 2, 0 }, { 0, 2, 0 }), ""); // empty range
}

TEST_CASE("core::Buffer::erase_range() removes a multi-line range as one undo") {
    Buffer buffer { "b", BufferCapability::editable, "hello\nworld\nlast" };
    CHECK(buffer.erase_range({ 0, 3, 0 }, { 2, 2, 0 }));
    CHECK_EQ(buffer.line_count(), 1u);
    CHECK_EQ(buffer.line(0), "helst");

    buffer.undo(); // one step restores everything
    CHECK_EQ(buffer.line_count(), 3u);
    CHECK_EQ(buffer.line(0), "hello");
    CHECK_EQ(buffer.line(1), "world");
    CHECK_EQ(buffer.line(2), "last");
    CHECK_FALSE(buffer.undo().has_value());

    buffer.redo();
    CHECK_EQ(buffer.line(0), "helst");
}

TEST_CASE("core::Buffer::erase_range() rejects bad input") {
    Buffer buffer { "b", BufferCapability::editable, "ab" };
    CHECK_FALSE(buffer.erase_range({ 0, 1, 0 }, { 0, 1, 0 })); // empty
    CHECK_FALSE(buffer.erase_range({ 0, 0, 0 }, { 5, 0, 0 })); // out of bounds

    Buffer log { "l", BufferCapability::append_only, "ab" };
    CHECK_FALSE(log.erase_range({ 0, 0, 0 }, { 0, 1, 0 }));
}

TEST_CASE("core::Buffer::insert_block() pastes multi-line text as one undo") {
    Buffer buffer { "b", BufferCapability::editable, "AB" };
    auto const end { buffer.insert_block({ 0, 1, 0 }, "x\ny\nz") };
    REQUIRE(end.has_value());
    CHECK_EQ(*end, Position { 2, 1, 0 });
    CHECK_EQ(buffer.line(0), "Ax");
    CHECK_EQ(buffer.line(1), "y");
    CHECK_EQ(buffer.line(2), "zB");

    buffer.undo();
    CHECK_EQ(buffer.line_count(), 1u);
    CHECK_EQ(buffer.line(0), "AB");

    buffer.redo();
    CHECK_EQ(buffer.line(2), "zB");
}

TEST_CASE("core::Buffer::insert_block() single line behaves like insert") {
    Buffer buffer { "b", BufferCapability::editable, "AB" };
    auto const end { buffer.insert_block({ 0, 1, 0 }, "xyz") };
    REQUIRE(end.has_value());
    CHECK_EQ(*end, Position { 0, 4, 0 });
    CHECK_EQ(buffer.line(0), "AxyzB");
    buffer.undo();
    CHECK_EQ(buffer.line(0), "AB");
}

TEST_CASE("core::Buffer undo round-trips a mixed edit sequence") {
    Buffer buffer { "b", BufferCapability::editable, "hello world" };
    buffer.split_line({ 0, 5, 0 });   // "hello" / " world"
    buffer.insert({ 1, 0, 0 }, "-");  // "hello" / "- world"
    buffer.erase({ 0, 4, 0 });        // "hell" / "- world"

    buffer.undo();
    buffer.undo();
    buffer.undo();
    CHECK_EQ(buffer.line_count(), 1u);
    CHECK_EQ(buffer.line(0), "hello world");

    buffer.redo();
    buffer.redo();
    buffer.redo();
    CHECK_EQ(buffer.line_count(), 2u);
    CHECK_EQ(buffer.line(0), "hell");
    CHECK_EQ(buffer.line(1), "- world");
}

// ---------------------------------------------------------------
// The change journal (protocol splices - byte columns)
// ---------------------------------------------------------------

TEST_CASE("core::Buffer::take_changes() starts empty and drains") {
    Buffer buffer { "b", BufferCapability::editable, "one\ntwo" };
    auto const initial { buffer.take_changes() };
    CHECK(initial.complete);
    CHECK(initial.splices.empty());
    CHECK_EQ(initial.from_version, buffer.version()); // creation is the base

    buffer.insert({ 0, 0, 0 }, "X");
    auto const first { buffer.take_changes() };
    CHECK_EQ(first.splices.size(), 1u);
    CHECK(buffer.take_changes().splices.empty()); // drained
}

TEST_CASE("core::Buffer journal reports inserts with byte columns") {
    Buffer buffer { "b", BufferCapability::editable, "a\xc3\xa9z" }; // aéz
    buffer.take_changes();

    buffer.insert({ 0, 2, 0 }, "X"); // after é: char col 2, byte col 3
    auto const set { buffer.take_changes() };
    REQUIRE_EQ(set.splices.size(), 1u);
    CHECK_EQ(set.splices[0], Buffer::Change { 0, 3, 0, 3, "X" });
}

TEST_CASE("core::Buffer journal reports erases and line joins") {
    Buffer buffer { "b", BufferCapability::editable, "ab\ncd" };
    buffer.take_changes();

    buffer.erase({ 0, 0, 0 }); // 'a' goes
    buffer.erase({ 0, 1, 0 }); // end of "b": joins the lines
    auto const set { buffer.take_changes() };
    REQUIRE_EQ(set.splices.size(), 2u);
    CHECK_EQ(set.splices[0], Buffer::Change { 0, 0, 0, 1, "" });
    CHECK_EQ(set.splices[1], Buffer::Change { 0, 1, 1, 0, "" }); // the newline
}

TEST_CASE("core::Buffer journal reports splits, blocks and appends") {
    Buffer buffer { "b", BufferCapability::editable, "hello world" };
    buffer.take_changes();

    buffer.split_line({ 0, 5, 0 });
    auto set { buffer.take_changes() };
    REQUIRE_EQ(set.splices.size(), 1u);
    CHECK_EQ(set.splices[0], Buffer::Change { 0, 5, 0, 5, "\n" });

    buffer.erase_range({ 0, 1, 0 }, { 1, 2, 0 }); // "ello\n w" goes
    set = buffer.take_changes();
    REQUIRE_EQ(set.splices.size(), 1u);
    CHECK_EQ(set.splices[0], Buffer::Change { 0, 1, 1, 2, "" });

    buffer.insert_block({ 0, 1, 0 }, "X\nY");
    set = buffer.take_changes();
    REQUIRE_EQ(set.splices.size(), 1u);
    CHECK_EQ(set.splices[0], Buffer::Change { 0, 1, 0, 1, "X\nY" });

    buffer.append("\ntail");
    set = buffer.take_changes();
    REQUIRE_EQ(set.splices.size(), 1u);
    CHECK_EQ(set.splices[0].text, "\ntail");
}

TEST_CASE("core::Buffer journal reports undo as the inverse splice") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    buffer.take_changes();

    buffer.insert({ 0, 1, 0 }, "X"); // "aXbc"
    buffer.take_changes();

    buffer.undo(); // back to "abc": the X was erased
    auto const undone { buffer.take_changes() };
    REQUIRE_EQ(undone.splices.size(), 1u);
    CHECK_EQ(undone.splices[0], Buffer::Change { 0, 1, 0, 2, "" });

    buffer.redo(); // "aXbc" again: the X came back
    auto const redone { buffer.take_changes() };
    REQUIRE_EQ(redone.splices.size(), 1u);
    CHECK_EQ(redone.splices[0], Buffer::Change { 0, 1, 0, 1, "X" });
}

// ---------------------------------------------------------------
// Batches: many splices, one edit
// ---------------------------------------------------------------

TEST_CASE("core::Buffer::apply_batch() is one version bump and one undo step") {
    Buffer buffer { "b", BufferCapability::editable, "alpha\nbeta\ngamma" };
    buffer.take_changes();
    uint64_t const before { buffer.version() };

    // descending document order, both citing the original content
    CHECK(buffer.apply_batch({
        { { 2, 0, 0 }, { 2, 5, 0 }, "GAMMA" }, // replace "gamma"
        { { 0, 0, 0 }, { 0, 0, 0 }, ">> " },   // insert at the top
    }));
    CHECK_EQ(buffer.line(0), ">> alpha");
    CHECK_EQ(buffer.line(2), "GAMMA");
    CHECK_EQ(buffer.version(), before + 1); // one bump for the whole batch

    auto const set { buffer.take_changes() };
    CHECK_EQ(set.splices.size(), 3u); // erase + insert + insert

    buffer.undo(); // the whole batch reverts as one
    CHECK_EQ(buffer.line(0), "alpha");
    CHECK_EQ(buffer.line(2), "gamma");

    buffer.redo();
    CHECK_EQ(buffer.line(0), ">> alpha");
    CHECK_EQ(buffer.line(2), "GAMMA");
}

// ---------------------------------------------------------------
// Marks: positions that ride along with the text
// ---------------------------------------------------------------

TEST_CASE("core::Buffer marks shift with edits around them") {
    Buffer buffer { "b", BufferCapability::editable, "hello world" };
    uint64_t const mark { buffer.set_mark(0, 6, Buffer::MarkGravity::left) }; // before "world"

    buffer.insert({ 0, 0, 0 }, "X"); // before the mark: it shifts right
    CHECK_EQ(buffer.mark(mark), Buffer::MarkInfo { 0, 7, true });

    buffer.insert({ 0, 10, 0 }, "Y"); // after the mark: no effect
    CHECK_EQ(buffer.mark(mark), Buffer::MarkInfo { 0, 7, true });

    buffer.split_line({ 0, 2, 0 }); // a newline above: the mark changes line
    CHECK_EQ(buffer.mark(mark), Buffer::MarkInfo { 1, 5, true });

    CHECK(buffer.delete_mark(mark));
    CHECK_FALSE(buffer.mark(mark).has_value());
}

TEST_CASE("core::Buffer mark gravity decides on insertion at the mark") {
    Buffer buffer { "b", BufferCapability::editable, "ab" };
    uint64_t const stays { buffer.set_mark(0, 1, Buffer::MarkGravity::left) };
    uint64_t const rides { buffer.set_mark(0, 1, Buffer::MarkGravity::right) };

    buffer.insert({ 0, 1, 0 }, "XY"); // exactly at both marks
    CHECK_EQ(buffer.mark(stays), Buffer::MarkInfo { 0, 1, true });
    CHECK_EQ(buffer.mark(rides), Buffer::MarkInfo { 0, 3, true });
}

TEST_CASE("core::Buffer marks inside deleted text go invalid, loudly") {
    Buffer buffer { "b", BufferCapability::editable, "hello world" };
    uint64_t const inside { buffer.set_mark(0, 8, Buffer::MarkGravity::left) };
    uint64_t const after { buffer.set_mark(0, 11, Buffer::MarkGravity::left) };

    buffer.erase_range({ 0, 5, 0 }, { 0, 10, 0 }); // " worl" goes
    CHECK_EQ(buffer.mark(inside), Buffer::MarkInfo { 0, 5, false }); // parked, invalid
    CHECK_EQ(buffer.mark(after), Buffer::MarkInfo { 0, 6, true });   // remapped
}

TEST_CASE("core::Buffer marks survive undo, moving back with the text") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    uint64_t const mark { buffer.set_mark(0, 2, Buffer::MarkGravity::left) };

    buffer.insert({ 0, 0, 0 }, "12");
    CHECK_EQ(buffer.mark(mark), Buffer::MarkInfo { 0, 4, true });
    buffer.undo();
    CHECK_EQ(buffer.mark(mark), Buffer::MarkInfo { 0, 2, true });
}

TEST_CASE("core::Buffer journal versions bracket the changes") {
    Buffer buffer { "b", BufferCapability::editable, "abc" };
    buffer.take_changes();
    uint64_t const before { buffer.version() };

    buffer.insert({ 0, 0, 0 }, "1");
    buffer.insert({ 0, 1, 0 }, "2");
    auto const set { buffer.take_changes() };
    CHECK_EQ(set.from_version, before);
    CHECK_EQ(buffer.version(), before + 2);
    CHECK_EQ(set.splices.size(), 2u);
}

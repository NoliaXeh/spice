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

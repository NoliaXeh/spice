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

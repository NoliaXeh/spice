#include "doctest.h"

#include "spice/core/Terminal.hpp"

using namespace spice::core;

namespace {

auto make() -> Terminal {
    return Terminal { 10, 4, colors::light_gray, colors::dark_gray };
}

auto row(Terminal const& terminal, uint32_t line) -> std::string {
    std::string out;
    for (uint32_t column { 0 }; column < terminal.width(); ++column) {
        out += terminal.cell(line, column).glyph;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}

TEST_CASE("core::Terminal prints, wraps and scrolls") {
    auto terminal { make() };
    terminal.feed("hello");
    CHECK_EQ(row(terminal, 0), "hello");
    CHECK_EQ(terminal.cursor(), Position { 0, 5, 0 });

    terminal.feed("\r\nline2\r\nline3\r\nline4\r\nline5"); // scrolls once
    CHECK_EQ(row(terminal, 0), "line2");
    CHECK_EQ(row(terminal, 3), "line5");
    auto const scrolled { terminal.take_scrollback() };
    REQUIRE_EQ(scrolled.size(), 1u);
    CHECK_EQ(scrolled[0], "hello");
    CHECK(terminal.take_scrollback().empty()); // drained

    Terminal wide { 4, 2, colors::white, colors::black };
    wide.feed("abcdef"); // wraps after 4
    CHECK_EQ(wide.cell(0, 3).glyph, "d");
    CHECK_EQ(wide.cell(1, 0).glyph, "e");
}

TEST_CASE("core::Terminal carriage return overwrites - prompt redraws work") {
    auto terminal { make() };
    terminal.feed("abcdef\rXY");
    CHECK_EQ(row(terminal, 0), "XYcdef");
    CHECK_EQ(terminal.cursor(), Position { 0, 2, 0 });
}

TEST_CASE("core::Terminal backspace makes character removal visible") {
    auto terminal { make() };
    terminal.feed("abc");
    terminal.feed("\b \b"); // the classic erase echo
    CHECK_EQ(row(terminal, 0), "ab");
    CHECK_EQ(terminal.cursor(), Position { 0, 2, 0 });
}

TEST_CASE("core::Terminal SGR colors: 16, 256 and truecolor") {
    auto terminal { make() };
    terminal.feed("\x1b[31mR\x1b[0m");
    CHECK_EQ(terminal.cell(0, 0).foreground.r, 0xCD); // xterm red

    terminal.feed("\x1b[92mG");
    CHECK_EQ(terminal.cell(0, 1).foreground.g, 0xFF); // bright green

    terminal.feed("\x1b[38;5;196mX");
    CHECK_EQ(terminal.cell(0, 2).foreground.r, 0xFF); // cube red

    terminal.feed("\x1b[38;2;1;2;3mT\x1b[0m");
    CHECK_EQ(terminal.cell(0, 3).foreground.r, 1);
    CHECK_EQ(terminal.cell(0, 3).foreground.g, 2);
    CHECK_EQ(terminal.cell(0, 3).foreground.b, 3);

    terminal.feed("\x1b[44mB\x1b[49m");
    CHECK_EQ(terminal.cell(0, 4).background.b, 0xEE); // blue background

    terminal.feed("\x1b[1;33mS");
    bool const bold { terminal.cell(0, 5).foreground.style.bold };
    CHECK(bold);

    terminal.feed("N"); // still yellow until reset
    CHECK_EQ(terminal.cell(0, 6).foreground.r, 0xCD);
    terminal.feed("\x1b[0mD");
    CHECK_EQ(terminal.cell(0, 7).foreground, colors::light_gray);
}

TEST_CASE("core::Terminal cursor movement and erasing") {
    auto terminal { make() };
    terminal.feed("aaaaa\r\nbbbbb\r\nccccc");
    terminal.feed("\x1b[2;3H"); // row 2, column 3
    CHECK_EQ(terminal.cursor(), Position { 1, 2, 0 });
    terminal.feed("\x1b[K"); // erase to end of line
    CHECK_EQ(row(terminal, 1), "bb");
    terminal.feed("\x1b[A\x1b[2D"); // up, left twice
    CHECK_EQ(terminal.cursor(), Position { 0, 0, 0 });
    terminal.feed("\x1b[2J"); // clear screen
    CHECK_EQ(row(terminal, 0), "");
    CHECK_EQ(row(terminal, 2), "");
    terminal.feed("\x1b[3;4Hx");
    CHECK_EQ(terminal.cell(2, 3).glyph, "x");
}

TEST_CASE("core::Terminal insert and delete characters and lines") {
    auto terminal { make() };
    terminal.feed("abcdef\r");
    terminal.feed("\x1b[2P"); // delete 2 chars at the cursor
    CHECK_EQ(row(terminal, 0), "cdef");
    terminal.feed("\x1b[2@"); // insert 2 blanks
    CHECK_EQ(row(terminal, 0), "  cdef");

    terminal.feed("\x1b[2J\x1b[Hone\r\ntwo\r\nthree\x1b[1;1H");
    terminal.feed("\x1b[M"); // delete line 1: everything moves up
    CHECK_EQ(row(terminal, 0), "two");
    CHECK_EQ(row(terminal, 1), "three");
    terminal.feed("\x1b[L"); // insert a blank line back
    CHECK_EQ(row(terminal, 0), "");
    CHECK_EQ(row(terminal, 1), "two");
}

TEST_CASE("core::Terminal ignores modes, OSC titles and charset escapes") {
    auto terminal { make() };
    terminal.feed("\x1b[?25l\x1b[?2004h");        // cursor hide, bracketed paste
    terminal.feed("\x1b]0;a title\x07");           // OSC with BEL
    terminal.feed("\x1b]2;another\x1b\\");         // OSC with ST
    terminal.feed("\x1b(B");                       // charset selection
    terminal.feed("\x1bP+q696e646e\x1b\\");        // DCS query (XTGETTCAP)
    terminal.feed("ok");
    CHECK_EQ(row(terminal, 0), "ok");
}

TEST_CASE("core::Terminal answers device queries so shells don't stall") {
    auto terminal { make() };
    CHECK_EQ(terminal.take_responses(), "");

    terminal.feed("\x1b[c"); // primary DA
    CHECK_EQ(terminal.take_responses(), "\x1b[?62;22c");

    terminal.feed("\x1b[>c"); // secondary DA
    CHECK_EQ(terminal.take_responses(), "\x1b[>1;10;0c");

    terminal.feed("ab\x1b[6n"); // cursor position report
    CHECK_EQ(terminal.take_responses(), "\x1b[1;3R");
    CHECK_EQ(terminal.take_responses(), ""); // drained

    terminal.feed("\x1b[5n");
    CHECK_EQ(terminal.take_responses(), "\x1b[0n");
    CHECK_EQ(row(terminal, 0), "ab"); // queries leave the screen alone
}

TEST_CASE("core::Terminal utf-8 output occupies one cell per character") {
    auto terminal { make() };
    terminal.feed("caf\xc3\xa9!");
    CHECK_EQ(terminal.cell(0, 3).glyph, "\xc3\xa9");
    CHECK_EQ(terminal.cell(0, 4).glyph, "!");
}

TEST_CASE("core::Terminal resize keeps content and clamps the cursor") {
    auto terminal { make() };
    terminal.feed("hello\r\nworld");
    terminal.resize(6, 2);
    CHECK_EQ(row(terminal, 0), "hello");
    CHECK_EQ(row(terminal, 1), "world");
    CHECK_EQ(terminal.cursor(), Position { 1, 5, 0 });
    terminal.resize(20, 5);
    CHECK_EQ(row(terminal, 0), "hello");
}

TEST_CASE("core::Terminal::screen_text() dumps the visible screen") {
    auto terminal { make() };
    terminal.feed("one\r\n\r\nthree");
    auto const lines { terminal.screen_text() };
    REQUIRE_EQ(lines.size(), 3u);
    CHECK_EQ(lines[0], "one");
    CHECK_EQ(lines[1], "");
    CHECK_EQ(lines[2], "three");
}

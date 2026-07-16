#ifndef SPICE_CORE_TERMINAL_H
#define SPICE_CORE_TERMINAL_H

#include "spice/core/Color.hpp"
#include "spice/core/Position.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spice::core {

//! One cell of an emulated terminal screen.
struct TerminalCell {
    std::string glyph { " " };
    Color foreground;
    Color background;
};

//! A small VT/xterm screen emulator, the display side of a PTY pane: it
//! feeds on the child's byte stream and maintains a width x height grid of
//! styled cells plus a cursor - so prompt redraws, backspacing, colors and
//! full-screen programs render the way a terminal would show them.
//!
//! Interprets the common core: printable text (UTF-8), \r \n \b \t, CSI
//! cursor movement and erasing, insert/delete of lines and characters,
//! SGR colors (16, 256 and truecolor) and attributes, save/restore cursor,
//! index/reverse-index. Modes, OSC titles and other sequences are ignored
//! gracefully. Lines scrolled off the top come out of take_scrollback()
//! so callers can keep history.
class Terminal {
public:
    Terminal(uint32_t width, uint32_t height, Color foreground, Color background);

    auto width() const -> uint32_t;
    auto height() const -> uint32_t;
    auto cursor() const -> Position;
    //! The cell at (line, column); a blank default cell when out of bounds.
    auto cell(uint32_t line, uint32_t column) const -> TerminalCell const&;

    //! Interprets the next chunk of the child's output.
    auto feed(std::string_view bytes) -> void;

    //! Resizes the screen, keeping the overlapping content and the cursor.
    auto resize(uint32_t width, uint32_t height) -> void;

    //! The lines scrolled off the top since the last call (plain text,
    //! oldest first, trailing blanks trimmed) - the caller's history.
    auto take_scrollback() -> std::vector<std::string>;

    //! Bytes the child expects back for its queries (device attributes,
    //! cursor position reports...). Write them to the pty after feeding,
    //! or programs like fish stall waiting for their terminal to answer.
    auto take_responses() -> std::string;

    //! The whole screen as plain text lines, trailing blanks trimmed and
    //! trailing empty lines dropped (dumped to history when a shell exits).
    auto screen_text() const -> std::vector<std::string>;

private:
    enum class State : uint8_t { ground, escape, csi, osc, dcs };

    auto at(uint32_t line, uint32_t column) -> TerminalCell&;
    auto put(std::string glyph) -> void;
    auto newline() -> void;
    auto scroll_up() -> void;
    auto erase_cells(uint32_t line, uint32_t from, uint32_t to) -> void;
    auto handle_control(char byte) -> void;
    auto handle_escape(char byte) -> void;
    auto handle_csi(char final) -> void;
    // handle_csi's parts, by sequence family
    auto csi_move_cursor(char final, std::vector<int> const& params) -> void;
    auto csi_erase(char final, std::vector<int> const& params) -> void;
    auto csi_edit_cells(char final, std::vector<int> const& params) -> void;
    auto apply_sgr() -> void;
    auto line_text(uint32_t line) const -> std::string;
    auto parameters() const -> std::vector<int>;

    uint32_t _width;
    uint32_t _height;
    std::vector<TerminalCell> _cells; //!< flat, row-major
    Position _cursor { 0, 0, 0 };
    Position _saved_cursor { 0, 0, 0 };

    Color _default_foreground;
    Color _default_background;
    Color _foreground; //!< current SGR state
    Color _background;

    State _state { State::ground };
    std::string _sequence; //!< CSI parameter bytes being collected
    std::string _pending_utf8;
    std::vector<std::string> _scrollback;
    std::string _responses;
};

}

#endif // SPICE_CORE_TERMINAL_H

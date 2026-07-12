#ifndef SPICE_CORE_BUFFER_H
#define SPICE_CORE_BUFFER_H

#include "spice/core/Position.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spice::core {

//! What operations a buffer accepts. Plugins and panes must respect this;
//! the core rejects writes a buffer doesn't support (see README).
enum class BufferCapability : uint8_t {
    editable,    //!< arbitrary insert/delete anywhere (EditPane)
    append_only, //!< content grows at the end; history is read-only (PTY scrollback)
};

//! Content, decoupled from any view. The core owns a buffer's bytes; the
//! same buffer can be shown in several panes, or in none (a background
//! buffer). Closing a pane never destroys its buffer.
//!
//! Positions address (line, column-in-characters); layer is ignored.
//! A buffer always holds at least one (possibly empty) line.
class Buffer {
public:
    Buffer(std::string&& name, BufferCapability capability, std::string_view content = {});

    auto name() const -> std::string const&;
    auto capability() const -> BufferCapability;

    auto line_count() const -> uint32_t;
    //! Returns empty if out of bounds.
    auto line(uint32_t index) const -> std::string_view;
    //! Length of a line in UTF-8 characters (0 if out of bounds).
    auto line_length(uint32_t index) const -> uint32_t;

    //! Inserts newline-free text before `position` (column may equal the
    //! line length to append). Editable buffers only.
    auto insert(Position position, std::string_view text) -> bool;

    //! Erases the character at `position`; at the end of a line it joins the
    //! next line instead. Editable buffers only.
    auto erase(Position position) -> bool;

    //! Splits the line at `position`, moving what follows onto a new line.
    //! Editable buffers only.
    auto split_line(Position position) -> bool;

    //! Appends text at the end of the buffer; '\n' starts a new line.
    //! Supported by every capability - append-only means *only* this grows.
    auto append(std::string_view text) -> void;

private:
    std::string _name;
    BufferCapability _capability;
    std::vector<std::string> _lines { std::string() };
};

}

#endif // SPICE_CORE_BUFFER_H

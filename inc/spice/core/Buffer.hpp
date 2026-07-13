#ifndef SPICE_CORE_BUFFER_H
#define SPICE_CORE_BUFFER_H

#include "spice/core/Position.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
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

    //! The file this buffer is tied to; empty for scratch/scrollback.
    auto path() const -> std::string const&;
    auto set_path(std::string path) -> void;

    //! True when the content changed since creation or the last save.
    auto dirty() const -> bool;
    auto mark_saved() -> void;

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
    //! Appends are not undoable (they are content arriving, not edits).
    auto append(std::string_view text) -> void;

    //! Undoes the most recent edit - a run of typing (or deleting) at one
    //! spot counts as a single edit. Returns where the change happened, so
    //! the caller can place its cursor; nothing when history is exhausted.
    auto undo() -> std::optional<Position>;
    //! Re-applies the last undone edit. Any new edit clears redo history.
    auto redo() -> std::optional<Position>;

private:
    //! One invertible edit: what happened, where, and the text involved.
    struct Edit {
        enum class Kind : uint8_t { insert_text, erase_text, split, join };
        Kind kind;
        Position position;
        std::string text;
    };

    auto raw_insert(Position position, std::string_view text) -> void;
    auto raw_erase(Position position, std::size_t bytes) -> void;
    auto raw_split(Position position) -> void;
    auto raw_join(uint32_t line) -> void;
    auto apply(Edit const& edit) -> Position;
    auto revert(Edit const& edit) -> Position;
    auto record(Edit&& edit) -> void;

    std::string _name;
    BufferCapability _capability;
    std::string _path;
    bool _dirty { false };
    std::vector<std::string> _lines { std::string() };
    std::vector<Edit> _undo;
    std::vector<Edit> _redo;
};

}

#endif // SPICE_CORE_BUFFER_H

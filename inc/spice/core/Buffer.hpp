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

    //! A counter bumped on every change, whatever the source (the plugin
    //! protocol's buffer version - a write must cite the version it saw).
    auto version() const -> uint64_t;

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

    //! The text in [begin, end), lines joined with '\n' (empty if the
    //! range is empty or out of bounds). Order of begin/end is normalized.
    auto text_between(Position begin, Position end) const -> std::string;

    //! Erases [begin, end) as a single undoable edit (multi-line ranges
    //! included). Editable buffers only.
    auto erase_range(Position begin, Position end) -> bool;

    //! Inserts text that may contain newlines at `position`, as a single
    //! undoable edit. Returns the position just past the inserted text
    //! (for the caller's cursor). Editable buffers only.
    auto insert_block(Position position, std::string_view text) -> std::optional<Position>;

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

    //! One change, protocol-shaped: the bytes [start, end) were replaced
    //! by `text`. Columns are byte offsets into the line's UTF-8 - how the
    //! plugin protocol addresses text (PROTOCOL.md) - unlike Position
    //! columns, which count characters.
    struct Change {
        uint32_t start_line { 0 };
        uint32_t start_byte { 0 };
        uint32_t end_line { 0 };
        uint32_t end_byte { 0 };
        std::string text; //!< what now sits in the range; empty for a pure erase

        auto operator==(Change const&) const -> bool = default;
    };

    //! The changes since the last call, oldest first, with the version the
    //! first applied against. `complete` false means the journal overflowed
    //! and the splices were dropped - a reader must refetch the buffer.
    struct ChangeSet {
        uint64_t from_version { 0 };
        bool complete { true };
        std::vector<Change> splices;
    };

    //! Drains the change journal (every mutation source: edits, appends,
    //! undo/redo). The next call starts from the current version.
    auto take_changes() -> ChangeSet;

    //! One splice of a batch: [begin, end) replaced by `text` (Positions,
    //! so character columns). Insert: begin == end. Delete: empty text.
    struct BatchSplice {
        Position begin;
        Position end;
        std::string text;
    };

    //! Applies every splice as a single undoable edit with a single
    //! version bump. Positions must be valid and the splices sorted in
    //! descending document order (so each cites unchanged text) - callers
    //! validate against the pre-batch content. Editable buffers only.
    auto apply_batch(std::vector<BatchSplice> const& splices) -> bool;

    // -- marks: positions that ride along with the text ----------------
    //! Which side of a mark an insertion exactly at it lands on: with
    //! `left` gravity the mark stays put, with `right` it follows the
    //! inserted text.
    enum class MarkGravity : uint8_t { left, right };

    //! Where a mark is now. Columns are byte offsets, like Change - marks
    //! belong to the plugin protocol's addressing. An invalid mark had the
    //! text around it deleted; it sits at the deletion point.
    struct MarkInfo {
        uint32_t line { 0 };
        uint32_t byte { 0 };
        bool valid { true };

        auto operator==(MarkInfo const&) const -> bool = default;
    };

    //! Anchors a mark (clamped into the buffer); its id is never reused.
    auto set_mark(uint32_t line, uint32_t byte, MarkGravity gravity) -> uint64_t;
    auto mark(uint64_t id) const -> std::optional<MarkInfo>;
    auto delete_mark(uint64_t id) -> bool;

    // -- highlights: colored spans painted over the text ----------------
    //! A foreground-colored span, byte-addressed like Change. Highlights
    //! are decoration: they carry no meaning the core interprets, and the
    //! next edit may leave them slightly stale until their owner (a
    //! plugin) recomputes them against the new content.
    struct Highlight {
        uint32_t start_line { 0 };
        uint32_t start_byte { 0 };
        uint32_t end_line { 0 };
        uint32_t end_byte { 0 };
        uint32_t rgb { 0 }; //!< 0xRRGGBB foreground

        auto operator==(Highlight const&) const -> bool = default;
    };

    //! Replaces the buffer's whole highlight set (there is one set, not
    //! one per plugin - last writer wins).
    auto set_highlights(std::vector<Highlight> highlights) -> void;
    auto highlights() const -> std::vector<Highlight> const&;

private:
    //! One invertible edit: what happened, where, and the text involved.
    //! A `batch` edit is a compound: its children applied in order.
    struct Edit {
        enum class Kind : uint8_t {
            insert_text, erase_text, split, join,
            insert_block, erase_block, // multi-line text with '\n's
            batch,                     // children carry the parts
        };
        Kind kind;
        Position position;
        std::string text;
        std::vector<Edit> children {};
    };

    auto raw_insert(Position position, std::string_view text) -> void;
    auto raw_erase(Position position, std::size_t bytes) -> void;
    auto raw_split(Position position) -> void;
    auto raw_join(uint32_t line) -> void;
    auto raw_insert_block(Position position, std::string_view text) -> Position;
    auto raw_erase_block(Position begin, Position end) -> void;
    auto valid(Position position) const -> bool;
    auto apply(Edit const& edit) -> Position;
    auto revert(Edit const& edit) -> Position;
    auto record(Edit&& edit) -> void;

    //! Journals the protocol splice for an edit just applied (`inverted`
    //! for one just undone). Must run after the mutation.
    auto journal(Edit const& edit, bool inverted) -> void;
    //! The primitive: `text` was inserted at `position` - or erased from
    //! there, when `erased` - with `position` in character columns. Also
    //! shifts marks, so it runs for every mutation, journal cap or not.
    auto journal_change(Position position, std::string_view text, bool erased) -> void;
    //! Applies (or reverts) an edit and journals it right after - child by
    //! child for a batch, since the journal reads post-mutation content.
    auto apply_and_journal(Edit const& edit) -> Position;
    auto revert_and_journal(Edit const& edit) -> Position;
    //! Moves every mark across a just-applied change (byte coordinates).
    auto shift_marks(Change const& change) -> void;

    std::string _name;
    BufferCapability _capability;
    std::string _path;
    bool _dirty { false };
    uint64_t _version { 0 };
    std::vector<std::string> _lines { std::string() };
    std::vector<Edit> _undo;
    std::vector<Edit> _redo;

    std::vector<Change> _changes; //!< protocol splices since the last take
    uint64_t _changes_base_version { 0 };
    bool _changes_complete { true };

    struct MarkSlot {
        uint64_t id;
        MarkGravity gravity;
        MarkInfo at;
    };
    std::vector<MarkSlot> _marks;
    uint64_t _next_mark_id { 1 };

    std::vector<Highlight> _highlights;
};

}

#endif // SPICE_CORE_BUFFER_H

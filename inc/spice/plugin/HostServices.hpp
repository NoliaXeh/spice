#ifndef SPICE_PLUGIN_HOSTSERVICES_H
#define SPICE_PLUGIN_HOSTSERVICES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spice::plugin {

//! A command a plugin offers, as it appears in the palette.
struct PluginCommand {
    std::string name;
    std::string title;
    std::string description;
    bool cancellable { false }; //!< invoking it offers a Cancel entry
};

//! A buffer position as the protocol addresses it: a line and a byte offset
//! into that line's UTF-8. Ranges are half-open [start, end).
struct BufferPosition {
    uint64_t line { 0 };
    uint64_t column { 0 }; //!< byte offset into the line
};

struct BufferRange {
    BufferPosition start;
    BufferPosition end;
};

//! One splice of a batch: `text` replaces `range` (PROTOCOL.md
//! buffer.splice_many). Insert is an empty range, delete an empty text.
struct Splice {
    BufferRange range;
    std::string text;
};

struct BufferInfo {
    uint64_t version { 0 };
    uint64_t line_count { 0 };
    bool editable { true };
    bool dirty { false };
    std::string name;
};

//! Why a splice was refused (PROTOCOL.md core error codes).
enum class SpliceStatus { ok, stale_version, capability_denied, no_such_id, bad_params };

//! One grid.update: the layers of `rect` a plugin redrew on a grid pane.
//! Present layers are flat, row-major, exactly rows x cols long; empty
//! vectors mean "this layer did not change". Colors are 0xRRGGBB; style
//! is the PROTOCOL.md bitfield (bold, italic, underline, strikethrough,
//! reverse, dim, blink).
struct GridUpdate {
    uint64_t pane { 0 };
    uint32_t row { 0 };
    uint32_t col { 0 };
    uint32_t rows { 0 };
    uint32_t cols { 0 };
    std::vector<std::string> chars;
    std::vector<uint32_t> fg;
    std::vector<uint32_t> bg;
    std::vector<uint32_t> style;
};

//! Where a mark sits now (columns are byte offsets, like BufferPosition).
//! An invalid mark had the text around it deleted.
struct MarkInfo {
    uint64_t line { 0 };
    uint64_t column { 0 };
    bool valid { true };
};

//! One highlight: `range` drawn with foreground `fg` (0xRRGGBB).
struct HighlightSpan {
    BufferRange range;
    uint32_t fg { 0 };
};

//! What the core exposes to plugins, behind the wire protocol. The plugin
//! host translates protocol messages into these calls; the application
//! implements them against the live session. Everything is non-blocking
//! and authoritative on the core side (invariants 1 and 2).
class HostServices {
public:
    virtual ~HostServices() = default;

    // -- commands and keybinds --------------------------------------
    virtual auto register_commands(
        std::string const& plugin, std::vector<PluginCommand> const& commands
    ) -> void = 0;
    virtual auto unregister_commands(
        std::string const& plugin, std::vector<std::string> const& names
    ) -> void = 0;
    virtual auto set_keybind(
        std::string const& plugin, std::string const& command, std::string const& key
    ) -> void = 0;

    // -- status, palette, logging -----------------------------------
    virtual auto status_message(std::string const& plugin, std::string const& text)
        -> void = 0;
    virtual auto status_error(
        std::string const& plugin, std::string const& code, std::string const& message
    ) -> void = 0;
    virtual auto open_palette() -> void = 0;
    virtual auto log(
        std::string const& plugin, std::string const& level, std::string const& message
    ) -> void = 0;

    // -- panes ------------------------------------------------------
    virtual auto open_pane(
        std::string const& kind, std::optional<uint64_t> buffer, std::string const& split
    ) -> void = 0;
    virtual auto close_pane(uint64_t pane) -> void = 0;
    virtual auto focus_pane(uint64_t pane) -> void = 0;
    virtual auto float_pane(uint64_t pane) -> void = 0;
    virtual auto dock_pane(uint64_t pane) -> void = 0;
    virtual auto set_pane_buffer(uint64_t pane, uint64_t buffer) -> void = 0;

    // -- buffers ----------------------------------------------------
    //! The buffer's metadata, or nothing when there is no such buffer.
    virtual auto buffer_info(uint64_t buffer) -> std::optional<BufferInfo> = 0;
    //! Lines [start, end) with the version they were read at; nothing when
    //! there is no such buffer.
    virtual auto buffer_lines(uint64_t buffer, uint64_t start, uint64_t end)
        -> std::optional<std::pair<std::vector<std::string>, uint64_t>> = 0;
    //! The text of `range` (clamped into the buffer) with the version it
    //! was read at; nothing when there is no such buffer.
    virtual auto buffer_text(uint64_t buffer, BufferRange range)
        -> std::optional<std::pair<std::string, uint64_t>> = 0;
    //! Creates a buffer, returning its id.
    virtual auto create_buffer(bool editable, std::string const& name) -> uint64_t = 0;
    virtual auto kill_buffer(uint64_t buffer) -> void = 0;
    //! Replaces `range` with `text` if `version` is current. On success
    //! sets `new_version`; otherwise the status says why (and, for a stale
    //! version, `new_version` carries the current one).
    virtual auto splice(
        uint64_t buffer, BufferRange range, std::string const& text,
        uint64_t version, uint64_t& new_version
    ) -> SpliceStatus = 0;
    //! Applies a batch of non-overlapping splices, every position relative
    //! to the cited base `version`, atomically: all or none, one version
    //! bump, one undo step. Overlapping splices are bad_params; the other
    //! checks match splice().
    virtual auto splice_many(
        uint64_t buffer, std::vector<Splice> const& splices,
        uint64_t version, uint64_t& new_version
    ) -> SpliceStatus = 0;

    // -- optional capabilities (default: absent / no-op) --------------

    //! Draws onto a grid pane's plugin surface.
    virtual auto grid_update(GridUpdate const& update) -> void { (void)update; }
    //! Drops the surface: the pane shows its buffer again.
    virtual auto grid_clear(uint64_t pane) -> void { (void)pane; }
    virtual auto grid_set_cursor(uint64_t pane, uint64_t line, uint64_t col, bool visible)
        -> void { (void)pane; (void)line; (void)col; (void)visible; }

    //! Anchors a mark in a buffer; 0 when there is no such buffer.
    //! `right_gravity`: an insertion exactly at the mark pushes it along.
    virtual auto set_mark(uint64_t buffer, BufferPosition position, bool right_gravity)
        -> uint64_t { (void)buffer; (void)position; (void)right_gravity; return 0; }
    //! Where a mark is now, with the buffer version it was read at.
    virtual auto get_mark(uint64_t buffer, uint64_t mark)
        -> std::optional<std::pair<MarkInfo, uint64_t>> {
        (void)buffer; (void)mark; return std::nullopt;
    }
    virtual auto delete_mark(uint64_t buffer, uint64_t mark) -> bool {
        (void)buffer; (void)mark; return false;
    }

    //! A dedicated pane (and its buffer) for a pane-mode plugin, created
    //! before its hello. {0, 0} when the host cannot provide one.
    virtual auto plugin_pane(std::string const& plugin) -> std::pair<uint64_t, uint64_t> {
        (void)plugin; return { 0, 0 };
    }

    //! Replaces a buffer's decoration: colored spans painted over its
    //! text wherever it is shown. Unknown buffers are ignored.
    virtual auto set_highlights(uint64_t buffer, std::vector<HighlightSpan> const& spans)
        -> void { (void)buffer; (void)spans; }
};

}

#endif // SPICE_PLUGIN_HOSTSERVICES_H

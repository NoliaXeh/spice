#ifndef SPICE_CORE_SPICE_H
#define SPICE_CORE_SPICE_H

#include "spice/core/Buffer.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Layout.hpp"
#include "spice/core/Pane.hpp"
#include "spice/core/Pty.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/Theme.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spice::core {

//! The session: owns the buffers, the panes viewing them, their layout and
//! the focus. Buffers and panes are decoupled - closing a pane never
//! destroys its buffer, and a buffer can exist with no pane showing it.
class Spice {
public:
    Spice(std::string&& name);

    auto name() const -> std::string const&;

    //! The screen rectangle panes lay out into. Set it before opening panes.
    auto set_screen(Rectangle screen) -> void;

    //! Creates a buffer; it lives for the whole session, shown or not.
    auto create_buffer(
        std::string&& name, BufferCapability capability, std::string_view content = {}
    ) -> std::shared_ptr<Buffer>;
    auto buffers() const -> std::vector<std::shared_ptr<Buffer>> const&;

    //! Opens a tiled pane on a buffer, splitting the focused tile (or
    //! filling the screen for the first pane). Focuses it; returns its id.
    //! This overload picks the orientation from the tile's shape.
    auto open_pane(PaneType type, std::shared_ptr<Buffer> buffer) -> uint32_t;
// This ios a comment
    //! Same, with an explicit orientation: `horizontal` puts the new pane
    //! to the right of the split tile (vertical divider), otherwise below.
    auto open_pane(PaneType type, std::shared_ptr<Buffer> buffer, bool horizontal) -> uint32_t;

    //! Opens a floating pane at `rect`, on top of the tree. Focuses it.
    auto open_float(PaneType type, std::shared_ptr<Buffer> buffer, Rectangle rect) -> uint32_t;

    //! The "Welcome" pane shown on startup (README).
    auto open_welcome_pane() -> uint32_t;

    //! Opens a PTY pane running `argv` on a pseudo-terminal sized to the
    //! pane's content, with an append-only scrollback buffer. Returns the
    //! pane id, or 0 when the process could not be started.
    auto open_pty_pane(std::vector<std::string> const& argv) -> uint32_t;

    //! Drains every running pty into its scrollback (filtered); returns
    //! the ids of the panes whose buffer changed. A child that hung up
    //! gets a closing "[exited]" line.
    auto pump_ptys() -> std::vector<uint32_t>;

    //! Sends bytes to the pty behind a pane; false if it has none running.
    auto write_to_pty(uint32_t id, std::string_view bytes) -> bool;

    //! Propagates each pty pane's current content size to its pty.
    auto resize_ptys() -> void;

    //! Closes the focused pane; its buffer survives. When pane_count()
    //! reaches 0 the program should end (README).
    auto close_focused_pane() -> void;

    auto pane_count() const -> size_t;
    auto pane_ids() const -> std::vector<uint32_t>;
    auto pane(uint32_t id) -> Pane*; //!< nullptr if unknown
    auto focused_id() const -> uint32_t; //!< 0 when no pane is focused
    auto focused_pane() -> Pane*;
    auto focus(uint32_t id) -> void;
    auto move_focus(Direction direction) -> void;

    //! Takes the focused tiled pane out of the tree, floating it centered.
    auto float_focused() -> void;
    //! Docks the focused floating pane back into the tree.
    auto dock_focused() -> void;
    //! Moves/resizes a floating pane; false if it is not floating.
    auto move_float(uint32_t id, Rectangle rect) -> bool;
    auto is_floating(uint32_t id) const -> bool;
    //! Exchanges the places of two panes (tile/tile, float/float or mixed).
    auto swap_panes(uint32_t a, uint32_t b) -> bool;

    //! Resizes the focused pane by cell deltas (see Layout::resize_pane),
    //! keeping any PTYs in step with their pane's new content size.
    auto resize_focused(int width_delta, int height_delta) -> bool;

    auto pane_at(Position point) const -> std::optional<uint32_t>;
    //! The rectangle a pane currently occupies (tile or float).
    auto pane_area(uint32_t id) const -> std::optional<Rectangle>;

    //! Draws every pane into the grid: tiles first, then floats bottom to
    //! top, so floating panes overlay the tree.
    auto draw(Grid& grid, Theme const& theme) -> void;

private:
    //! Wide tiles split side by side, tall ones top/bottom.
    auto split_is_horizontal(Rectangle rect) const -> bool;
    //! The tile to split when inserting: the focused tile, else the largest.
    auto insertion_target() const -> uint32_t;

    struct PtyEntry {
        Pty pty;
        PtyFilter filter;
    };

    std::string _name;
    Rectangle _screen {};
    uint32_t _next_pane_id { 1 };
    std::vector<std::shared_ptr<Buffer>> _buffers;
    std::map<uint32_t, Pane> _panes;
    std::map<uint32_t, PtyEntry> _ptys;
    Layout _layout;
    uint32_t _focused { 0 };
};

}

#endif // SPICE_CORE_SPICE_H

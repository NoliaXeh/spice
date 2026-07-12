#ifndef SPICE_CORE_PANE_H
#define SPICE_CORE_PANE_H

#include "spice/core/Buffer.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/Theme.hpp"
#include <cstdint>
#include <memory>

namespace spice::core {

enum class PaneType : uint8_t {
    edit, //!< text editing; the core handles input and line editing
    grid, //!< raw pane; everything is handled by a plugin
    pty,  //!< interactive shell pane (type declared; process plumbing to come)
};

//! A view onto a buffer. Panes never own content: several panes can show
//! the same buffer, and closing a pane leaves the buffer alive. A pane
//! carries what is view-specific - cursor and scroll - and knows how to
//! draw itself (border, title, content) into a Grid rectangle.
class Pane {
public:
    Pane(PaneType type, std::shared_ptr<Buffer> buffer);

    auto type() const -> PaneType;
    auto buffer() const -> std::shared_ptr<Buffer> const&;

    //! Cursor in buffer coordinates, clamped to the buffer on writes
    //! (column may equal the line length: the insertion point at the end).
    auto cursor() const -> Position;
    auto set_cursor(Position position) -> void;

    auto scroll() const -> Position;
    //! Scrolls so the buffer's last lines fill the view (scrollback style).
    auto scroll_to_bottom(Rectangle area) -> void;

    //! The rectangle inside the border where content shows (empty when the
    //! area is too small for a border).
    static auto content_area(Rectangle area) -> Rectangle;

    //! Draws border, title (the buffer's name) and content into `grid`,
    //! scrolling first so the cursor stays visible.
    auto draw(Grid& grid, Rectangle area, bool focused, Theme const& theme) -> void;

    //! The buffer position for a screen point, given the pane drawn at
    //! `area` (clamped into the buffer).
    auto position_from_screen(Rectangle area, Position screen) const -> Position;

    //! Where the terminal cursor should be parked for this pane.
    auto cursor_screen_position(Rectangle area) const -> Position;

private:
    auto clamp(Position position) const -> Position;
    auto scroll_to_cursor(Rectangle content) -> void;

    PaneType _type;
    std::shared_ptr<Buffer> _buffer;
    Position _cursor { 0, 0, 0 };
    Position _scroll { 0, 0, 0 };
};

}

#endif // SPICE_CORE_PANE_H

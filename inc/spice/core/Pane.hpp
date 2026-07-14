#ifndef SPICE_CORE_PANE_H
#define SPICE_CORE_PANE_H

#include "spice/core/Buffer.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/Theme.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

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

    //! A read-only pane rejects edits made through it (the editing engine
    //! and clipboard commands check this) while cursor, selection and
    //! copying keep working. It is a property of the view: the buffer may
    //! still change through another pane. Shown as [ro] in the title.
    auto read_only() const -> bool;
    auto set_read_only(bool read_only) -> void;

    //! Selection: the range between an anchor and the cursor. Set the
    //! anchor where selecting starts (shift-move, mouse press); it stays
    //! put while the cursor extends the range. No anchor - or anchor equal
    //! to the cursor - means no selection.
    auto set_anchor(Position position) -> void;
    auto clear_anchor() -> void;
    auto has_anchor() const -> bool;
    //! The selected range in document order, or nothing.
    auto selection() const -> std::optional<std::pair<Position, Position>>;

    auto scroll() const -> Position;
    //! Scrolls so the buffer's last lines fill the view (scrollback style).
    auto scroll_to_bottom(Rectangle area) -> void;

    //! The rectangle inside the chrome where content shows (empty when the
    //! area is too small): below the title bar, inside the side borders.
    static auto content_area(Rectangle area) -> Rectangle;

    //! The title bar's float-toggle button (the F) for a pane drawn at
    //! `area`; zero-sized when the pane is too narrow for buttons.
    static auto float_button(Rectangle area) -> Rectangle;
    //! The title bar's close button (the x); zero-sized when too narrow.
    static auto close_button(Rectangle area) -> Rectangle;

    //! The mouse-resize grab handle: the last two cells of the bottom
    //! border (dragging it moves the pane's bottom-right corner).
    static auto resize_corner(Rectangle area) -> Rectangle;

    //! Draws the pane chrome and content into `grid`: a title bar (the
    //! buffer's name plus the F / x buttons) across the top, side borders,
    //! a rounded bottom - scrolling first so the cursor stays visible.
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
    bool _read_only { false };
    Position _cursor { 0, 0, 0 };
    Position _scroll { 0, 0, 0 };
    std::optional<Position> _anchor;
};

}

#endif // SPICE_CORE_PANE_H

#ifndef SPICE_CORE_LAYOUT_H
#define SPICE_CORE_LAYOUT_H

#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace spice::core {

enum class Direction : uint8_t { left, right, up, down };

//! Where panes live on screen: tiled panes in a split tree, floating panes
//! in a flat list on top of it, each float with its own rectangle and a
//! z-order given by its rank in the list (last = topmost). The layout only
//! tracks pane ids; it knows nothing about pane contents.
class Layout {
public:
    struct Node; //!< split-tree node; defined in Layout.cpp

    Layout();
    ~Layout();
    Layout(Layout&&);
    auto operator=(Layout&&) -> Layout&;

    //! No tiled and no floating panes at all.
    auto empty() const -> bool;
    auto contains(uint32_t pane) const -> bool;
    auto is_floating(uint32_t pane) const -> bool;

    //! Splits the tile holding `at` in two, the new pane taking the second
    //! half (right of it when `horizontal`, below it otherwise). When the
    //! tree is empty the pane becomes the whole tree and `at` is ignored.
    auto insert(uint32_t pane, uint32_t at, bool horizontal) -> bool;

    //! Removes a pane from the tree (its sibling takes the space) or from
    //! the floating list.
    auto remove(uint32_t pane) -> bool;

    //! Floats a pane at `rect`, on top, taking it out of the tree if it was
    //! tiled. Panes can start floating without ever having been tiled.
    auto float_pane(uint32_t pane, Rectangle rect) -> bool;

    //! Puts a floating pane back into the tree, splitting the tile holding
    //! `at` (like insert). `at` is ignored when the tree is empty.
    auto dock_pane(uint32_t pane, uint32_t at, bool horizontal) -> bool;

    //! Moves/resizes a floating pane; false if the pane is not floating.
    auto move_float(uint32_t pane, Rectangle rect) -> bool;

    //! Exchanges the places of two panes - tile with tile, float with
    //! float, or tile with float (which trades tiled for floating).
    auto swap(uint32_t a, uint32_t b) -> bool;

    //! The tiled panes and their rectangles for a screen of `screen` size.
    //! Splits partition their rectangle exactly, so tiles cover the screen.
    auto tiles(Rectangle screen) const -> std::vector<std::pair<uint32_t, Rectangle>>;

    //! The floating panes and their rectangles, bottom to top.
    auto floats() const -> std::vector<std::pair<uint32_t, Rectangle>>;

    //! The pane under `point`: topmost float first, else the tile there.
    auto pane_at(Rectangle screen, Position point) const -> std::optional<uint32_t>;

    //! The tiled pane one step in `direction` from `pane` (which may be
    //! floating; its own rectangle is the starting point either way).
    auto neighbor(Rectangle screen, uint32_t pane, Direction direction) const
        -> std::optional<uint32_t>;

private:
    struct Float {
        uint32_t pane;
        Rectangle rect;
    };

    auto rect_of(Rectangle screen, uint32_t pane) const -> std::optional<Rectangle>;

    std::unique_ptr<Node> _root;
    std::vector<Float> _floats;
};

}

#endif // SPICE_CORE_LAYOUT_H

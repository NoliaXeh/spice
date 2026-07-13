#ifndef SPICE_CORE_PALETTE_H
#define SPICE_CORE_PALETTE_H

#include "spice/core/Event.hpp"
#include "spice/core/Grid.hpp"
#include "spice/core/Position.hpp"
#include "spice/core/Rectangle.hpp"
#include "spice/core/Theme.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace spice::core {

//! The command palette: a modal overlay listing runnable commands. Typing
//! filters the list (case-insensitive substring on the title), up/down move
//! the selection, RETURN picks, ESCAPE closes. The palette knows nothing
//! about running commands - it hands back the picked name.
class Palette {
public:
    struct Item {
        std::string name;  //!< command name, returned by selected_name()
        std::string title; //!< what is listed and filtered on
    };

    //! What a key did, so the caller knows how to react.
    enum class Outcome : uint8_t {
        ignored, //!< key meant nothing to the palette
        updated, //!< filter or selection changed: repaint the palette
        closed,  //!< palette closed without picking
        picked,  //!< a command was picked (see selected_name()); palette closed
    };

    //! Opens over these items, sorted by title; resets filter and selection.
    auto open(std::vector<Item> items) -> void;
    auto close() -> void;
    auto is_open() const -> bool;

    auto handle(KeyEvent const& key) -> Outcome;

    auto query() const -> std::string const&;
    //! Items matching the current filter, display order.
    auto filtered() const -> std::vector<Item> const&;
    auto selected_index() const -> uint32_t;
    //! The picked/selected command name; empty when nothing matches.
    auto selected_name() const -> std::string;

    //! Where the palette sits on `screen`: centered, capped size.
    static auto area(Rectangle screen) -> Rectangle;

    //! Paints the palette into the grid (border, query line, list with the
    //! selection highlighted). Call after the panes so it overlays them.
    auto draw(Grid& grid, Rectangle screen, Theme const& theme) -> void;

    //! Where the terminal cursor should be parked: end of the query text.
    auto cursor_screen_position(Rectangle screen) const -> Position;

private:
    auto refilter() -> void;

    bool _open { false };
    std::vector<Item> _items;
    std::vector<Item> _filtered;
    std::string _query;
    uint32_t _selected { 0 };
    uint32_t _scroll { 0 };
};

}

#endif // SPICE_CORE_PALETTE_H

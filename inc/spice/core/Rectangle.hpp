#ifndef SPICE_CORE_RECTANGLE_H
#define SPICE_CORE_RECTANGLE_H

#include "spice/core/Position.hpp"
#include <cstdint>

namespace spice::core {

//! An axis-aligned rectangle of cells: `position` is the top-left corner,
//! spanning `width` columns and `height` lines.
struct Rectangle {
    Position position;
    uint32_t width;
    uint32_t height;

    auto operator==(Rectangle const&) const -> bool = default;

    //! Whether `point` falls inside the rectangle (layers are ignored).
    constexpr auto contains(Position point) const -> bool {
        return point.line >= position.line && point.line < position.line + height
            && point.column >= position.column && point.column < position.column + width;
    }
};

}

#endif // SPICE_CORE_RECTANGLE_H

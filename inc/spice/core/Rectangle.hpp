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
};

}

#endif // SPICE_CORE_RECTANGLE_H

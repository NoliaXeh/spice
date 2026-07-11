#ifndef SPICE_CORE_POSITION_H
#define SPICE_CORE_POSITION_H

#include <cstdint>
namespace spice::core {

//! represent a position in a frame/pane/terminal
struct Position {
    uint32_t line;
    uint32_t column;
    uint32_t layer;
};

}

#endif // SPICE_CORE_POSITION_H
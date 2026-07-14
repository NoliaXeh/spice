#ifndef SPICE_CORE_POSITION_H
#define SPICE_CORE_POSITION_H

#include <cstdint>
namespace spice::core {

//! Represents a position in a frame, pane or terminal (in cells or
//! characters depending on the consumer).
struct Position {
    uint32_t line;
    uint32_t column;
    uint32_t layer;

    auto operator==(Position const&) const -> bool = default;
};

//! Whether `a` comes before `b` in document order (line, then column;
//! layers are ignored).
constexpr auto document_order(Position a, Position b) -> bool {
    return a.line < b.line || (a.line == b.line && a.column < b.column);
}

}

#endif // SPICE_CORE_POSITION_H
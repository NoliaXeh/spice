#ifndef SPICE_CORE_COLOR_H
#define SPICE_CORE_COLOR_H

#include <cstdint>
namespace spice::core {

struct StyleFlags {
    bool bold: 1;
    bool italic: 1;
    bool underline: 1;
    bool strikethrought: 1;
    bool blinking: 1;
    bool selected: 1;

    auto operator==(StyleFlags const&) const -> bool = default;
};

struct Color {
    uint8_t r, g, b;
    StyleFlags style;

    auto operator==(Color const&) const -> bool = default;
};

//! Usual colors, plain style. Combine with designated initializers or
//! mutate a copy when a styled variant is needed.
namespace colors {

inline constexpr Color black   { .r = 0x00, .g = 0x00, .b = 0x00, .style = {} };
inline constexpr Color white   { .r = 0xFF, .g = 0xFF, .b = 0xFF, .style = {} };
inline constexpr Color gray    { .r = 0x80, .g = 0x80, .b = 0x80, .style = {} };
inline constexpr Color red     { .r = 0xFF, .g = 0x00, .b = 0x00, .style = {} };
inline constexpr Color green   { .r = 0x00, .g = 0xFF, .b = 0x00, .style = {} };
inline constexpr Color blue    { .r = 0x00, .g = 0x00, .b = 0xFF, .style = {} };
inline constexpr Color yellow  { .r = 0xFF, .g = 0xFF, .b = 0x00, .style = {} };
inline constexpr Color cyan    { .r = 0x00, .g = 0xFF, .b = 0xFF, .style = {} };
inline constexpr Color magenta { .r = 0xFF, .g = 0x00, .b = 0xFF, .style = {} };
inline constexpr Color orange  { .r = 0xFF, .g = 0xA5, .b = 0x00, .style = {} };

}

}

#endif // SPICE_CORE_COLOR_H
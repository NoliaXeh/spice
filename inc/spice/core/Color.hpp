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
};

struct Color {
    uint8_t r, g, b;
    StyleFlags style;
};

}

#endif // SPICE_CORE_COLOR_H
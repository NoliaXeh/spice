#ifndef SPICE_CORE_UTF8_H
#define SPICE_CORE_UTF8_H

#include <cstddef>

namespace spice::core {

//! Number of bytes in the UTF-8 character starting at `lead`.
//! An invalid leading byte is treated as a single byte, so callers always
//! make forward progress instead of looping on malformed input.
constexpr auto utf8_length(char lead) -> std::size_t {
    auto const byte { static_cast<unsigned char>(lead) };
    if ((byte & 0x80) == 0x00) return 1;
    if ((byte & 0xE0) == 0xC0) return 2;
    if ((byte & 0xF0) == 0xE0) return 3;
    if ((byte & 0xF8) == 0xF0) return 4;
    return 1;
}

}

#endif // SPICE_CORE_UTF8_H

#ifndef SPICE_CORE_UTF8_H
#define SPICE_CORE_UTF8_H

#include <cstddef>
#include <string_view>

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

//! Byte offset of the start of the `index`-th UTF-8 character in `text`.
//! Returns text.size() if `index` runs past the last character.
constexpr auto utf8_offset(std::string_view text, std::size_t index) -> std::size_t {
    std::size_t offset { 0 };
    for (std::size_t i { 0 }; i < index && offset < text.size(); ++i) {
        offset += utf8_length(text[offset]);
    }
    return offset < text.size() ? offset : text.size();
}

//! Number of UTF-8 characters in `text`.
constexpr auto utf8_count(std::string_view text) -> std::size_t {
    std::size_t count { 0 };
    for (std::size_t offset { 0 }; offset < text.size(); offset += utf8_length(text[offset])) {
        ++count;
    }
    return count;
}

}

#endif // SPICE_CORE_UTF8_H

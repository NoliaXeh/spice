#include "spice/core/Base64.hpp"

#include <cstdint>

namespace {

constexpr std::string_view table {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
};

auto byte_at(std::string_view input, size_t index) -> uint32_t {
    return static_cast<uint32_t>(static_cast<unsigned char>(input[index]));
}

}

namespace spice::core {

auto base64_encode(std::string_view input) -> std::string {
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);

    size_t i { 0 };
    for (; i + 3 <= input.size(); i += 3) {
        uint32_t const n {
            byte_at(input, i) << 16 | byte_at(input, i + 1) << 8 | byte_at(input, i + 2)
        };
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (input.size() - i == 1) {
        uint32_t const n { byte_at(input, i) << 16 };
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += "==";
    } else if (input.size() - i == 2) {
        uint32_t const n { byte_at(input, i) << 16 | byte_at(input, i + 1) << 8 };
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

}

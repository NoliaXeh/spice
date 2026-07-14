#ifndef SPICE_CORE_BASE64_H
#define SPICE_CORE_BASE64_H

#include <string>
#include <string_view>

namespace spice::core {

//! Standard (RFC 4648) base64 encoding with padding. Used to push copied
//! text to the terminal's clipboard via OSC 52.
auto base64_encode(std::string_view input) -> std::string;

}

#endif // SPICE_CORE_BASE64_H

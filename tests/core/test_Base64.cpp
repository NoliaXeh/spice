#include "doctest.h"

#include "spice/core/Base64.hpp"

using namespace spice::core;

TEST_CASE("core::base64_encode() matches the RFC 4648 test vectors") {
    CHECK_EQ(base64_encode(""), "");
    CHECK_EQ(base64_encode("f"), "Zg==");
    CHECK_EQ(base64_encode("fo"), "Zm8=");
    CHECK_EQ(base64_encode("foo"), "Zm9v");
    CHECK_EQ(base64_encode("foob"), "Zm9vYg==");
    CHECK_EQ(base64_encode("fooba"), "Zm9vYmE=");
    CHECK_EQ(base64_encode("foobar"), "Zm9vYmFy");
}

TEST_CASE("core::base64_encode() handles binary bytes") {
    CHECK_EQ(base64_encode(std::string_view("\x00\xff\x10", 3)), "AP8Q");
}

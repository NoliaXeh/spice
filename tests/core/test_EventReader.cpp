#include "doctest.h"

#include "spice/core/EventReader.hpp"

using namespace spice::core;

TEST_CASE("core::EventReader is inert without a controlling terminal") {
    // Test runners have no tty on stdin: construction must not touch the
    // terminal state, and poll() must return nothing rather than block.
    EventReader reader;
    CHECK_FALSE(reader.poll(0).has_value());
    CHECK_FALSE(reader.poll(10).has_value());
}

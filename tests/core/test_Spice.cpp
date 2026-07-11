#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "spice/core/Spice.hpp"

using namespace spice;

TEST_CASE("core::Spice::name()") {
    auto sp { core::Spice("name") };
    CHECK_EQ(sp.name(), "name");
}
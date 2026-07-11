#include "doctest.h"

#include "spice/core/TermInfo.hpp"

#include <unistd.h>

using namespace spice::core;

TEST_CASE("core::TermInfo::pid() returns the current process id") {
    TermInfo info;
    CHECK_EQ(info.pid(), getpid());
}

TEST_CASE("core::TermInfo::width() and height() without a controlling terminal") {
    // Test runners typically redirect stdout, so there is no tty to query;
    // TermInfo should degrade to 0 rather than crash or return garbage.
    TermInfo info;
    if (!isatty(STDOUT_FILENO)) {
        CHECK_EQ(info.width(), 0u);
        CHECK_EQ(info.height(), 0u);
    } else {
        CHECK_GT(info.width(), 0u);
        CHECK_GT(info.height(), 0u);
    }
}

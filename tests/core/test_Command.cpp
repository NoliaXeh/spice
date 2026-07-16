#include "doctest.h"

#include "spice/core/Command.hpp"

using namespace spice::core;

TEST_CASE("core::CommandRegistry runs registered commands") {
    CommandRegistry registry;
    int ran { 0 };
    CHECK(registry.add({ "test.run", "Run the test", [&] { ++ran; } }));

    CHECK(registry.run("test.run"));
    CHECK_EQ(ran, 1);
    CHECK_FALSE(registry.run("test.unknown"));
    CHECK_EQ(ran, 1);
}

TEST_CASE("core::CommandRegistry rejects duplicate names") {
    CommandRegistry registry;
    CHECK(registry.add({ "a", "first", [] {} }));
    CHECK_FALSE(registry.add({ "a", "second", [] {} }));
    CHECK_EQ(registry.commands().size(), 1u);
    CHECK_EQ(registry.find("a")->title, "first");
}

TEST_CASE("core::CommandRegistry::find() returns nothing for unknown names") {
    CommandRegistry registry;
    CHECK_FALSE(registry.find("nope"));
}

TEST_CASE("core::CommandRegistry::remove() unregisters") {
    CommandRegistry registry;
    registry.add({ "a", "A", [] {} });
    CHECK(registry.remove("a"));
    CHECK_FALSE(registry.find("a"));
    CHECK_FALSE(registry.remove("a"));
    CHECK(registry.add({ "a", "again", [] {} })); // name free again
}

TEST_CASE("core::CommandRegistry::run() rejects commands without an action") {
    CommandRegistry registry;
    registry.add({ "empty", "No action", {} });
    CHECK_FALSE(registry.run("empty"));
}

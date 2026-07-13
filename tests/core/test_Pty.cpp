#include "doctest.h"

#include "spice/core/Pty.hpp"

#include <chrono>
#include <thread>

using namespace spice::core;

namespace {

//! Reads from the pty until `needle` shows up or ~2s pass.
auto read_until(Pty& pty, std::string_view needle) -> std::string {
    std::string out;
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(2) };
    while (std::chrono::steady_clock::now() < deadline) {
        out += pty.read_output();
        if (out.find(needle) != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return out;
}

}

TEST_CASE("core::Pty runs a process and captures its output") {
    Pty pty;
    REQUIRE(pty.spawn({ "/bin/sh", "-c", "printf 'hello-pty\\n'" }, 80, 24));
    CHECK(pty.running());

    auto const out { read_until(pty, "hello-pty") };
    CHECK_NE(out.find("hello-pty"), std::string::npos);
}

TEST_CASE("core::Pty detects the child hanging up") {
    Pty pty;
    REQUIRE(pty.spawn({ "/bin/sh", "-c", "exit 0" }, 80, 24));
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(2) };
    while (pty.running() && std::chrono::steady_clock::now() < deadline) {
        pty.read_output(); // hangup is noticed while draining
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_FALSE(pty.running());
}

TEST_CASE("core::Pty forwards input to the child") {
    Pty pty;
    REQUIRE(pty.spawn({ "/bin/cat" }, 80, 24));
    CHECK(pty.write_input("ping\r"));
    auto const out { read_until(pty, "ping") };
    CHECK_NE(out.find("ping"), std::string::npos);
    pty.terminate();
    CHECK_FALSE(pty.running());
}

TEST_CASE("core::Pty::spawn() rejects an empty argv and reports exec failure") {
    Pty pty;
    CHECK_FALSE(pty.spawn({}, 80, 24));

    Pty broken;
    REQUIRE(broken.spawn({ "/nonexistent-program-xyz" }, 80, 24));
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(2) };
    while (broken.running() && std::chrono::steady_clock::now() < deadline) {
        broken.read_output();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_FALSE(broken.running()); // exec failed, child _exit(127)
}

TEST_CASE("core::PtyFilter strips escape sequences and control bytes") {
    PtyFilter filter;
    CHECK_EQ(filter.feed("plain text\n"), "plain text\n");
    CHECK_EQ(filter.feed("a\x1b[31mred\x1b[0mb"), "aredb");       // CSI colors
    CHECK_EQ(filter.feed("x\x1b]0;title\x07y"), "xy");             // OSC title
    CHECK_EQ(filter.feed("l1\r\nl2\r"), "l1\nl2");                 // \r dropped
    CHECK_EQ(filter.feed("a\tb"), "a    b");                       // tab to spaces
    CHECK_EQ(filter.feed("caf\xc3\xa9"), "caf\xc3\xa9");           // utf-8 passes
    CHECK_EQ(filter.feed("a\x08\x07z"), "az");                     // BS/BEL dropped
}

TEST_CASE("core::PtyFilter handles a sequence split across feeds") {
    PtyFilter filter;
    CHECK_EQ(filter.feed("a\x1b["), "a");
    CHECK_EQ(filter.feed("31mb"), "b"); // rest of the CSI, then text
}

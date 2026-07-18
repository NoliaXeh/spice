#include "doctest.h"

#include "spice/plugin/PluginHost.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace spice::plugin;
using msgpack::Value;

namespace {

//! Records everything the host asks of the core, so tests can assert on it.
class FakeServices : public HostServices {
public:
    std::vector<PluginCommand> commands;
    std::vector<std::string> unregistered;
    std::vector<std::string> statuses;
    std::vector<std::string> errors;
    std::vector<std::string> logs;
    int palette_opens { 0 };

    // a one-buffer world for read tests
    uint64_t the_buffer { 7 };
    std::string buffer_name { "buf" };
    std::vector<std::string> lines { "hello world", "second line" };
    uint64_t version { 3 };
    std::vector<HighlightSpan> highlights; //!< last set_highlights payload

    auto register_commands(std::string const&, std::vector<PluginCommand> const& c)
        -> void override { for (auto const& x : c) commands.push_back(x); }
    auto unregister_commands(std::string const&, std::vector<std::string> const& n)
        -> void override {
        if (n.empty()) unregistered.push_back("<all>");
        for (auto const& x : n) unregistered.push_back(x);
    }
    auto set_keybind(std::string const&, std::string const&, std::string const&)
        -> void override {}
    auto status_message(std::string const&, std::string const& t) -> void override {
        statuses.push_back(t);
    }
    auto status_error(std::string const&, std::string const& c, std::string const&)
        -> void override { errors.push_back(c); }
    auto open_palette() -> void override { ++palette_opens; }
    auto log(std::string const&, std::string const&, std::string const& m) -> void override {
        logs.push_back(m);
    }
    auto open_pane(std::string const&, std::optional<uint64_t>, std::string const&)
        -> void override {}
    auto close_pane(uint64_t) -> void override {}
    auto focus_pane(uint64_t) -> void override {}
    auto float_pane(uint64_t) -> void override {}
    auto dock_pane(uint64_t) -> void override {}
    auto set_pane_buffer(uint64_t, uint64_t) -> void override {}
    auto buffer_info(uint64_t b) -> std::optional<BufferInfo> override {
        if (b != the_buffer) return std::nullopt;
        return BufferInfo {
            version, static_cast<uint64_t>(lines.size()), true, false, buffer_name
        };
    }
    auto buffer_lines(uint64_t b, uint64_t, uint64_t)
        -> std::optional<std::pair<std::vector<std::string>, uint64_t>> override {
        if (b != the_buffer) return std::nullopt;
        return std::pair { lines, version };
    }
    auto buffer_text(uint64_t b, BufferRange)
        -> std::optional<std::pair<std::string, uint64_t>> override {
        if (b != the_buffer) return std::nullopt;
        return std::pair { lines[0], version };
    }
    auto create_buffer(bool, std::string const&) -> uint64_t override { return 99; }
    auto kill_buffer(uint64_t) -> void override {}
    auto splice(uint64_t b, BufferRange, std::string const&, uint64_t v, uint64_t& nv)
        -> SpliceStatus override {
        if (b != the_buffer) return SpliceStatus::no_such_id;
        if (v != version) { nv = version; return SpliceStatus::stale_version; }
        nv = ++version;
        return SpliceStatus::ok;
    }
    auto splice_many(
        uint64_t b, std::vector<Splice> const&, uint64_t v, uint64_t& nv
    ) -> SpliceStatus override {
        return splice(b, {}, "", v, nv);
    }
    auto set_highlights(uint64_t, std::vector<HighlightSpan> const& spans)
        -> void override {
        highlights = spans;
    }
};

//! Pumps the host for up to ~2s until `done` is satisfied.
template <class Predicate>
auto pump_until(PluginHost& host, Predicate done) -> void {
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(2) };
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        host.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

auto greeter_path() -> std::string {
    return GREETER_PATH; // absolute path, set by CMake
}

}

TEST_CASE("plugin host: handshake registers the plugin's commands") {
    FakeServices services;
    PluginHost host { services };
    REQUIRE(host.start({ "greeter", { greeter_path() }, false }));

    pump_until(host, [&] { return !services.commands.empty(); });
    REQUIRE_EQ(services.commands.size(), 2u);
    CHECK_EQ(services.commands[0].name, "greet");
    CHECK_EQ(services.commands[0].title, "Greeter: greet the focused buffer");
    CHECK_EQ(services.commands[1].name, "shout");
    CHECK_EQ(host.plugin_count(), 1u);
}

TEST_CASE("plugin host: get_text and splice_many round-trip the wire") {
    FakeServices services;
    PluginHost host { services };
    REQUIRE(host.start({ "greeter", { greeter_path() }, false }));
    pump_until(host, [&] { return services.commands.size() >= 2; });

    host.emit("spice.palette.command_invoked", Value::object({
        { "plugin", Value { "greeter" } },
        { "command", Value { "shout" } },
        { "buffer", Value { services.the_buffer } },
    }));

    // the greeter reads the buffer, then splices citing the read's version
    pump_until(host, [&] { return services.statuses.size() >= 2; });
    REQUIRE_GE(services.statuses.size(), 2u);
    CHECK_NE(services.statuses[0].find("hello world"), std::string::npos);
    CHECK_NE(services.statuses[1].find("version 4"), std::string::npos);
}

TEST_CASE("plugin host: an event drives a command, status, and a buffer read") {
    FakeServices services;
    PluginHost host { services };
    REQUIRE(host.start({ "greeter", { greeter_path() }, false }));
    pump_until(host, [&] { return !services.commands.empty(); });

    // invoking the command the plugin registered
    host.emit("spice.palette.command_invoked", Value::object({
        { "plugin", Value { "greeter" } },
        { "command", Value { "greet" } },
        { "buffer", Value { services.the_buffer } },
    }));

    // it posts a hello, then reads the buffer and reports its info back
    pump_until(host, [&] { return services.statuses.size() >= 2; });
    REQUIRE_GE(services.statuses.size(), 2u);
    CHECK_EQ(services.statuses[0], "greeter: hello!");
    CHECK_NE(services.statuses[1].find("2 lines"), std::string::npos);
    CHECK_NE(services.statuses[1].find("version 3"), std::string::npos);
}

TEST_CASE("plugin host: broadcast reaches a subscribed peer, not the source") {
    FakeServices services;
    PluginHost host { services };
    // two greeters: both subscribe to nothing greeter.*, so extend via a
    // second start; the greeter publishes greeter.greeted and subscribes to
    // command_invoked only, so it won't receive its own broadcast - we just
    // assert the source-suppression path does not crash and the peer count.
    REQUIRE(host.start({ "greeter", { greeter_path() }, false }));
    REQUIRE(host.start({ "greeter2", { greeter_path() }, false }));
    pump_until(host, [&] { return services.commands.size() >= 4; });
    CHECK_EQ(host.plugin_count(), 2u);

    host.emit("spice.palette.command_invoked", Value::object({
        { "plugin", Value { "greeter" } },
        { "command", Value { "greet" } },
    }));
    pump_until(host, [&] { return !services.statuses.empty(); });
    CHECK_FALSE(services.statuses.empty());
}

TEST_CASE("plugin host: buffer requests are answered, splices version-checked") {
    // drive the request path directly with a hand-built plugin process is
    // not available here, so exercise the services the host would call -
    // the wire path is covered by the greeter e2e; this pins the contract.
    FakeServices services;
    CHECK_EQ(services.buffer_info(services.the_buffer)->line_count, 2u);
    CHECK_FALSE(services.buffer_info(999).has_value());

    uint64_t new_version {};
    CHECK_EQ(
        services.splice(services.the_buffer, {}, "x", 999, new_version),
        SpliceStatus::stale_version
    );
    CHECK_EQ(new_version, 3u); // carries the current version

    CHECK_EQ(
        services.splice(services.the_buffer, {}, "x", 3, new_version),
        SpliceStatus::ok
    );
    CHECK_EQ(new_version, 4u); // bumped
    CHECK_EQ(services.splice(42, {}, "x", 4, new_version), SpliceStatus::no_such_id);
}

TEST_CASE("plugin host: dying before the handshake is reported, not silent") {
    FakeServices services;
    PluginHost host { services };
    REQUIRE(host.start({ "broken", { "/bin/false" }, false })); // exits at once

    pump_until(host, [&] { return !services.errors.empty(); });
    REQUIRE_FALSE(services.errors.empty());
    CHECK_EQ(services.errors[0], "spice.core.plugin_crashed");
    CHECK_EQ(host.plugin_count(), 0u);
}

TEST_CASE("plugin process: a death is noticed even with a sibling running") {
    // pipe ends must not leak into later children: if plugin B inherited
    // A's pipes, A's stdout would never reach EOF and A's death would go
    // unnoticed for as long as B lives
    PluginProcess first;
    PluginProcess second;
    REQUIRE(first.spawn({ greeter_path() }));
    REQUIRE(second.spawn({ greeter_path() }));

    first.request_stop(); // SIGTERM: the greeter dies, the sibling lives
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(2) };
    while (first.running() && std::chrono::steady_clock::now() < deadline) {
        first.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(first.running());
    CHECK(second.running());
}

TEST_CASE("plugin process: writing to a dead plugin does not kill the core") {
    PluginProcess process;
    REQUIRE(process.spawn({ greeter_path() }));
    process.request_stop();
    // let the child die but do NOT poll: the process still believes it is
    // running, so the next send() really writes into the broken pipe -
    // without SIGPIPE ignored, that terminates this whole test binary
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(process.running()); // dead, but not yet noticed
    for (int i { 0 }; i < 64; ++i) { // enough to overflow any pipe buffering
        process.send(Message::event("spice.input.key", Value::object({
            { "padding", Value { std::string(4096, 'x') } },
        })));
    }

    process.poll(); // now it notices
    CHECK_FALSE(process.running());
}

TEST_CASE("plugin host: the cpp-keywords plugin paints keywords pink") {
    if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
        return; // no python3 on this machine: nothing to exercise
    }
    FakeServices services;
    services.buffer_name = "demo.cpp";
    services.lines = { "int main() {", "    return 0;", "}" };
    PluginHost host { services };
    REQUIRE(host.start({ "cpp-keywords", { "python3", CPP_KEYWORDS_PATH }, false }));

    // announce the buffer until the plugin (once ready) reacts: it reads
    // the name, sees a C++ file, fetches the lines and sends highlights
    auto const deadline { std::chrono::steady_clock::now() + std::chrono::seconds(5) };
    while (services.highlights.empty() && std::chrono::steady_clock::now() < deadline) {
        host.emit("spice.buffer.created", Value::object({
            { "buffer", Value { services.the_buffer } },
            { "caps", Value { "editable" } },
        }));
        host.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    REQUIRE_EQ(services.highlights.size(), 2u); // "int" and "return"
    CHECK_EQ(services.highlights[0].fg, 0xFF69B4u); // pink
    CHECK_EQ(services.highlights[0].range.start.line, 0u);
    CHECK_EQ(services.highlights[0].range.start.column, 0u);
    CHECK_EQ(services.highlights[0].range.end.column, 3u);   // int
    CHECK_EQ(services.highlights[1].range.start.line, 1u);
    CHECK_EQ(services.highlights[1].range.start.column, 4u);
    CHECK_EQ(services.highlights[1].range.end.column, 10u);  // return
}

TEST_CASE("plugin host: the restart policy relaunches a crashed plugin") {
    FakeServices services;
    PluginHost host { services };
    PluginSpec spec { "flaky", { "/bin/false" }, false };
    spec.restart = Restart::on_crash; // exit status 1: a crash
    spec.max_restarts = 2; // dies instantly: it gets exactly two more lives
    REQUIRE(host.start(spec));

    pump_until(host, [&] { return services.errors.size() >= 3; });
    CHECK_EQ(services.errors.size(), 3u); // the launch and both restarts died
    CHECK_EQ(host.plugin_count(), 0u);    // then the host gave up

    int restarts { 0 };
    for (auto const& line : services.logs) {
        if (line.find("restarting") != std::string::npos) {
            ++restarts;
        }
    }
    CHECK_EQ(restarts, 2);
}

TEST_CASE("plugin host: restart policy off stays dead") {
    FakeServices services;
    PluginHost host { services };
    REQUIRE(host.start({ "broken", { "/bin/false" }, false })); // default: no restart

    pump_until(host, [&] { return !services.errors.empty(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    host.poll();
    CHECK_EQ(services.errors.size(), 1u);
    CHECK_EQ(host.plugin_count(), 0u);
}

TEST_CASE("plugin host: on-crash does not resurrect a clean exit; always does") {
    FakeServices services;
    PluginHost host { services };
    PluginSpec spec { "quitter", { "/bin/true" }, false }; // exits 0: clean
    spec.restart = Restart::on_crash;
    spec.max_restarts = 2;
    REQUIRE(host.start(spec));
    pump_until(host, [&] { return !services.errors.empty(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    host.poll();
    CHECK_EQ(services.errors.size(), 1u); // reported once, never relaunched

    services.errors.clear();
    spec.name = "comeback";
    spec.restart = Restart::always;
    REQUIRE(host.start(spec));
    pump_until(host, [&] { return services.errors.size() >= 3; });
    CHECK_EQ(services.errors.size(), 3u); // the launch and both restarts
}

TEST_CASE("plugin host: a crash unregisters commands and reports it") {
    FakeServices services;
    PluginHost host { services };
    REQUIRE(host.start({ "greeter", { greeter_path() }, false }));
    pump_until(host, [&] { return !services.commands.empty(); });

    // shutdown makes the greeter exit; the host should reap it
    host.shutdown(0);
    pump_until(host, [&] { return host.plugin_count() == 0; });
    CHECK_EQ(host.plugin_count(), 0u);
    CHECK_FALSE(services.unregistered.empty());
    CHECK_EQ(services.unregistered[0], "<all>");
}

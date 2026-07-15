//! \file plugins/greeter/main.cpp
//! A small reference Spice plugin, exercising the protocol end to end:
//! it handshakes, registers a command, and on invocation reads the focused
//! buffer, posts a status line, and broadcasts a greeting. Written against
//! the same msgpack/framing code the core uses - a plugin in any language
//! would speak the identical wire format.

#include "spice/plugin/Message.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unistd.h>

using namespace spice::plugin;
using msgpack::Value;

namespace {

//! Reads one length-prefixed frame from stdin, blocking. Nothing on EOF.
auto read_frame() -> std::optional<Message> {
    static std::string buffer;
    while (true) {
        if (auto message { take_frame(buffer) }) {
            return message;
        }
        char chunk[4096];
        ssize_t const count { read(STDIN_FILENO, chunk, sizeof chunk) };
        if (count <= 0) {
            return std::nullopt; // core closed the pipe
        }
        buffer.append(chunk, static_cast<size_t>(count));
    }
}

auto write_message(Message const& message) -> void {
    std::string const frame { encode_frame(message) };
    ssize_t written { 0 };
    while (written < static_cast<ssize_t>(frame.size())) {
        ssize_t const n { write(STDOUT_FILENO, frame.data() + written, frame.size() - written) };
        if (n <= 0) {
            return;
        }
        written += n;
    }
}

auto notify(std::string method, Value params) -> void {
    write_message({ Kind::notify, 0, false, std::move(method), std::move(params) });
}

uint64_t next_request_id { 1 };

auto request(std::string method, Value params) -> uint64_t {
    uint64_t const id { next_request_id++ };
    write_message({ Kind::request, id, true, std::move(method), std::move(params) });
    return id;
}

}

int main() {
    uint64_t pending_focus { 0 }; // request id of an in-flight buffer read

    while (auto message { read_frame() }) {
        auto const& [kind, id, has_id, method, params] { *message };

        if (kind == Kind::event && method == "spice.lifecycle.hello") {
            notify("ready", Value::object({
                { "protocol", Value { "0.1" } },
                { "subscribe", Value { Value::Array {
                    Value { "spice.palette.command_invoked" },
                    Value { "spice.pane.focused" },
                } } },
                { "publishes", Value { Value::Array { Value { "greeter." } } } },
                { "commands", Value { Value::Array {
                    Value::object({
                        { "name", Value { "greet" } },
                        { "title", Value { "Greeter: greet the focused buffer" } },
                    }),
                } } },
            }));
        } else if (kind == Kind::event && method == "spice.palette.command_invoked"
                   && params["command"].as_string() == "greet") {
            notify("status.message", Value::object({
                { "text", Value { "greeter: hello!" } },
            }));
            notify("broadcast", Value::object({
                { "topic", Value { "greeter.greeted" } },
                { "payload", Value::object({ { "who", Value { "greeter" } } }) },
            }));
            if (params.contains("buffer")) {
                pending_focus = request("buffer.info", Value::object({
                    { "buffer", params["buffer"] },
                }));
            }
        } else if (kind == Kind::response && has_id && id == pending_focus) {
            notify("status.message", Value::object({
                { "text", Value {
                    "greeter: buffer has " + std::to_string(params["line_count"].as_int())
                    + " lines, version " + std::to_string(params["version"].as_int())
                } },
            }));
        } else if (kind == Kind::event && method == "spice.lifecycle.shutdown") {
            notify("log", Value::object({
                { "level", Value { "info" } },
                { "message", Value { "greeter: goodbye" } },
            }));
            return 0;
        }
    }
    return 0;
}

#include "spice/plugin/PluginHost.hpp"

#include <algorithm>
#include <utility>

namespace spice::plugin {

using msgpack::Value;

namespace {

//! The major part of a "major.minor" version string.
auto major(std::string_view version) -> std::string_view {
    return version.substr(0, version.find('.'));
}

auto strings(Value const& array) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (auto const& element : array.as_array()) {
        out.push_back(element.as_string());
    }
    return out;
}

}

PluginHost::PluginHost(HostServices& services)
    : _services { services }
{}

PluginHost::~PluginHost() = default;

auto PluginHost::start(PluginSpec spec) -> bool {
    auto plugin { std::make_unique<Plugin>() };
    plugin->spec = std::move(spec);
    plugin->id = _next_plugin_id++;

    if (!plugin->process.spawn(plugin->spec.command)) {
        _services.log(
            plugin->spec.name, "error", "could not start plugin process"
        );
        return false;
    }

    // the hello event (pane-mode pane/buffer fields are added once panes
    // own plugins; global plugins get name and mode)
    plugin->process.send(Message::event("spice.lifecycle.hello", Value::object({
        { "protocol", Value { std::string(protocol_version) } },
        { "plugin", Value { plugin->id } },
        { "name", Value { plugin->spec.name } },
        { "mode", Value { plugin->spec.pane_mode ? "pane" : "global" } },
    })));

    _plugins.push_back(std::move(plugin));
    return true;
}

auto PluginHost::poll() -> void {
    for (auto& plugin : _plugins) {
        for (auto const& message : plugin->process.poll()) {
            dispatch(*plugin, message);
        }
        if (auto errors { plugin->process.take_stderr() }; !errors.empty()) {
            _services.log(plugin->spec.name, "warn", std::move(errors));
        }
    }

    // reap the dead: unregister their commands, report, drop them. A
    // plugin that died before ready is reported too - crashing on startup
    // is the failure a user most needs to hear about.
    for (auto& plugin : _plugins) {
        if (!plugin->process.running()) {
            reap(*plugin);
        }
    }
    std::erase_if(_plugins, [](auto const& plugin) {
        return !plugin->process.running();
    });
}

auto PluginHost::reap(Plugin& plugin) -> void {
    if (plugin.ready) {
        _services.unregister_commands(plugin.spec.name, {}); // {}: all of them
    }
    _services.status_error(
        plugin.spec.name, "spice.core.plugin_crashed",
        plugin.ready
            ? "plugin '" + plugin.spec.name + "' exited"
            : "plugin '" + plugin.spec.name + "' exited before completing its handshake"
    );
    plugin.ready = false;
}

auto PluginHost::emit(std::string const& topic, Value params) -> void {
    for (auto& plugin : _plugins) {
        if (plugin->ready && subscribed(*plugin, topic)) {
            plugin->process.send(Message::event(topic, params));
        }
    }
}

auto PluginHost::shutdown(int grace_ms) -> void {
    for (auto& plugin : _plugins) {
        plugin->process.send(Message::event("spice.lifecycle.shutdown", Value::object({
            { "grace_ms", Value { grace_ms } },
        })));
    }
}

auto PluginHost::stop_all() -> void {
    for (auto& plugin : _plugins) {
        plugin->process.request_stop();
    }
}

auto PluginHost::plugin_count() const -> size_t {
    return _plugins.size();
}

// -- routing -----------------------------------------------------------

auto PluginHost::subscribed(Plugin const& plugin, std::string const& topic) -> bool {
    for (auto const& prefix : plugin.subscriptions) {
        if (topic.size() >= prefix.size() && topic.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

auto PluginHost::send_error(Plugin& plugin, std::string const& method, std::string const& code)
    -> void {
    plugin.process.send(Message::error(method, Value::object({
        { "code", Value { code } },
    })));
}

auto PluginHost::dispatch(Plugin& plugin, Message const& message) -> void {
    switch (message.kind) {
    case Kind::notify:
        handle_notify(plugin, message);
        break;
    case Kind::request:
        handle_request(plugin, message);
        break;
    default:
        break; // plugins never send events/responses/errors to the core
    }
}

auto PluginHost::handle_ready(Plugin& plugin, Value const& params) -> void {
    if (major(params["protocol"].as_string()) != major(protocol_version)) {
        _services.log(
            plugin.spec.name, "error",
            "protocol version mismatch, refusing plugin '" + plugin.spec.name + "'"
        );
        plugin.process.terminate();
        return;
    }
    plugin.subscriptions = strings(params["subscribe"]);
    plugin.ready = true;

    std::vector<PluginCommand> commands;
    for (auto const& entry : params["commands"].as_array()) {
        commands.push_back({
            entry["name"].as_string(),
            entry["title"].as_string(),
            entry["description"].as_string(),
        });
    }
    if (!commands.empty()) {
        _services.register_commands(plugin.spec.name, commands);
    }
}

auto PluginHost::handle_notify(Plugin& plugin, Message const& message) -> void {
    std::string const& method { message.method };
    Value const& params { message.params };
    std::string const& name { plugin.spec.name };

    if (method == "ready") {
        handle_ready(plugin, params);
    } else if (!plugin.ready) {
        return; // nothing but ready is honored before the handshake
    } else if (method == "subscribe") {
        plugin.subscriptions = strings(params["subscribe"]);
    } else if (method == "command.register") {
        std::vector<PluginCommand> commands;
        for (auto const& entry : params["commands"].as_array()) {
            commands.push_back({
                entry["name"].as_string(), entry["title"].as_string(),
                entry["description"].as_string(),
            });
        }
        _services.register_commands(name, commands);
    } else if (method == "command.unregister") {
        _services.unregister_commands(name, strings(params["names"]));
    } else if (method == "keybind.set") {
        // binds target (plugin, command); usually the sender's own, but the
        // params say so explicitly (PROTOCOL.md: never an id)
        _services.set_keybind(
            params["plugin"].as_string(name),
            params["command"].as_string(), params["key"].as_string()
        );
    } else if (method == "status.message") {
        _services.status_message(name, params["text"].as_string());
    } else if (method == "status.error") {
        _services.status_error(
            name, params["code"].as_string(), params["message"].as_string()
        );
    } else if (method == "log") {
        _services.log(name, params["level"].as_string(), params["message"].as_string());
    } else if (method == "palette.open") {
        _services.open_palette();
    } else if (method == "pane.open") {
        std::optional<uint64_t> buffer;
        if (params.contains("buffer")) {
            buffer = static_cast<uint64_t>(params["buffer"].as_int());
        }
        _services.open_pane(params["kind"].as_string(), buffer, params["split"].as_string());
    } else if (method == "pane.close") {
        _services.close_pane(static_cast<uint64_t>(params["pane"].as_int()));
    } else if (method == "pane.focus") {
        _services.focus_pane(static_cast<uint64_t>(params["pane"].as_int()));
    } else if (method == "pane.float") {
        _services.float_pane(static_cast<uint64_t>(params["pane"].as_int()));
    } else if (method == "pane.dock") {
        _services.dock_pane(static_cast<uint64_t>(params["pane"].as_int()));
    } else if (method == "pane.set_buffer") {
        _services.set_pane_buffer(
            static_cast<uint64_t>(params["pane"].as_int()),
            static_cast<uint64_t>(params["buffer"].as_int())
        );
    } else if (method == "buffer.kill") {
        _services.kill_buffer(static_cast<uint64_t>(params["buffer"].as_int()));
    } else if (method == "broadcast") {
        std::string const topic { params["topic"].as_string() };
        if (topic.starts_with("spice.")) {
            return; // the core's namespace is reserved; cannot be spoofed
        }
        Value const event { Value::object({
            { "source", Value { plugin.id } },
            { "payload", params["payload"] },
        }) };
        for (auto& other : _plugins) {
            if (other.get() != &plugin && other->ready && subscribed(*other, topic)) {
                other->process.send(Message::event(topic, event)); // never to the source
            }
        }
    } else {
        send_error(plugin, method, "spice.core.unknown_method");
    }
}

auto PluginHost::respond(Plugin& plugin, Message const& message, Value result) -> void {
    plugin.process.send(Message::response(message.id, message.method, std::move(result)));
}

auto PluginHost::respond_failure(
    Plugin& plugin, Message const& message, std::string const& code, Value extra
) -> void {
    Value::Map fields { { "code", Value { code } } };
    for (auto const& field : extra.as_map()) {
        fields.push_back(field);
    }
    respond(plugin, message, Value::make_map(std::move(fields)));
}

auto PluginHost::handle_request(Plugin& plugin, Message const& message) -> void {
    if (!plugin.ready) {
        return;
    }
    std::string const& method { message.method };
    if (method == "buffer.info") {
        request_buffer_info(plugin, message);
    } else if (method == "buffer.get_lines") {
        request_buffer_lines(plugin, message);
    } else if (method == "buffer.create") {
        request_buffer_create(plugin, message);
    } else if (method == "buffer.splice") {
        request_buffer_splice(plugin, message);
    } else {
        respond_failure(plugin, message, "spice.core.unknown_method");
    }
}

auto PluginHost::request_buffer_info(Plugin& plugin, Message const& message) -> void {
    auto const buffer_id { static_cast<uint64_t>(message.params["buffer"].as_int()) };
    auto const info { _services.buffer_info(buffer_id) };
    if (!info) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    respond(plugin, message, Value::object({
        { "line_count", Value { info->line_count } },
        { "version", Value { info->version } },
        { "caps", Value { info->editable ? "editable" : "append" } },
        { "dirty", Value { info->dirty } },
        { "name", Value { info->name } },
    }));
}

auto PluginHost::request_buffer_lines(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    auto const buffer_id { static_cast<uint64_t>(params["buffer"].as_int()) };
    auto const start { static_cast<uint64_t>(params["start"].as_int()) };
    auto const end { static_cast<uint64_t>(params["end"].as_int(-1)) };
    auto const lines { _services.buffer_lines(buffer_id, start, end) };
    if (!lines) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    Value::Array out;
    for (auto const& line : lines->first) {
        out.push_back(Value { line });
    }
    respond(plugin, message, Value::object({
        { "lines", Value { std::move(out) } },
        { "version", Value { lines->second } },
    }));
}

auto PluginHost::request_buffer_create(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    bool const editable { params["caps"].as_string("editable") != "append" };
    uint64_t const id { _services.create_buffer(editable, params["name"].as_string()) };
    respond(plugin, message, Value::object({ { "buffer", Value { id } } }));
}

auto PluginHost::request_buffer_splice(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    auto const buffer_id { static_cast<uint64_t>(params["buffer"].as_int()) };
    BufferRange const range {
        {
            static_cast<uint64_t>(params["range"]["start"]["line"].as_int()),
            static_cast<uint64_t>(params["range"]["start"]["col"].as_int()),
        },
        {
            static_cast<uint64_t>(params["range"]["end"]["line"].as_int()),
            static_cast<uint64_t>(params["range"]["end"]["col"].as_int()),
        },
    };
    uint64_t new_version {};
    auto const status { _services.splice(
        buffer_id, range, params["text"].as_string(),
        static_cast<uint64_t>(params["version"].as_int()), new_version
    ) };
    switch (status) {
    case SpliceStatus::ok:
        respond(plugin, message, Value::object({ { "version", Value { new_version } } }));
        break;
    case SpliceStatus::stale_version:
        respond_failure(plugin, message, "spice.core.stale_version",
                        Value::object({ { "version", Value { new_version } } }));
        break;
    case SpliceStatus::capability_denied:
        respond_failure(plugin, message, "spice.core.capability_denied");
        break;
    case SpliceStatus::bad_params:
        respond_failure(plugin, message, "spice.core.bad_params");
        break;
    case SpliceStatus::no_such_id:
        respond_failure(plugin, message, "spice.core.no_such_id");
        break;
    }
}

}

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

//! A protocol range map - {start: {line, col}, end: {line, col}} - as the
//! host's BufferRange. Absent fields read as zero.
auto decode_range(Value const& range) -> BufferRange {
    return {
        .start={
            .line=static_cast<uint64_t>(range["start"]["line"].as_int()),
            .column=static_cast<uint64_t>(range["start"]["col"].as_int()),
        },
        .end={
            .line=static_cast<uint64_t>(range["end"]["line"].as_int()),
            .column=static_cast<uint64_t>(range["end"]["col"].as_int()),
        },
    };
}

//! What a plugin may broadcast per second before the broker drops the rest.
constexpr uint32_t max_broadcasts_per_second { 100 };

//! The commands array of a ready / command.register payload.
auto decode_commands(Value const& array) -> std::vector<PluginCommand> {
    std::vector<PluginCommand> commands;
    for (auto const& entry : array.as_array()) {
        commands.push_back({
            entry["name"].as_string(),
            entry["title"].as_string(),
            entry["description"].as_string(),
            entry["cancellable"].as_bool(false),
        });
    }
    return commands;
}

//! keybind.set may carry modifiers separately: fold ["ctrl","alt"] plus
//! "g" into the config-style "ctrl-alt-g" the core binds on.
auto key_with_mods(std::string key, Value const& mods) -> std::string {
    std::string prefix;
    for (std::string_view const mod : { "ctrl", "alt", "shift" }) {
        for (auto const& entry : mods.as_array()) {
            if (entry.as_string() == mod) {
                prefix += std::string(mod) + "-";
            }
        }
    }
    return prefix + std::move(key);
}

//! A grid.update payload as the host's GridUpdate.
auto decode_grid_update(Value const& params) -> GridUpdate {
    GridUpdate update;
    update.pane = static_cast<uint64_t>(params["pane"].as_int());
    Value const& rect { params["rect"] };
    update.row = static_cast<uint32_t>(rect["row"].as_int());
    update.col = static_cast<uint32_t>(rect["col"].as_int());
    update.rows = static_cast<uint32_t>(rect["rows"].as_int());
    update.cols = static_cast<uint32_t>(rect["cols"].as_int());
    for (auto const& cell : params["chars"].as_array()) {
        update.chars.push_back(cell.as_string());
    }
    auto const numbers = [](Value const& layer, std::vector<uint32_t>& into) {
        for (auto const& value : layer.as_array()) {
            into.push_back(static_cast<uint32_t>(value.as_int()));
        }
    };
    numbers(params["fg"], update.fg);
    numbers(params["bg"], update.bg);
    numbers(params["style"], update.style);
    return update;
}

//! The status codes a splice can come back with, as protocol responses.
auto splice_code(SpliceStatus status) -> std::string_view {
    switch (status) {
    case SpliceStatus::ok: return {};
    case SpliceStatus::stale_version: return "spice.core.stale_version";
    case SpliceStatus::capability_denied: return "spice.core.capability_denied";
    case SpliceStatus::bad_params: return "spice.core.bad_params";
    case SpliceStatus::no_such_id: return "spice.core.no_such_id";
    }
    return "spice.core.bad_params";
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

    // the hello event; a pane-mode plugin gets a dedicated pane and buffer
    // of its own, created by the host before the handshake
    Value::Map hello {
        { "protocol", Value { std::string(protocol_version) } },
        { "plugin", Value { plugin->id } },
        { "name", Value { plugin->spec.name } },
        { "mode", Value { plugin->spec.pane_mode ? "pane" : "global" } },
    };
    if (plugin->spec.pane_mode) {
        auto const [pane, buffer] { _services.plugin_pane(plugin->spec.name) };
        if (pane != 0) {
            hello.emplace_back("pane", Value { pane });
            hello.emplace_back("buffer", Value { buffer });
        }
    }
    plugin->process.send(Message::event(
        "spice.lifecycle.hello", Value::make_map(std::move(hello))
    ));

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
    std::vector<PluginSpec> to_restart;
    for (auto& plugin : _plugins) {
        if (!plugin->process.running()) {
            reap(*plugin);
            bool const wants_restart {
                plugin->spec.restart == Restart::always
                || (plugin->spec.restart == Restart::on_crash
                    && !plugin->process.exited_cleanly())
            };
            if (wants_restart && should_restart(plugin->spec)) {
                to_restart.push_back(plugin->spec);
            }
        }
    }
    std::erase_if(_plugins, [](auto const& plugin) {
        return !plugin->process.running();
    });
    for (auto& spec : to_restart) { // per config's restart policy
        _services.log(spec.name, "info", "restarting plugin");
        start(std::move(spec));
    }
}

auto PluginHost::should_restart(PluginSpec const& spec) -> bool {
    if (_shutting_down || spec.restart == Restart::never) {
        return false;
    }
    auto const now { std::chrono::steady_clock::now() };
    auto& history { _restarts[spec.name] };
    std::erase_if(history, [&](auto when) { return now - when > spec.restart_window; });
    if (history.size() >= spec.max_restarts) {
        _services.log(spec.name, "error", "crashing too often; giving up on it");
        return false;
    }
    history.push_back(now);
    return true;
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
    _shutting_down = true; // deaths from here on are exits, not crashes
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

auto PluginHost::take_newly_ready() -> bool {
    return std::exchange(_newly_ready, false);
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
    plugin.publishes = strings(params["publishes"]); // the topic registry
    plugin.ready = true;
    _newly_ready = true; // the owner replays the world for the newcomer

    std::vector<PluginCommand> commands { decode_commands(params["commands"]) };
    if (!commands.empty()) {
        _services.register_commands(plugin.spec.name, commands);
    }
}

auto PluginHost::handle_notify(Plugin& plugin, Message const& message) -> void {
    std::string const& method { message.method };
    Value const& params { message.params };

    if (method == "ready") {
        handle_ready(plugin, params);
        return;
    }
    if (!plugin.ready) {
        return; // nothing but ready is honored before the handshake
    }
    if (method == "subscribe") {
        plugin.subscriptions = strings(params["subscribe"]);
    } else if (method == "broadcast") {
        handle_broadcast(plugin, params);
    } else if (!handle_session_notify(plugin, message)) {
        send_error(plugin, method, "spice.core.unknown_method");
    }
}

auto PluginHost::handle_session_notify(Plugin& plugin, Message const& message) -> bool {
    std::string const& method { message.method };
    Value const& params { message.params };
    std::string const& name { plugin.spec.name };
    auto const id_of = [&](std::string_view field) {
        return static_cast<uint64_t>(params[field].as_int());
    };

    if (method == "command.register") {
        _services.register_commands(name, decode_commands(params["commands"]));
    } else if (method == "command.unregister") {
        _services.unregister_commands(name, strings(params["names"]));
    } else if (method == "keybind.set") {
        // binds target (plugin, command); usually the sender's own, but the
        // params say so explicitly (PROTOCOL.md: never an id). Modifiers
        // may come separately or already folded into the key name.
        _services.set_keybind(
            params["plugin"].as_string(name),
            params["command"].as_string(),
            key_with_mods(params["key"].as_string(), params["mods"])
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
            buffer = id_of("buffer");
        }
        _services.open_pane(params["kind"].as_string(), buffer, params["split"].as_string());
    } else if (method == "pane.close") {
        _services.close_pane(id_of("pane"));
    } else if (method == "pane.focus") {
        _services.focus_pane(id_of("pane"));
    } else if (method == "pane.float") {
        _services.float_pane(id_of("pane"));
    } else if (method == "pane.dock") {
        _services.dock_pane(id_of("pane"));
    } else if (method == "pane.set_buffer") {
        _services.set_pane_buffer(id_of("pane"), id_of("buffer"));
    } else if (method == "buffer.kill") {
        _services.kill_buffer(id_of("buffer"));
    } else if (method == "buffer.set_highlights") {
        std::vector<HighlightSpan> spans;
        for (auto const& entry : params["highlights"].as_array()) {
            spans.push_back({
                decode_range(entry["range"]),
                static_cast<uint32_t>(entry["fg"].as_int()),
            });
        }
        _services.set_highlights(name, id_of("buffer"), spans);
    } else if (method == "grid.update") {
        GridUpdate update { decode_grid_update(params) };
        uint64_t const cells { static_cast<uint64_t>(update.rows) * update.cols };
        bool const layers_fit { // a present layer must cover the rect exactly
            (update.chars.empty() || update.chars.size() == cells)
            && (update.fg.empty() || update.fg.size() == cells)
            && (update.bg.empty() || update.bg.size() == cells)
            && (update.style.empty() || update.style.size() == cells)
        };
        if (!layers_fit || cells == 0) {
            send_error(plugin, method, "spice.core.bad_params");
        } else {
            _services.grid_update(update);
        }
    } else if (method == "grid.clear") {
        _services.grid_clear(id_of("pane"));
    } else if (method == "grid.set_cursor") {
        _services.grid_set_cursor(
            id_of("pane"),
            static_cast<uint64_t>(params["pos"]["line"].as_int()),
            static_cast<uint64_t>(params["pos"]["col"].as_int()),
            params["visible"].as_bool(true)
        );
    } else if (method == "pane.set_cursor") {
        _services.set_cursor(id_of("pane"), {
            static_cast<uint64_t>(params["pos"]["line"].as_int()),
            static_cast<uint64_t>(params["pos"]["col"].as_int()),
        });
    } else {
        return false;
    }
    return true;
}

auto PluginHost::within_broadcast_budget(Plugin& plugin) -> bool {
    auto const now { std::chrono::steady_clock::now() };
    if (now - plugin.broadcast_window >= std::chrono::seconds(1)) {
        plugin.broadcast_window = now;
        plugin.broadcasts_in_window = 0;
        plugin.broadcast_limited = false;
    }
    if (++plugin.broadcasts_in_window <= max_broadcasts_per_second) {
        return true;
    }
    if (!plugin.broadcast_limited) { // log the storm once, not per drop
        plugin.broadcast_limited = true;
        _services.log(
            plugin.spec.name, "warn",
            "broadcasting too fast; dropping the rest of this second"
        );
    }
    return false;
}

auto PluginHost::handle_broadcast(Plugin& plugin, Value const& params) -> void {
    std::string const topic { params["topic"].as_string() };
    if (topic.starts_with("spice.")) {
        return; // the core's namespace is reserved; cannot be spoofed
    }
    if (!within_broadcast_budget(plugin)) {
        return; // a loop or a firehose: drop, already logged
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
    } else if (method == "buffer.get_text") {
        request_buffer_text(plugin, message);
    } else if (method == "buffer.create") {
        request_buffer_create(plugin, message);
    } else if (method == "buffer.splice") {
        request_buffer_splice(plugin, message);
    } else if (method == "buffer.splice_many") {
        request_buffer_splice_many(plugin, message);
    } else if (method == "mark.set") {
        request_mark_set(plugin, message);
    } else if (method == "mark.get") {
        request_mark_get(plugin, message);
    } else if (method == "mark.delete") {
        request_mark_delete(plugin, message);
    } else if (method == "topics.list") {
        request_topics_list(plugin, message);
    } else if (method == "pane.info") {
        request_pane_info(plugin, message);
    } else {
        respond_failure(plugin, message, "spice.core.unknown_method");
    }
}

auto PluginHost::request_pane_info(Plugin& plugin, Message const& message) -> void {
    auto const info { _services.pane_info(
        static_cast<uint64_t>(message.params["pane"].as_int())
    ) };
    if (!info) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    respond(plugin, message, Value::object({
        { "buffer", Value { info->buffer } },
        { "cursor", Value::object({
            { "line", Value { info->cursor_line } },
            { "col", Value { info->cursor_column } },
        }) },
        { "kind", Value { info->kind } },
    }));
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

auto PluginHost::request_buffer_text(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    auto const buffer_id { static_cast<uint64_t>(params["buffer"].as_int()) };
    auto const text { _services.buffer_text(buffer_id, decode_range(params["range"])) };
    if (!text) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    respond(plugin, message, Value::object({
        { "text", Value { text->first } },
        { "version", Value { text->second } },
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
    uint64_t new_version {};
    auto const status { _services.splice(
        static_cast<uint64_t>(params["buffer"].as_int()),
        decode_range(params["range"]), params["text"].as_string(),
        static_cast<uint64_t>(params["version"].as_int()), new_version
    ) };
    respond_splice_status(plugin, message, status, new_version);
}

auto PluginHost::request_buffer_splice_many(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    std::vector<Splice> splices;
    for (auto const& entry : params["splices"].as_array()) {
        splices.push_back({ decode_range(entry["range"]), entry["text"].as_string() });
    }
    uint64_t new_version {};
    auto const status { _services.splice_many(
        static_cast<uint64_t>(params["buffer"].as_int()), splices,
        static_cast<uint64_t>(params["version"].as_int()), new_version
    ) };
    respond_splice_status(plugin, message, status, new_version);
}

auto PluginHost::request_mark_set(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    BufferPosition const position {
        static_cast<uint64_t>(params["pos"]["line"].as_int()),
        static_cast<uint64_t>(params["pos"]["col"].as_int()),
    };
    uint64_t const mark { _services.set_mark(
        static_cast<uint64_t>(params["buffer"].as_int()),
        position,
        params["gravity"].as_string("left") == "right"
    ) };
    if (mark == 0) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    respond(plugin, message, Value::object({ { "mark", Value { mark } } }));
}

auto PluginHost::request_mark_get(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    auto const found { _services.get_mark(
        static_cast<uint64_t>(params["buffer"].as_int()),
        static_cast<uint64_t>(params["mark"].as_int())
    ) };
    if (!found) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    auto const& [info, version] { *found };
    respond(plugin, message, Value::object({
        { "pos", Value::object({
            { "line", Value { info.line } },
            { "col", Value { info.column } },
        }) },
        { "valid", Value { info.valid } },
        { "version", Value { version } },
    }));
}

auto PluginHost::request_mark_delete(Plugin& plugin, Message const& message) -> void {
    Value const& params { message.params };
    bool const deleted { _services.delete_mark(
        static_cast<uint64_t>(params["buffer"].as_int()),
        static_cast<uint64_t>(params["mark"].as_int())
    ) };
    if (!deleted) {
        respond_failure(plugin, message, "spice.core.no_such_id");
        return;
    }
    respond(plugin, message, Value::object({}));
}

auto PluginHost::request_topics_list(Plugin& plugin, Message const& message) -> void {
    Value::Array topics;
    for (auto const& other : _plugins) {
        if (!other->ready) {
            continue;
        }
        for (auto const& prefix : other->publishes) {
            topics.push_back(Value::object({
                { "plugin", Value { other->spec.name } },
                { "prefix", Value { prefix } },
            }));
        }
    }
    respond(plugin, message, Value::object({ { "topics", Value { std::move(topics) } } }));
}

auto PluginHost::respond_splice_status(
    Plugin& plugin, Message const& message, SpliceStatus status, uint64_t new_version
) -> void {
    if (status == SpliceStatus::ok) {
        respond(plugin, message, Value::object({ { "version", Value { new_version } } }));
        return;
    }
    Value extra { Value::object({}) };
    if (status == SpliceStatus::stale_version) { // carries the current version
        extra = Value::object({ { "version", Value { new_version } } });
    }
    respond_failure(plugin, message, std::string(splice_code(status)), std::move(extra));
}

}

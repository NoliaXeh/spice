#ifndef SPICE_PLUGIN_PLUGINHOST_H
#define SPICE_PLUGIN_PLUGINHOST_H

#include "spice/plugin/HostServices.hpp"
#include "spice/plugin/Message.hpp"
#include "spice/plugin/PluginProcess.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace spice::plugin {

//! The protocol version the core speaks. Only the major part is negotiated.
inline constexpr std::string_view protocol_version { "0.1" };

//! When a dead plugin is relaunched (CONFIG.md's `restart`).
enum class Restart : uint8_t {
    never,    //!< stays dead
    on_crash, //!< relaunched unless it exited by itself with status 0
    always,   //!< relaunched for any death outside shutdown
};

//! How a plugin is launched (from CONFIG.md's `[[plugin]]`).
struct PluginSpec {
    std::string name;
    std::vector<std::string> command; //!< argv
    bool pane_mode { false };         //!< "pane" vs "global"

    //! Relaunch policy: at most `max_restarts` times within
    //! `restart_window`, then give up.
    Restart restart { Restart::never };
    uint32_t max_restarts { 3 };
    std::chrono::milliseconds restart_window { 60000 };
};

//! Runs plugins and brokers the protocol between them and the core.
//!
//! Owns each plugin subprocess, performs the hello/ready handshake with
//! major-version negotiation, routes core events to the plugins that
//! subscribed (by topic prefix), and dispatches the plugin -> core methods
//! onto HostServices. It broadcasts between plugins (a dumb prefix broker),
//! answers buffer requests, and manages lifecycle (graceful shutdown,
//! crash detection that unregisters the dead plugin's commands).
//!
//! Nothing here ever blocks on a plugin: poll() drains whatever has arrived
//! and returns. Call it every main-loop turn, like the PTY pump.
class PluginHost {
public:
    explicit PluginHost(HostServices& services);
    ~PluginHost();

    //! Spawns a plugin and sends it the hello event. False on spawn
    //! failure (already logged through the services).
    auto start(PluginSpec spec) -> bool;

    //! Drains every plugin: reads their frames, dispatches their methods,
    //! routes stderr to the log, and reaps any that died (unregistering
    //! their commands and reporting the death). Non-blocking.
    auto poll() -> void;

    //! Delivers a core event to every plugin subscribed to its topic.
    //! `topic` must start with "spice."; params is the event's map.
    auto emit(std::string const& topic, msgpack::Value params) -> void;

    //! Sends the shutdown event to every plugin (the caller then waits the
    //! grace period, pumping poll(), before destroying the host).
    auto shutdown(int grace_ms) -> void;

    //! SIGTERMs every plugin still running - the step between the grace
    //! period and the destructor's SIGKILL (PROTOCOL.md shutdown order).
    auto stop_all() -> void;

    //! Number of running plugins (for tests and status).
    auto plugin_count() const -> size_t;

    //! True once per completed handshake since the last call. The host's
    //! owner replays the session state when this fires, so a plugin that
    //! became ready after events went out still learns what exists.
    auto take_newly_ready() -> bool;

private:
    struct Plugin {
        PluginSpec spec;
        uint64_t id { 0 };
        PluginProcess process;
        bool ready { false };
        std::vector<std::string> subscriptions; //!< topic prefixes
        std::vector<std::string> publishes;     //!< declared broadcast topics

        // broadcast rate limiting (a dumb broker still meters the door)
        std::chrono::steady_clock::time_point broadcast_window {};
        uint32_t broadcasts_in_window { 0 };
        bool broadcast_limited { false }; //!< logged once per window
    };

    auto dispatch(Plugin& plugin, Message const& message) -> void;
    auto handle_notify(Plugin& plugin, Message const& message) -> void;
    //! The pane/buffer/grid/status notifies, once the handshake is done.
    auto handle_session_notify(Plugin& plugin, Message const& message) -> bool;
    auto handle_request(Plugin& plugin, Message const& message) -> void;
    auto handle_ready(Plugin& plugin, msgpack::Value const& params) -> void;
    auto handle_broadcast(Plugin& plugin, msgpack::Value const& params) -> void;
    //! Whether this broadcast fits the plugin's per-second allowance.
    auto within_broadcast_budget(Plugin& plugin) -> bool;

    // handle_request's methods, one each
    auto request_buffer_info(Plugin& plugin, Message const& message) -> void;
    auto request_buffer_lines(Plugin& plugin, Message const& message) -> void;
    auto request_buffer_text(Plugin& plugin, Message const& message) -> void;
    auto request_buffer_create(Plugin& plugin, Message const& message) -> void;
    auto request_buffer_splice(Plugin& plugin, Message const& message) -> void;
    auto request_buffer_splice_many(Plugin& plugin, Message const& message) -> void;
    auto request_mark_set(Plugin& plugin, Message const& message) -> void;
    auto request_mark_get(Plugin& plugin, Message const& message) -> void;
    auto request_mark_delete(Plugin& plugin, Message const& message) -> void;
    //! The discoverable topic registry: every ready plugin's `publishes`.
    auto request_topics_list(Plugin& plugin, Message const& message) -> void;
    auto request_pane_info(Plugin& plugin, Message const& message) -> void;
    //! The success response to `message`.
    auto respond(Plugin& plugin, Message const& message, msgpack::Value result) -> void;
    //! The failure response: `code` plus whatever `extra` fields.
    auto respond_failure(
        Plugin& plugin, Message const& message, std::string const& code,
        msgpack::Value extra = msgpack::Value::object({})
    ) -> void;
    //! A splice outcome as its response (ok / the matching error code).
    auto respond_splice_status(
        Plugin& plugin, Message const& message, SpliceStatus status, uint64_t new_version
    ) -> void;
    auto subscribed(Plugin const& plugin, std::string const& topic) -> bool;
    auto send_error(Plugin& plugin, std::string const& method, std::string const& code)
        -> void;
    auto reap(Plugin& plugin) -> void;
    //! Whether a dead plugin's spec earns another launch right now.
    auto should_restart(PluginSpec const& spec) -> bool;

    HostServices& _services;
    uint64_t _next_plugin_id { 1 };
    bool _shutting_down { false };
    bool _newly_ready { false };
    std::vector<std::unique_ptr<Plugin>> _plugins;
    //! Restart timestamps per plugin name, pruned to the restart window.
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>>
        _restarts;
};

}

#endif // SPICE_PLUGIN_PLUGINHOST_H

#ifndef SPICE_PLUGIN_PLUGINHOST_H
#define SPICE_PLUGIN_PLUGINHOST_H

#include "spice/plugin/HostServices.hpp"
#include "spice/plugin/Message.hpp"
#include "spice/plugin/PluginProcess.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spice::plugin {

//! The protocol version the core speaks. Only the major part is negotiated.
inline constexpr std::string_view protocol_version { "0.1" };

//! How a plugin is launched (from CONFIG.md's `[[plugin]]`).
struct PluginSpec {
    std::string name;
    std::vector<std::string> command; //!< argv
    bool pane_mode { false };         //!< "pane" vs "global"
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

    //! Number of running plugins (for tests and status).
    auto plugin_count() const -> size_t;

private:
    struct Plugin {
        PluginSpec spec;
        uint64_t id { 0 };
        PluginProcess process;
        bool ready { false };
        std::vector<std::string> subscriptions; //!< topic prefixes
    };

    auto dispatch(Plugin& plugin, Message const& message) -> void;
    auto handle_notify(Plugin& plugin, Message const& message) -> void;
    auto handle_request(Plugin& plugin, Message const& message) -> void;
    auto handle_ready(Plugin& plugin, msgpack::Value const& params) -> void;
    auto subscribed(Plugin const& plugin, std::string const& topic) -> bool;
    auto send_error(Plugin& plugin, std::string const& method, std::string const& code)
        -> void;
    auto reap(Plugin& plugin) -> void;

    HostServices& _services;
    uint64_t _next_plugin_id { 1 };
    std::vector<std::unique_ptr<Plugin>> _plugins;
};

}

#endif // SPICE_PLUGIN_PLUGINHOST_H

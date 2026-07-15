#ifndef SPICE_CONFIG_CONFIG_H
#define SPICE_CONFIG_CONFIG_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace spice::config {

//! One key binding: a config-style key name mapped to a command name.
//! (CONFIG.md binds (plugin, command) pairs; until plugins land, a bind
//! with a plugin field targets "<plugin>.<command>".)
struct Keybind {
    std::string key;     //!< "ctrl-g"
    std::string command; //!< "pane.float"

    auto operator==(Keybind const&) const -> bool = default;
};

enum class LogLevel : uint8_t { trace, debug, info, warn, error };

//! When a crashed plugin is restarted (CONFIG.md).
enum class RestartPolicy : uint8_t { never, on_crash, always };

//! One `[[plugin]]` entry: how to launch a plugin and how it behaves.
struct PluginEntry {
    std::string name;                 //!< unique; its topic/error namespace
    std::vector<std::string> command; //!< path + argv
    bool pane_mode { false };         //!< "pane" (per pane) vs "global"
    RestartPolicy restart { RestartPolicy::on_crash };
    uint32_t max_restarts { 3 };
    std::chrono::milliseconds restart_window { 60000 };

    auto operator==(PluginEntry const&) const -> bool = default;
};

//! Everything CONFIG.md makes configurable, plugins excepted for now,
//! with its defaults. Two files feed it: config.toml (user-owned, never
//! written by Spice) and keybinds.toml (Spice-owned, regenerated freely);
//! config.toml wins every conflict.
struct Config {
    // [keys]
    std::string master { "ctrl-space" }; //!< the prefix reaching Spice
    std::string palette_run { "return" };
    std::string palette_bind { "shift-return" };

    // [[plugin]]
    std::vector<PluginEntry> plugins;

    // [[keybind]]
    std::vector<Keybind> user_keybinds;  //!< from config.toml: never written
    std::vector<Keybind> state_keybinds; //!< from keybinds.toml: Spice-owned

    // [lifecycle] (consumed once plugins arrive; parsed and kept already)
    std::chrono::milliseconds shutdown_grace { 2000 };
    std::chrono::milliseconds sigterm_grace { 1000 };

    // [log] (consumed once the logger arrives; parsed and kept already)
    LogLevel log_level { LogLevel::info };
    std::string log_file; //!< defaulted to the state directory

    //! Problems found while loading, for surfacing to the user. A bad or
    //! missing file never prevents startup: defaults fill every hole.
    std::vector<std::string> warnings;
};

//! $XDG_CONFIG_HOME/spice/config.toml, falling back to ~/.config/spice/.
auto config_file_path() -> std::string;
//! $XDG_STATE_HOME/spice/keybinds.toml, falling back to ~/.local/state/spice/.
auto keybinds_file_path() -> std::string;

//! Loads both files (absent ones are simply defaults). State keybinds
//! whose key is already taken by config.toml - or by the Master key - are
//! dropped, per CONFIG.md's precedence rule.
auto load(std::string const& config_file, std::string const& keybinds_file) -> Config;
//! Same, from the standard paths.
auto load() -> Config;

//! Regenerates keybinds.toml (comments do not survive - it is machine
//! owned), creating parent directories as needed.
auto save_keybinds(std::string const& path, std::vector<Keybind> const& keybinds) -> bool;

}

#endif // SPICE_CONFIG_CONFIG_H

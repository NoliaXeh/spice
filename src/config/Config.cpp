#include "spice/config/Config.hpp"
#include "spice/config/KeyName.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <set>

#include <toml.hpp>

namespace {

using namespace spice::config;

//! $VARIABLE with a fallback under $HOME.
auto xdg_directory(char const* variable, std::string_view home_suffix) -> std::string {
    if (char const* value { std::getenv(variable) }; value != nullptr && *value != '\0') {
        return value;
    }
    char const* home { std::getenv("HOME") };
    return std::string(home != nullptr ? home : ".") + std::string(home_suffix);
}

//! "2s" / "1500ms" / bare milliseconds.
auto parse_duration(std::string const& text) -> std::optional<std::chrono::milliseconds> {
    size_t digits { 0 };
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        ++digits;
    }
    if (digits == 0) {
        return std::nullopt;
    }
    long const value { std::stol(text.substr(0, digits)) };
    std::string const unit { text.substr(digits) };
    if (unit == "s") {
        return std::chrono::milliseconds { value * 1000 };
    }
    if (unit == "ms" || unit.empty()) {
        return std::chrono::milliseconds { value };
    }
    return std::nullopt;
}

auto parse_log_level(std::string const& name) -> std::optional<LogLevel> {
    if (name == "trace") return LogLevel::trace;
    if (name == "debug") return LogLevel::debug;
    if (name == "info") return LogLevel::info;
    if (name == "warn") return LogLevel::warn;
    if (name == "error") return LogLevel::error;
    return std::nullopt;
}

//! A leading $VARIABLE in a path is expanded ("$XDG_STATE_HOME/spice/...").
auto expand_leading_variable(std::string const& path) -> std::string {
    if (path.empty() || path.front() != '$') {
        return path;
    }
    size_t const slash { path.find('/') };
    std::string const variable { path.substr(1, slash - 1) };
    char const* value { std::getenv(variable.c_str()) };
    if (value == nullptr) {
        return path;
    }
    return value + (slash == std::string::npos ? "" : path.substr(slash));
}

auto parse_restart(std::string const& name) -> std::optional<RestartPolicy> {
    if (name == "never") return RestartPolicy::never;
    if (name == "on-crash") return RestartPolicy::on_crash;
    if (name == "always") return RestartPolicy::always;
    return std::nullopt;
}

//! [[plugin]] entries from config.toml.
auto read_plugins(toml::table const& table, std::vector<std::string>& warnings)
    -> std::vector<PluginEntry> {
    std::vector<PluginEntry> plugins;
    auto const* array { table["plugin"].as_array() };
    if (array == nullptr) {
        return plugins;
    }
    for (auto const& element : *array) {
        auto const* entry { element.as_table() };
        if (entry == nullptr) {
            continue;
        }
        PluginEntry plugin;
        plugin.name = (*entry)["name"].value<std::string>().value_or("");

        if (auto const* command { (*entry)["command"].as_array() }) {
            for (auto const& arg : *command) {
                if (auto const value { arg.value<std::string>() }) {
                    plugin.command.push_back(expand_leading_variable(*value));
                }
            }
        }
        if (plugin.name.empty() || plugin.command.empty()) {
            warnings.push_back("plugin entry needs a `name` and a `command`");
            continue;
        }

        if (auto const mode { (*entry)["mode"].value<std::string>() }) {
            if (*mode != "pane" && *mode != "global") {
                warnings.push_back(std::format("unknown plugin mode '{}'", *mode));
            }
            plugin.pane_mode = *mode == "pane";
        }
        if (auto const restart { (*entry)["restart"].value<std::string>() }) {
            if (auto const parsed { parse_restart(*restart) }) {
                plugin.restart = *parsed;
            } else {
                warnings.push_back(std::format("unknown restart policy '{}'", *restart));
            }
        }
        if (auto const max { (*entry)["max_restarts"].value<int64_t>() }) {
            plugin.max_restarts = static_cast<uint32_t>(std::max<int64_t>(*max, 0));
        }
        if (auto const window { (*entry)["restart_window"].value<std::string>() }) {
            if (auto const parsed { parse_duration(*window) }) {
                plugin.restart_window = *parsed;
            }
        }
        plugins.push_back(std::move(plugin));
    }
    return plugins;
}

//! [[keybind]] entries from one parsed file.
auto read_keybinds(toml::table const& table, std::vector<std::string>& warnings)
    -> std::vector<Keybind> {
    std::vector<Keybind> keybinds;
    auto const* array { table["keybind"].as_array() };
    if (array == nullptr) {
        return keybinds;
    }
    for (auto const& element : *array) {
        auto const* bind { element.as_table() };
        if (bind == nullptr) {
            continue;
        }
        auto const key { (*bind)["key"].value<std::string>() };
        auto command { (*bind)["command"].value<std::string>().value_or("") };
        if (auto const plugin { (*bind)["plugin"].value<std::string>() }) {
            command = *plugin + "." + command; // plugin commands are namespaced
        }
        if (!key || command.empty()) {
            warnings.push_back("keybind entry needs both `key` and `command`");
            continue;
        }
        if (!parse_key(*key)) {
            warnings.push_back(std::format("unknown key name '{}' in keybind", *key));
            continue;
        }
        keybinds.push_back({ *key, command });
    }
    return keybinds;
}

//! Parses one TOML file into `config`; which sections are honored depends
//! on the file (config.toml gets everything, keybinds.toml only keybinds).
auto read_file(std::string const& path, bool full, Config& config) -> std::vector<Keybind> {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return {}; // no file, no problem: defaults stand
    }

    toml::table table;
    try {
        table = toml::parse_file(path);
    } catch (toml::parse_error const& error) {
        config.warnings.push_back(std::format(
            "{}: {} (using defaults)", path, std::string(error.description())
        ));
        return {};
    }

    if (full) {
        if (auto const value { table["keys"]["master"].value<std::string>() }) {
            if (parse_key(*value)) {
                config.master = *value;
            } else {
                config.warnings.push_back(std::format("unknown master key '{}'", *value));
            }
        }
        if (auto const value { table["keys"]["palette_run"].value<std::string>() }) {
            config.palette_run = *value;
        }
        if (auto const value { table["keys"]["palette_bind"].value<std::string>() }) {
            config.palette_bind = *value;
        }

        if (auto const value { table["editor"]["indent"].value<int64_t>() }) {
            if (*value >= 1 && *value <= 16) {
                config.indent = static_cast<uint32_t>(*value);
            } else {
                config.warnings.push_back(
                    std::format("editor indent {} is not between 1 and 16", *value)
                );
            }
        }

        auto const read_grace = [&](char const* name, std::chrono::milliseconds& out) {
            if (auto const value { table["lifecycle"][name].value<std::string>() }) {
                if (auto const parsed { parse_duration(*value) }) {
                    out = *parsed;
                } else {
                    config.warnings.push_back(std::format("bad duration '{}'", *value));
                }
            } else if (auto const raw { table["lifecycle"][name].value<int64_t>() }) {
                out = std::chrono::milliseconds { *raw };
            }
        };
        read_grace("shutdown_grace", config.shutdown_grace);
        read_grace("sigterm_grace", config.sigterm_grace);

        if (auto const value { table["log"]["level"].value<std::string>() }) {
            if (auto const level { parse_log_level(*value) }) {
                config.log_level = *level;
            } else {
                config.warnings.push_back(std::format("unknown log level '{}'", *value));
            }
        }
        if (auto const value { table["log"]["file"].value<std::string>() }) {
            config.log_file = expand_leading_variable(*value);
        }

        config.plugins = read_plugins(table, config.warnings);
    }

    return read_keybinds(table, config.warnings);
}

}

namespace spice::config {

auto config_file_path() -> std::string {
    return xdg_directory("XDG_CONFIG_HOME", "/.config") + "/spice/config.toml";
}

auto keybinds_file_path() -> std::string {
    return xdg_directory("XDG_STATE_HOME", "/.local/state") + "/spice/keybinds.toml";
}

auto load(std::string const& config_file, std::string const& keybinds_file) -> Config {
    Config config;
    config.log_file = xdg_directory("XDG_STATE_HOME", "/.local/state") + "/spice/spice.log";

    config.user_keybinds = read_file(config_file, true, config);
    auto state { read_file(keybinds_file, false, config) };

    // config.toml wins: drop state binds on keys the user (or Master) holds
    std::set<std::string> taken { parse_key(config.master).value_or("") };
    for (auto const& bind : config.user_keybinds) {
        taken.insert(*parse_key(bind.key)); // validated at read time
    }
    for (auto& bind : state) {
        if (taken.contains(*parse_key(bind.key))) {
            config.warnings.push_back(std::format(
                "keybind '{}' is taken by config.toml; dropped from keybinds.toml", bind.key
            ));
        } else {
            config.state_keybinds.push_back(std::move(bind));
        }
    }
    return config;
}

auto load() -> Config {
    return load(config_file_path(), keybinds_file_path());
}

auto save_keybinds(std::string const& path, std::vector<Keybind> const& keybinds) -> bool {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);

    std::ofstream file { path, std::ios::trunc };
    if (!file) {
        return false;
    }
    file << "# Generated by Spice. Do not edit; edit config.toml instead.\n";
    for (auto const& bind : keybinds) {
        file << "\n[[keybind]]\n";
        file << "key     = \"" << bind.key << "\"\n";
        file << "command = \"" << bind.command << "\"\n";
    }
    file.flush();
    return file.good();
}

}

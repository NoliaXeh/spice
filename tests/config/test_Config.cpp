#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "spice/config/Config.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <unistd.h>

using namespace spice::config;
namespace fs = std::filesystem;

namespace {

//! A throwaway directory for config/keybinds files, removed afterwards.
struct TempConfigs {
    fs::path root;

    TempConfigs() {
        root = fs::temp_directory_path() / std::format("spice-configtest-{}", ::getpid());
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempConfigs() { fs::remove_all(root); }

    auto write(std::string const& name, std::string const& content) -> std::string {
        auto const path { (root / name).string() };
        std::ofstream { path } << content;
        return path;
    }
    auto path(std::string const& name) const -> std::string {
        return (root / name).string();
    }
};

}

TEST_CASE("config::load() returns pure defaults when files are absent") {
    TempConfigs files;
    auto const config { load(files.path("none.toml"), files.path("nothing.toml")) };
    CHECK_EQ(config.master, "ctrl-space");
    CHECK_EQ(config.palette_run, "return");
    CHECK_EQ(config.palette_bind, "shift-return");
    CHECK(config.user_keybinds.empty());
    CHECK(config.state_keybinds.empty());
    CHECK_EQ(config.shutdown_grace.count(), 2000);
    CHECK_EQ(config.sigterm_grace.count(), 1000);
    CHECK_EQ(config.log_level, LogLevel::info);
    CHECK_FALSE(config.log_file.empty());
    CHECK(config.warnings.empty());
}

TEST_CASE("config::load() reads every section of config.toml") {
    TempConfigs files;
    auto const path { files.write("config.toml", R"(
[keys]
master = "ctrl-g"
palette_run = "return"
palette_bind = "ctrl-b"

[[keybind]]
key     = "ctrl-t"
command = "pane.split_horizontal"

[[keybind]]
key     = "f5"
plugin  = "files"
command = "open"

[lifecycle]
shutdown_grace = "3s"
sigterm_grace  = "250ms"

[log]
level = "debug"
file  = "/tmp/spice-test.log"
)") };
    auto const config { load(path, files.path("none.toml")) };
    CHECK_EQ(config.master, "ctrl-g");
    CHECK_EQ(config.palette_bind, "ctrl-b");
    REQUIRE_EQ(config.user_keybinds.size(), 2u);
    CHECK_EQ(config.user_keybinds[0], Keybind { "ctrl-t", "pane.split_horizontal" });
    CHECK_EQ(config.user_keybinds[1], Keybind { "f5", "files.open" }); // plugin namespaced
    CHECK_EQ(config.shutdown_grace.count(), 3000);
    CHECK_EQ(config.sigterm_grace.count(), 250);
    CHECK_EQ(config.log_level, LogLevel::debug);
    CHECK_EQ(config.log_file, "/tmp/spice-test.log");
    CHECK(config.warnings.empty());
}

TEST_CASE("config::load() reads plugin entries") {
    TempConfigs files;
    auto const path { files.write("config.toml", R"(
[[plugin]]
name    = "files"
command = ["spice-files"]
mode    = "pane"
restart = "on-crash"

[[plugin]]
name    = "lsp"
command = ["/usr/local/bin/spice-lsp", "--config", "lsp.toml"]
mode    = "global"
restart = "never"
max_restarts   = 5

[[plugin]]
command = ["missing-name"]
)") };
    auto const config { load(path, files.path("none.toml")) };
    REQUIRE_EQ(config.plugins.size(), 2u); // the nameless one is dropped
    CHECK_EQ(config.plugins[0].name, "files");
    CHECK_EQ(config.plugins[0].command, std::vector<std::string> { "spice-files" });
    CHECK(config.plugins[0].pane_mode);
    CHECK_EQ(config.plugins[0].restart, RestartPolicy::on_crash);
    CHECK_EQ(config.plugins[1].name, "lsp");
    CHECK_FALSE(config.plugins[1].pane_mode);
    CHECK_EQ(config.plugins[1].restart, RestartPolicy::never);
    CHECK_EQ(config.plugins[1].max_restarts, 5u);
    CHECK_EQ(config.plugins[1].command.size(), 3u);
    CHECK_EQ(config.warnings.size(), 1u); // the missing name
}

TEST_CASE("config::load() lets config.toml win over keybinds.toml") {
    TempConfigs files;
    auto const user { files.write("config.toml", R"(
[keys]
master = "ctrl-g"

[[keybind]]
key     = "ctrl-t"
command = "pane.close"
)") };
    auto const state { files.write("keybinds.toml", R"(
[[keybind]]
key     = "ctrl-t"
command = "pane.float"

[[keybind]]
key     = "ctrl-g"
command = "session.close"

[[keybind]]
key     = "ctrl-u"
command = "pane.dock"
)") };
    auto const config { load(user, state) };
    REQUIRE_EQ(config.state_keybinds.size(), 1u); // ctrl-t and master dropped
    CHECK_EQ(config.state_keybinds[0], Keybind { "ctrl-u", "pane.dock" });
    CHECK_EQ(config.warnings.size(), 2u);
}

TEST_CASE("config::load() warns on bad input instead of failing") {
    TempConfigs files;
    auto const path { files.write("config.toml", R"(
[keys]
master = "not-a-key"

[[keybind]]
key = "ctrl-q"

[[keybind]]
key = "who-knows"
command = "pane.close"
)") };
    auto const config { load(path, files.path("none.toml")) };
    CHECK_EQ(config.master, "ctrl-space"); // default kept
    CHECK(config.user_keybinds.empty());
    CHECK_EQ(config.warnings.size(), 3u);

    auto const broken { files.write("broken.toml", "this is [not] toml =") };
    auto const fallback { load(broken, files.path("none.toml")) };
    CHECK_EQ(fallback.master, "ctrl-space");
    CHECK_EQ(fallback.warnings.size(), 1u);
}

TEST_CASE("config::save_keybinds() round-trips through load") {
    TempConfigs files;
    std::vector<Keybind> const binds {
        { "ctrl-u", "pane.float" },
        { "f9", "buffer.list" },
    };
    auto const path { files.path("state/keybinds.toml") }; // dirs created
    REQUIRE(save_keybinds(path, binds));

    auto const config { load(files.path("none.toml"), path) };
    CHECK_EQ(config.state_keybinds, binds);
}

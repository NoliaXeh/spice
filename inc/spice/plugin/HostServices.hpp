#ifndef SPICE_PLUGIN_HOSTSERVICES_H
#define SPICE_PLUGIN_HOSTSERVICES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spice::plugin {

//! A command a plugin offers, as it appears in the palette.
struct PluginCommand {
    std::string name;
    std::string title;
    std::string description;
};

//! A buffer position as the protocol addresses it: a line and a byte offset
//! into that line's UTF-8. Ranges are half-open [start, end).
struct BufferPosition {
    uint64_t line { 0 };
    uint64_t column { 0 }; //!< byte offset into the line
};

struct BufferRange {
    BufferPosition start;
    BufferPosition end;
};

struct BufferInfo {
    uint64_t version { 0 };
    uint64_t line_count { 0 };
    bool editable { true };
    bool dirty { false };
    std::string name;
};

//! Why a splice was refused (PROTOCOL.md core error codes).
enum class SpliceStatus { ok, stale_version, capability_denied, no_such_id, bad_params };

//! What the core exposes to plugins, behind the wire protocol. The plugin
//! host translates protocol messages into these calls; the application
//! implements them against the live session. Everything is non-blocking
//! and authoritative on the core side (invariants 1 and 2).
class HostServices {
public:
    virtual ~HostServices() = default;

    // -- commands and keybinds --------------------------------------
    virtual auto register_commands(
        std::string const& plugin, std::vector<PluginCommand> const& commands
    ) -> void = 0;
    virtual auto unregister_commands(
        std::string const& plugin, std::vector<std::string> const& names
    ) -> void = 0;
    virtual auto set_keybind(
        std::string const& plugin, std::string const& command, std::string const& key
    ) -> void = 0;

    // -- status, palette, logging -----------------------------------
    virtual auto status_message(std::string const& plugin, std::string const& text)
        -> void = 0;
    virtual auto status_error(
        std::string const& plugin, std::string const& code, std::string const& message
    ) -> void = 0;
    virtual auto open_palette() -> void = 0;
    virtual auto log(
        std::string const& plugin, std::string const& level, std::string const& message
    ) -> void = 0;

    // -- panes ------------------------------------------------------
    virtual auto open_pane(
        std::string const& kind, std::optional<uint64_t> buffer, std::string const& split
    ) -> void = 0;
    virtual auto close_pane(uint64_t pane) -> void = 0;
    virtual auto focus_pane(uint64_t pane) -> void = 0;
    virtual auto float_pane(uint64_t pane) -> void = 0;
    virtual auto dock_pane(uint64_t pane) -> void = 0;
    virtual auto set_pane_buffer(uint64_t pane, uint64_t buffer) -> void = 0;

    // -- buffers ----------------------------------------------------
    //! The buffer's metadata, or nothing when there is no such buffer.
    virtual auto buffer_info(uint64_t buffer) -> std::optional<BufferInfo> = 0;
    //! Lines [start, end) with the version they were read at; nothing when
    //! there is no such buffer.
    virtual auto buffer_lines(uint64_t buffer, uint64_t start, uint64_t end)
        -> std::optional<std::pair<std::vector<std::string>, uint64_t>> = 0;
    //! Creates a buffer, returning its id.
    virtual auto create_buffer(bool editable, std::string const& name) -> uint64_t = 0;
    virtual auto kill_buffer(uint64_t buffer) -> void = 0;
    //! Replaces `range` with `text` if `version` is current. On success
    //! sets `new_version`; otherwise the status says why (and, for a stale
    //! version, `new_version` carries the current one).
    virtual auto splice(
        uint64_t buffer, BufferRange range, std::string const& text,
        uint64_t version, uint64_t& new_version
    ) -> SpliceStatus = 0;
};

}

#endif // SPICE_PLUGIN_HOSTSERVICES_H

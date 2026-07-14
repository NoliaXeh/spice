#ifndef SPICE_CONFIG_KEYNAME_H
#define SPICE_CONFIG_KEYNAME_H

#include <optional>
#include <string>

namespace spice::config {

//! Turns a config-file key name ("ctrl-space", "shift-return", "alt-p",
//! "ctrl-page-up", "f5", "g") into the event id the binding map keys on
//! ("C-' '", "S-enter", "M-'p'"...). Modifiers are ctrl / alt (or meta) /
//! shift in any order; the base is a single character or a named key.
//! Nothing for names that make no sense.
auto parse_key(std::string const& name) -> std::optional<std::string>;

//! The reverse: an event id back to its config-file name, for writing
//! keybinds.toml. Nothing for ids that have no name (mouse events...).
auto key_name(std::string const& event_id) -> std::optional<std::string>;

}

#endif // SPICE_CONFIG_KEYNAME_H

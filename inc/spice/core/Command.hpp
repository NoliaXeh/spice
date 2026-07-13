#ifndef SPICE_CORE_COMMAND_H
#define SPICE_CORE_COMMAND_H

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace spice::core {

//! Something the user can run from the command palette (or bind to a key).
//! Built-ins are registered by the core; plugins will register theirs over
//! the wire protocol later, name-spaced by their plugin name.
struct Command {
    std::string name;  //!< unique id, e.g. "pane.close"
    std::string title; //!< what the palette shows, e.g. "Close current pane"
    std::function<void()> action;
};

//! The commands currently offered. Registration order is kept; consumers
//! (the palette) sort for display themselves.
class CommandRegistry {
public:
    //! Rejects duplicate names - a name is an identity, not a label.
    auto add(Command command) -> bool;
    //! Removes a command (a restarting plugin re-registers its own).
    auto remove(std::string_view name) -> bool;

    auto commands() const -> std::vector<Command> const&;
    auto find(std::string_view name) const -> Command const*;

    //! Runs a command by name; false if there is no such command.
    auto run(std::string_view name) const -> bool;

private:
    std::vector<Command> _commands;
};

}

#endif // SPICE_CORE_COMMAND_H

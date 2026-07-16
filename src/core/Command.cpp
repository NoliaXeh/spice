#include "spice/core/Command.hpp"

#include <algorithm>
#include <utility>

namespace spice::core {

auto CommandRegistry::add(Command command) -> bool {
    if (find(command.name)) {
        return false;
    }
    _commands.push_back(std::move(command));
    return true;
}

auto CommandRegistry::remove(std::string_view name) -> bool {
    auto const removed {
        std::erase_if(_commands, [&](Command const& c) { return c.name == name; })
    };
    return removed > 0;
}

auto CommandRegistry::commands() const -> std::vector<Command> const& {
    return _commands;
}

auto CommandRegistry::find(std::string_view name) const -> OptRef<Command const> {
    auto const found {
        std::ranges::find_if(_commands, [&](Command const& c) { return c.name == name; })
    };
    if (found == _commands.end()) {
        return {};
    }
    return *found;
}

auto CommandRegistry::run(std::string_view name) const -> bool {
    auto const command { find(name) };
    if (!command || !command->action) {
        return false;
    }
    command->action();
    return true;
}

}

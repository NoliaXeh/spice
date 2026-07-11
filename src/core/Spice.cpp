#include "spice/core/Spice.hpp"

spice::core::Spice::Spice(std::string&& name)
    : _name { std::move(name) }
{}

auto spice::core::Spice::name() const -> std::string const& {
    return _name;
}
#include <print>

#include "spice/core/Spice.hpp"

using namespace spice;

int main() {
    auto sp { core::Spice("Spice") };
    std::println("Hello {}!", sp.name());
}
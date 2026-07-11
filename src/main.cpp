#include <print>

#include "spice/core/Spice.hpp"
#include "spice/core/TermInfo.hpp"

using namespace spice;

int main() {
    auto sp { core::Spice("Spice") };
    std::println("Hello {}!", sp.name());

    auto ti { core::TermInfo() };
    std::println("w={}, h={}, pid={}", ti.width(), ti.height(), ti.pid());
}
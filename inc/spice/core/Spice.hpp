#ifndef SPICE_CORE_SPICE_H
#define SPICE_CORE_SPICE_H

#include <string>
namespace spice::core {

class Spice {
public:
    Spice(std::string&& name);

    auto name() const -> std::string const&;

private:
    std::string _name;
};

}

#endif // SPICE_CORE_SPICE_H

#ifndef SPICE_CORE_TERMINFO_H
#define SPICE_CORE_TERMINFO_H

#include <cstdint>
#include <unistd.h>

namespace spice::core {
 
//! This gives information about current terminal
class TermInfo {
    public:
    auto width() const -> uint32_t;
    auto height() const -> uint32_t;
    
    auto pid() const -> pid_t;
};

}

#endif // SPICE_CORE_TERMINFO_H

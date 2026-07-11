#ifndef SPICE_CORE_TERMINFO_H
#define SPICE_CORE_TERMINFO_H

#include <cstdint>
#include <unistd.h>

namespace spice::core {
 
//! This gives information about current terminal
class TermInfo {
    public:
    auto width() -> uint32_t;
    auto height() -> uint32_t;
    
    auto pid() -> pid_t;
};

}

#endif // SPICE_CORE_TERMINFO_H

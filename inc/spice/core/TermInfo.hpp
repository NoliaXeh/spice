#ifndef SPICE_CORE_TERMINFO_H
#define SPICE_CORE_TERMINFO_H

#include <cstdint>
#include <unistd.h>

namespace spice::core {

//! Facts about the terminal we are running on, queried live from the
//! kernel - never cached, so a resize is reflected by the next call.
class TermInfo {
public:
    //! Terminal width in cells; 0 when there is no controlling terminal.
    auto width() const -> uint32_t;
    //! Terminal height in cells; 0 when there is no controlling terminal.
    auto height() const -> uint32_t;

    //! The current process id.
    auto pid() const -> pid_t;
};

}

#endif // SPICE_CORE_TERMINFO_H

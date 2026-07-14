#include "spice/core/TermInfo.hpp"

#include <sys/ioctl.h>

namespace {

auto window_size() -> winsize {
    winsize ws {};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    return ws;
}

}

auto spice::core::TermInfo::width() const -> uint32_t {
    return window_size().ws_col;
}

auto spice::core::TermInfo::height() const -> uint32_t {
    return window_size().ws_row;
}

auto spice::core::TermInfo::pid() const -> pid_t {
    return getpid();
}

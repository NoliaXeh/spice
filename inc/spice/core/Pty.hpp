#ifndef SPICE_CORE_PTY_H
#define SPICE_CORE_PTY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace spice::core {

//! A child process on a pseudo-terminal, POSIX only (posix_openpt et al).
//! The master side is non-blocking: read_output() drains what the child
//! wrote so far, write_input() sends bytes to it. The child is terminated
//! and reaped on destruction.
class Pty {
public:
    Pty() = default;
    ~Pty();

    Pty(Pty const&) = delete;
    auto operator=(Pty const&) -> Pty& = delete;
    Pty(Pty&& other) noexcept;
    auto operator=(Pty&& other) noexcept -> Pty&;

    //! Starts `argv` on a fresh pty of the given size. False on failure.
    auto spawn(std::vector<std::string> const& argv, uint32_t columns, uint32_t rows) -> bool;

    //! Still running? Updated by read_output() when the child hangs up.
    auto running() const -> bool;

    //! Drains everything the child has written so far (raw bytes,
    //! escape sequences included); empty when there is nothing.
    auto read_output() -> std::string;

    //! Sends bytes to the child's stdin.
    auto write_input(std::string_view bytes) -> bool;

    //! Propagates a new size to the pty (the child gets SIGWINCH).
    auto resize(uint32_t columns, uint32_t rows) -> void;

    //! SIGHUP then SIGKILL, and reaps the child.
    auto terminate() -> void;

private:
    int _fd { -1 };
    pid_t _pid { -1 };
    bool _running { false };
};

}

#endif // SPICE_CORE_PTY_H

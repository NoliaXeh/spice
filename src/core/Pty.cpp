#include "spice/core/Pty.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

auto window_size(uint32_t columns, uint32_t rows) -> winsize {
    winsize size {};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    return size;
}

}

namespace spice::core {

Pty::~Pty() {
    terminate();
}

Pty::Pty(Pty&& other) noexcept
    : _fd { std::exchange(other._fd, -1) }
    , _pid { std::exchange(other._pid, -1) }
    , _running { std::exchange(other._running, false) }
{}

auto Pty::operator=(Pty&& other) noexcept -> Pty& {
    if (this != &other) {
        terminate();
        _fd = std::exchange(other._fd, -1);
        _pid = std::exchange(other._pid, -1);
        _running = std::exchange(other._running, false);
    }
    return *this;
}

auto Pty::spawn(std::vector<std::string> const& argv, uint32_t columns, uint32_t rows) -> bool {
    if (_running || argv.empty()) {
        return false;
    }

    int const master { posix_openpt(O_RDWR | O_NOCTTY) };
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (master >= 0) close(master);
        return false;
    }
    char const* const slave_name { ptsname(master) };
    if (slave_name == nullptr) {
        close(master);
        return false;
    }
    winsize size { window_size(columns, rows) };
    ioctl(master, TIOCSWINSZ, &size);

    pid_t const pid { fork() };
    if (pid < 0) {
        close(master);
        return false;
    }

    if (pid == 0) { // child: new session, slave becomes the controlling tty
        setsid();
        int const slave { open(slave_name, O_RDWR) };
        if (slave < 0) {
            _exit(127);
        }
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) {
            close(slave);
        }
        close(master);

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (auto const& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    _fd = master;
    _pid = pid;
    _running = true;
    return true;
}

auto Pty::running() const -> bool {
    return _running;
}

auto Pty::read_output() -> std::string {
    std::string out;
    if (_fd < 0) {
        return out;
    }

    char buffer[4096];
    while (true) {
        ssize_t const count { read(_fd, buffer, sizeof buffer) };
        if (count > 0) {
            out.append(buffer, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // drained
        }
        // 0 or EIO: the child hung up
        waitpid(_pid, nullptr, WNOHANG);
        _running = false;
        break;
    }
    return out;
}

auto Pty::write_input(std::string_view bytes) -> bool {
    if (_fd < 0 || !_running) {
        return false;
    }
    return write(_fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
}

auto Pty::resize(uint32_t columns, uint32_t rows) -> void {
    if (_fd >= 0) {
        winsize size { window_size(columns, rows) };
        ioctl(_fd, TIOCSWINSZ, &size);
    }
}

auto Pty::terminate() -> void {
    if (_pid > 0) {
        kill(_pid, SIGHUP);
        kill(_pid, SIGKILL);
        waitpid(_pid, nullptr, 0);
        _pid = -1;
    }
    if (_fd >= 0) {
        close(_fd);
        _fd = -1;
    }
    _running = false;
}

}

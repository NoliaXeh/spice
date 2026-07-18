#include "spice/plugin/PluginProcess.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

//! A declared frame length past this is a broken or hostile stream: no
//! protocol message is remotely this large, and honoring it would let a
//! buggy plugin make the core buffer without bound.
constexpr size_t max_frame_bytes { 64UL << 20UL };

//! How much one read() pulls off a plugin pipe.
constexpr size_t read_chunk_bytes { 8192 };

auto set_nonblocking(int fd) -> void {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

//! Close-on-exec, so a pipe never leaks into later-spawned children. A
//! leaked write end would keep a dead plugin's stdout open in a sibling
//! and the core would never see EOF - crash detection must not depend on
//! which plugin spawned first.
auto set_cloexec(int fd) -> void {
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

}

namespace spice::plugin {

PluginProcess::~PluginProcess() {
    terminate();
}

auto PluginProcess::spawn(std::vector<std::string> const& argv) -> bool {
    if (_running || argv.empty()) {
        return false;
    }

    // a plugin dying mid-write must surface as EPIPE, not kill the core
    signal(SIGPIPE, SIG_IGN);

    std::array<int, 2> to_child {};   // core -> plugin stdin
    std::array<int, 2> from_child {}; // plugin stdout -> core
    std::array<int, 2> errs {};       // plugin stderr -> core
    if (pipe(to_child.data()) != 0) {
        return false;
    }
    if (pipe(from_child.data()) != 0) {
        close(to_child[0]); close(to_child[1]);
        return false;
    }
    if (pipe(errs.data()) != 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return false;
    }
    for (int const fd : { to_child[0], to_child[1], from_child[0], from_child[1],
                          errs[0], errs[1] }) {
        set_cloexec(fd);
    }

    pid_t const pid { fork() };
    if (pid < 0) {
        for (int const fd : { to_child[0], to_child[1], from_child[0], from_child[1],
                              errs[0], errs[1] }) {
            close(fd);
        }
        return false;
    }

    if (pid == 0) { // child: dup2 clears close-on-exec for 0/1/2; every
                    // other inherited pipe end closes itself at exec
        signal(SIGPIPE, SIG_DFL); // the plugin gets default signal behavior
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(errs[1], STDERR_FILENO);
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (auto const& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }

    close(to_child[0]);
    close(from_child[1]);
    close(errs[1]);
    _stdin = to_child[1];
    _stdout = from_child[0];
    _stderr = errs[0];
    set_nonblocking(_stdin);
    set_nonblocking(_stdout);
    set_nonblocking(_stderr);
    _pid = pid;
    _running = true;
    return true;
}

auto PluginProcess::running() const -> bool {
    return _running;
}

auto PluginProcess::exited_cleanly() const -> bool {
    return !_running && _reaped
        && WIFEXITED(_wait_status) && WEXITSTATUS(_wait_status) == 0;
}

auto PluginProcess::reap_child(int options) -> void {
    if (_pid <= 0 || _reaped) {
        return;
    }
    int status {};
    if (waitpid(_pid, &status, options) == _pid) {
        _wait_status = status;
        _reaped = true;
        _pid = -1; // never signal this (possibly recycled) pid again
    }
}

auto PluginProcess::send(Message const& message) -> void {
    if (!_running) {
        return;
    }
    _outgoing += encode_frame(message);
    flush_output();
}

auto PluginProcess::flush_output() -> void {
    if (_stdin < 0) {
        return;
    }
    while (!_outgoing.empty()) {
        ssize_t const written { write(_stdin, _outgoing.data(), _outgoing.size()) };
        if (written > 0) {
            _outgoing.erase(0, static_cast<size_t>(written));
        } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // the pipe is full; the rest waits for the next flush
        } else {
            break; // EPIPE and friends: the plugin is gone, poll() will notice
        }
    }
}

auto PluginProcess::drain_fd(int fd, std::string& into) -> bool {
    if (fd < 0) {
        return false;
    }
    std::array<char, read_chunk_bytes> buffer {};
    while (true) {
        ssize_t const count { read(fd, buffer.data(), buffer.size()) };
        if (count > 0) {
            into.append(buffer.data(), static_cast<size_t>(count));
        } else if (count == 0) {
            return false; // hangup
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true; // drained, still open
        } else {
            return false;
        }
    }
}

auto PluginProcess::poll() -> std::vector<Message> {
    std::vector<Message> messages;
    if (!_running) {
        return messages;
    }

    flush_output(); // make progress on anything backed up
    drain_fd(_stderr, _errors);

    bool alive { drain_fd(_stdout, _incoming) };
    if (alive) {
        reap_child(WNOHANG);
        alive = !_reaped;
    }
    if (!alive) {
        reap_child(WNOHANG);
        _running = false;
    }

    // a declared frame length past the cap can never complete: cut the
    // plugin off rather than buffering forever toward it
    if (_incoming.size() >= frame_header_bytes
        && frame_length(_incoming) > max_frame_bytes) {
        terminate();
        return messages;
    }

    while (auto message { take_frame(_incoming) }) {
        messages.push_back(std::move(*message));
    }
    return messages;
}

auto PluginProcess::take_stderr() -> std::string {
    return std::exchange(_errors, {});
}

auto PluginProcess::request_stop() -> void {
    if (_pid > 0) {
        kill(_pid, SIGTERM);
    }
}

auto PluginProcess::terminate() -> void {
    if (_pid > 0) {
        kill(_pid, SIGKILL);
        reap_child(0); // blocking: SIGKILL cannot be ignored
    }
    auto const close_fd = [](int& fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };
    close_fd(_stdin);
    close_fd(_stdout);
    close_fd(_stderr);
    _running = false;
    _incoming.clear();
    _outgoing.clear();
}

}

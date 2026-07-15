#include "spice/plugin/PluginProcess.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

auto set_nonblocking(int fd) -> void {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
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

    int to_child[2] {};   // core -> plugin stdin
    int from_child[2] {}; // plugin stdout -> core
    int errs[2] {};       // plugin stderr -> core
    if (pipe(to_child) != 0) {
        return false;
    }
    if (pipe(from_child) != 0) {
        close(to_child[0]); close(to_child[1]);
        return false;
    }
    if (pipe(errs) != 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return false;
    }

    pid_t const pid { fork() };
    if (pid < 0) {
        for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
                        errs[0], errs[1] }) {
            close(fd);
        }
        return false;
    }

    if (pid == 0) { // child
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(errs[1], STDERR_FILENO);
        for (int fd : { to_child[0], to_child[1], from_child[0], from_child[1],
                        errs[0], errs[1] }) {
            close(fd);
        }
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
    char buffer[8192];
    while (true) {
        ssize_t const count { read(fd, buffer, sizeof buffer) };
        if (count > 0) {
            into.append(buffer, static_cast<size_t>(count));
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

    if (!drain_fd(_stdout, _incoming)) {
        waitpid(_pid, nullptr, WNOHANG);
        _running = false;
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
        waitpid(_pid, nullptr, 0);
        _pid = -1;
    }
    for (int* fd : { &_stdin, &_stdout, &_stderr }) {
        if (*fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
    _running = false;
}

}

#include "spice/core/EventReader.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <poll.h>
#include <unistd.h>

namespace {

//! How long a lone ESC may sit unresolved before it is reported as the
//! escape key rather than the start of a sequence (see README on why bare
//! ESC needs a timeout at all).
constexpr int escape_timeout_ms { 25 };

// 1002: report presses, releases and drag motion. 1006: SGR encoding.
constexpr char mouse_on[] { "\x1b[?1002h\x1b[?1006h" };
constexpr char mouse_off[] { "\x1b[?1002l\x1b[?1006l" };

//! Set by the SIGWINCH handler, consumed by poll(). The handler does
//! nothing else - everything async-signal-unsafe waits for the poll loop.
volatile std::sig_atomic_t window_resized { 0 };

auto on_sigwinch(int) -> void {
    window_resized = 1;
}

}

namespace spice::core {

EventReader::EventReader() {
    if (isatty(STDIN_FILENO) != 1 || tcgetattr(STDIN_FILENO, &_saved) != 0) {
        return;
    }
    termios raw { _saved };
    cfmakeraw(&raw);
    _raw = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
    if (_raw) {
        (void)!write(STDOUT_FILENO, mouse_on, sizeof(mouse_on) - 1);

        struct sigaction action {};
        action.sa_handler = on_sigwinch; // no SA_RESTART: poll must wake on resize
        sigemptyset(&action.sa_mask);
        sigaction(SIGWINCH, &action, &_saved_sigwinch);
    }
}

EventReader::~EventReader() {
    if (_raw) {
        sigaction(SIGWINCH, &_saved_sigwinch, nullptr);
        (void)!write(STDOUT_FILENO, mouse_off, sizeof(mouse_off) - 1);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved);
    }
}

auto EventReader::pop() -> std::optional<Event> {
    if (_queue.empty()) {
        return std::nullopt;
    }
    Event event { _queue.front() };
    _queue.pop_front();
    return event;
}

auto EventReader::poll(int timeout_ms) -> std::optional<Event> {
    if (auto event { pop() }) {
        return event;
    }
    if (!_raw) {
        return std::nullopt;
    }

    while (true) {
        if (window_resized != 0) {
            window_resized = 0;
            return Event { .type = EventType::resize, .key = {}, .mouse = {} };
        }

        // While a sequence is half-read, wait only the escape timeout so a
        // lone ESC resolves promptly instead of blocking on the caller's.
        int wait { timeout_ms };
        if (_parser.pending() && (wait < 0 || wait > escape_timeout_ms)) {
            wait = escape_timeout_ms;
        }

        pollfd pfd { .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int const ready { ::poll(&pfd, 1, wait) };

        if (ready > 0) {
            char buffer[512];
            ssize_t const count { read(STDIN_FILENO, buffer, sizeof buffer) };
            if (count <= 0) {
                return std::nullopt;
            }
            for (auto const& event : _parser.feed({ buffer, static_cast<size_t>(count) })) {
                _queue.push_back(event);
            }
            if (auto event { pop() }) {
                return event;
            }
            continue; // sequence split across reads: keep collecting
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue; // a signal (SIGWINCH) woke us: re-check the flag
            }
            return std::nullopt;
        }

        // timed out
        if (_parser.pending()) {
            for (auto const& event : _parser.flush()) {
                _queue.push_back(event);
            }
            if (auto event { pop() }) {
                return event;
            }
            continue; // dropped an unfinishable sequence; wait out the caller's timeout
        }
        return std::nullopt;
    }
}

}

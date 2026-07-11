#ifndef SPICE_CORE_EVENTREADER_H
#define SPICE_CORE_EVENTREADER_H

#include "spice/core/Event.hpp"
#include "spice/core/EventParser.hpp"
#include <deque>
#include <optional>
#include <termios.h>

namespace spice::core {

//! Owns the input side of the terminal: switches stdin to raw mode, enables
//! SGR mouse reporting, and turns the byte stream into Events through an
//! EventParser. The terminal is restored on destruction. Safe to construct
//! without a tty; poll() then simply never returns an event.
class EventReader {
public:
    EventReader();
    ~EventReader();

    EventReader(EventReader const&) = delete;
    auto operator=(EventReader const&) -> EventReader& = delete;

    //! Next event, waiting at most timeout_ms for one to arrive
    //! (0 = don't wait, negative = wait forever).
    auto poll(int timeout_ms) -> std::optional<Event>;

private:
    auto pop() -> std::optional<Event>;

    EventParser _parser;
    std::deque<Event> _queue;
    termios _saved {};
    bool _raw { false };
};

}

#endif // SPICE_CORE_EVENTREADER_H

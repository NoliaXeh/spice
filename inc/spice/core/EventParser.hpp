#ifndef SPICE_CORE_EVENTPARSER_H
#define SPICE_CORE_EVENTPARSER_H

#include "spice/core/Event.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spice::core {

//! Incremental terminal-input parser: turns the raw bytes a terminal sends
//! (utf-8 text, control characters, CSI/SS3 escape sequences, SGR mouse
//! reports) into Events. A sequence may arrive split across feed() calls;
//! the parser keeps the partial bytes and finishes it on the next feed.
class EventParser {
public:
    //! Parse the next chunk of bytes; returns the events they complete.
    auto feed(std::string_view bytes) -> std::vector<Event>;

    //! True when bytes are buffered waiting for the rest of a sequence.
    auto pending() const -> bool;

    //! Resolve buffered bytes when no more input is coming. A lone ESC can
    //! only be told apart from the start of a sequence by such a timeout
    //! (see README); it resolves to Key::escape here. An unfinished
    //! CSI/SS3/utf-8 sequence is unrecoverable and is dropped.
    auto flush() -> std::vector<Event>;

private:
    enum class State : uint8_t { ground, escape, csi, ss3, utf8 };

    auto consume(char byte, std::vector<Event>& events) -> void;
    auto handle_ground(char byte, bool alt, std::vector<Event>& events) -> void;
    auto parse_csi(char final, std::vector<Event>& events) -> void;
    auto parse_ss3(char byte, std::vector<Event>& events) -> void;
    auto parse_mouse(char final, std::vector<Event>& events) -> void;

    State _state { State::ground };
    std::string _pending; //!< bytes of the sequence being collected
    bool _alt { false };  //!< ESC- (alt) prefix applies to the utf-8 char being collected
};

}

#endif // SPICE_CORE_EVENTPARSER_H

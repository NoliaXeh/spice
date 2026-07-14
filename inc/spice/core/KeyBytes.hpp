#ifndef SPICE_CORE_KEYBYTES_H
#define SPICE_CORE_KEYBYTES_H

#include "spice/core/Event.hpp"
#include <string>

namespace spice::core {

//! The bytes a terminal would send for this key - EventParser's inverse,
//! used to forward keystrokes into a PTY pane's child process. Empty when
//! the key has no terminal encoding here.
auto key_to_bytes(KeyEvent const& key) -> std::string;

}

#endif // SPICE_CORE_KEYBYTES_H

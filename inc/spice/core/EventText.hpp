#ifndef SPICE_CORE_EVENTTEXT_H
#define SPICE_CORE_EVENTTEXT_H

#include "spice/core/Event.hpp"
#include <string>

namespace spice::core {

//! The position-independent identity of an event: "C-'w'", "S-up",
//! "press left", "resize"... Keybinding maps use it as their key, so two
//! events that should trigger the same binding produce the same id.
auto event_id(Event const& event) -> std::string;

//! A short human-readable line for an event, id plus position for mouse
//! events: "key C-'w'", "mouse press left 4:20". Suited for event logs.
auto describe(Event const& event) -> std::string;

}

#endif // SPICE_CORE_EVENTTEXT_H

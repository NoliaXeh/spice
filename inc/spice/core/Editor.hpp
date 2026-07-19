#ifndef SPICE_CORE_EDITOR_H
#define SPICE_CORE_EDITOR_H

#include "spice/core/Event.hpp"
#include "spice/core/Pane.hpp"
#include <cstdint>
#include <string>

namespace spice::core {

//! The core text-editing engine (README: editing lives in the core, not in
//! plugins). Applies one key to a pane viewing an editable buffer:
//!
//! - characters insert, ENTER splits (the new line inheriting the old
//!   one's leading spaces), BACKSPACE/DELETE remove (joining lines at the
//!   edges), all through the capability-checked Buffer API;
//! - TAB indents - `indent_width` spaces at the cursor, or every line a
//!   selection touches - and SHIFT-TAB dedents them;
//! - arrows, HOME/END and PAGE-UP/DOWN move the cursor (`page_rows` is the
//!   page jump, usually the pane's content height);
//! - SHIFT + movement extends a selection, plain movement drops it, ESCAPE
//!   clears it; typing and ENTER replace the selection, BACKSPACE/DELETE
//!   remove it, SHIFT-DELETE cuts it into `clipboard`;
//! - on a read-only pane every mutation is rejected, while movement and
//!   selection keep working.
//!
//! Returns true when anything changed (buffer, cursor or selection).
auto apply_editing_key(
    Pane& pane, KeyEvent const& key, uint32_t page_rows, std::string& clipboard,
    uint32_t indent_width = 4
) -> bool;

}

#endif // SPICE_CORE_EDITOR_H

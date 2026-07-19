# TODO — the road to a polished Spice

What separates the project from "complete" as of today. Checked items are
done; each entry says why it matters, not just what it is.

## Daily-driver gaps (a user hits these in the first hour)

- [x] **Mouse wheel scrolling.** Scrolls the pane under the pointer, three
      lines per notch, without moving its cursor; the view stops chasing
      the cursor until the cursor next moves.
- [x] **Unsaved-changes guard.** The first quit request (ctrl-w, or
      closing the last pane) lists the dirty buffers in the log; quitting
      again discards them. Saving disarms the warning.
- [x] **In-buffer search.** `edit.find` prompts and selects the next
      match, wrapping; `edit.find_next` (F3 by default) repeats it.
- [x] **Buffer switcher.** `buffer.switch` opens a picker over every
      session buffer (dirty ones marked `*`) and repoints the focused
      pane. PTY panes keep their terminal.
- [x] **Configurable indent.** `[editor] indent` in config.toml (1..16,
      warned and defaulted otherwise).

## Project hygiene (what a contributor expects to find)

- [x] **Continuous integration.** `.github/workflows/ci.yml`: a plain
      build+ctest job and a sanitizer job, on every push and PR.
- [x] **Sanitizer build.** `-DSPICE_SANITIZE=ON` wires ASan + UBSan into
      everything. (Compiles everywhere; linking needs the `libasan` /
      `libubsan` runtimes, absent on this machine - CI has them.)
- [x] **.clang-format.** Written to match the codebase (4-space indent,
      attached braces, east const, spaced braced init, 95 columns).
- [x] **Install target.** `cmake --install` places the binary in `bin/`,
      the reference plugins in `share/spice/plugins/`, the docs in
      `share/doc/spice/`.
- [ ] **License.** The repository has no LICENSE file, which legally means
      "all rights reserved" and blocks any reuse. *Owner's call — picking
      a license is a legal decision, not a code change; MIT or GPL-3.0 are
      the usual candidates for this kind of tool.*

## Deliberately not on the list

Protocol-level work (grid `ul_color` rendering, command progress,
per-pane plugin instances) is tracked in PLUGIN.md's "What is not built
yet" table; marks/cancellation/broadcast policy in PROTOCOL.md. This file
is about the editor and the repository.

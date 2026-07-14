# Spice internals

How the code is actually built, as of today. The [README](README.md) describes the design and
where the project is going; [BUILD.md](BUILD.md) describes how to build it; this file describes
what exists: the classes, how they relate, and how the terminal is driven at the syscall and
escape-sequence level.

The code lives in two modules (headers in `inc/spice/<module>/`, sources in `src/<module>/`,
tests in `tests/<module>/`): `core`, everything terminal/session/editing, and `config`, the
configuration system - plus the executable's `src/main.cpp`.

## The one-glance map

There is no inheritance anywhere - the "hierarchy" is composition. Arrows mean "uses":

```
                              main.cpp
                                 │
        ┌──────────┬─────────────┼─────────────┬──────────┬─────────┐
        ▼          ▼             ▼             ▼          ▼         ▼
   EventReader   Spice (session) Grid       TermInfo    Theme   CommandRegistry
        │          │              ▲            │                    ▲
        ▼          │              │       (ioctl, getpid)           │
   EventParser     ├─ Buffer      ├─ Color              Palette ────┘
        │          ├─ Pane ───────┤  (+ StyleFlags)     (draws into Grid,
        ▼          │   (draws into Grid)                 picks command names)
      Event        ├─ Layout
   (Key, Mouse*)   │   (split tree + floats)
                   └─ Pty (+ PtyFilter)
                       (child process on a pseudo-terminal)

   shared value types: Position, Rectangle, Color, Utf8 helpers
```

Two independent halves meet in `main.cpp`:

- **input**: terminal bytes → `EventReader` → `EventParser` → `Event`
- **output**: `Buffer` content → `Pane::draw` → `Grid` cells → escape sequences → terminal

## Value types

| type | file | contents |
| --- | --- | --- |
| `Position` | Position.hpp | `line`, `column`, `layer` (all `uint32_t`). Used for cells on screen, cells in a grid, and character positions in a buffer. `layer` is only meaningful for floats; most code ignores it. |
| `Rectangle` | Rectangle.hpp | top-left `Position` + `width`/`height`, plus `contains(Position)`. Used for pane areas, damage regions, the screen. |
| `StyleFlags` | Color.hpp | one bit each: bold, italic, underline, strikethrough, blinking, selected. |
| `Color` | Color.hpp | `r`,`g`,`b` bytes + `StyleFlags`. A "color" in Spice always carries its style. `colors::` holds constexpr constants for the usual colors. |

All of these have defaulted `operator==` so they can be compared (the renderer relies on
comparing `Color`s to skip redundant escape codes).

`Utf8.hpp` holds three constexpr helpers used everywhere text is addressed:

- `utf8_length(lead)` - byte length of the character starting at a lead byte (malformed bytes
  count as length 1 so loops always make progress),
- `utf8_offset(text, index)` - byte offset of the *index*-th character,
- `utf8_count(text)` - characters in a string.

The policy throughout: **columns are UTF-8 characters, never bytes.** Strings store UTF-8;
`utf8_offset` converts character columns to byte offsets at the edges.

## Terminal interaction

All terminal I/O is plain POSIX - no curses, no terminfo database, no external library.
Escape sequences are hardcoded to the de-facto xterm standard.

### C / POSIX usage

| API | header | used by | purpose |
| --- | --- | --- | --- |
| `ioctl(TIOCGWINSZ)` | `<sys/ioctl.h>` | TermInfo | terminal width/height in cells; returns 0×0 when stdout is not a tty |
| `getpid()` | `<unistd.h>` | TermInfo | own pid |
| `isatty()`, `tcgetattr()`, `tcsetattr()`, `cfmakeraw()` | `<termios.h>`, `<unistd.h>` | EventReader | switch stdin to raw mode (no echo, no line buffering, no signal keys), restore on destruction |
| `sigaction(SIGWINCH)` | `<csignal>` | EventReader | terminal resize notification; the handler only sets a `sig_atomic_t` flag, the poll loop turns it into an `EventType::resize` (no `SA_RESTART`, so `poll` wakes with `EINTR`) |
| `poll()` | `<poll.h>` | EventReader | wait for stdin bytes with a timeout |
| `read()`, `write()` | `<unistd.h>` | EventReader | raw byte input; unbuffered writes of the mouse on/off sequences |
| `fwrite()`/stdio | `<cstdio>` | Grid | one buffered write per rendered frame/rect |
| `posix_openpt()`, `grantpt()`, `unlockpt()`, `ptsname()` | `<cstdlib>`, `<fcntl.h>` | Pty | allocate the pseudo-terminal pair for a PTY pane |
| `fork()`, `setsid()`, `dup2()`, `execvp()` | `<unistd.h>` | Pty | start the child as a session leader with the slave as its controlling tty and stdio |
| `kill()`, `waitpid()` | `<csignal>`, `<sys/wait.h>` | Pty | SIGHUP+SIGKILL on terminate and reaping; the kernel HUPs the foreground job when the leader dies |

### Escape sequences emitted

| sequence | meaning | emitted by |
| --- | --- | --- |
| `ESC[?1049h` / `l` | enter/leave the alternate screen | main.cpp |
| `ESC[?1002h` / `l` | mouse reporting: presses, releases, drag motion | EventReader |
| `ESC[?1006h` / `l` | SGR mouse encoding (the parseable, > 223-column-safe one) | EventReader |
| `ESC[{row};{col}H` | cursor position (CUP), 1-based | Grid (per rendered row), main.cpp (parking the cursor) |
| `ESC[0;{styles};38;2;r;g;b;48;2;r;g;bm` | one combined SGR: reset, style flags (1 bold, 3 italic, 4 underline, 9 strike, 5 blink, 7 reverse), truecolor foreground and background | Grid |
| `ESC[0m` | reset at the end of every frame | Grid |

### Escape sequences parsed (input)

| bytes | parsed as |
| --- | --- |
| printable ASCII / UTF-8 sequence | `Key::character` with the text |
| `0x01..0x1a` control bytes | ctrl-letter (`0x11` → ctrl-q...); `\r`/`\n` enter, `\t` tab, `0x7f`/`0x08` backspace, `0x00` ctrl-space |
| `ESC [ params final` (CSI) | arrows `A B C D`, home/end `H F`, backtab `Z`, F1-F4 `P Q R S`, tilde keys `1~..6~` (home/insert/delete/end/page) and `11~..24~` (F1-F12); modifier parameter `1;m` decoded as 1+bitmask(shift=1, alt=2, ctrl=4) |
| `ESC O final` (SS3) | application-mode arrows and F1-F4 |
| `ESC char` | alt-char |
| lone `ESC` | `Key::escape`, but **only after a timeout** (see below) |
| `ESC [ < btn;col;row M/m` | SGR mouse report: button (0 left, 1 middle, 2 right, 64/65 wheel), +32 motion, mods (+4 shift, +8 alt, +16 ctrl), `M` press / `m` release; 1-based coordinates converted to 0-based |

## Input path: EventReader → EventParser → Event

`Event` (Event.hpp) is a tagged struct: `EventType type` says whether `key` (a `KeyEvent`: `Key`
enum + `Modifiers` + UTF-8 `text`) or `mouse` (a `MouseEvent`: `MouseAction` + `MouseButton` +
`Modifiers` + `Position`) is meaningful - or `resize`, which carries nothing (the new size is
re-queried from TermInfo). Everything is enums and plain data - no virtuals.

`EventParser` (EventParser.cpp) is an incremental state machine
(`ground / escape / csi / ss3 / utf8`) fed raw bytes via `feed()`. Sequences split across
reads are held in `_pending` and finished by the next `feed()`. Unknown sequences are dropped
without desyncing the stream. It does no I/O at all, which is what makes it unit-testable.

The bare-ESC problem (README: escape sequences are themselves ESC-prefixed) is solved by
`pending()`/`flush()`: the parser never guesses. `EventReader` owns the timing: while a
sequence is half-read it polls with a short 25 ms timeout instead of the caller's, and on
timeout calls `flush()`, which resolves a lone ESC to `Key::escape` and discards unfinishable
fragments.

Two small companions round the event types out: `EventText.hpp` names events - `event_id()`
gives the position-independent identity string binding maps key on ("C-'w'", "S-up",
"press left"), `describe()` the human-readable log line - and `KeyBytes.hpp` goes the other
way, `key_to_bytes()` turning a KeyEvent back into the bytes a terminal would send
(EventParser's inverse), which is how keystrokes are forwarded into PTY panes.

`EventReader` (EventReader.cpp) is the only class touching stdin state. Constructor: if stdin
is a tty, save termios, `cfmakeraw`, enable mouse reporting. Destructor restores both, so the
shell gets its terminal back even on quit. `poll(timeout_ms)` loops: poll stdin → read up to
512 bytes → feed the parser → queue events (`std::deque`) → return one. Without a tty it is
inert (poll returns nothing), which keeps tests from hanging.

## Output path: Grid, Theme, rendering

`Grid` (Grid.cpp) is the screen model: a 2-D buffer of cells, each holding a UTF-8 character
plus a foreground `Color` and a background `Color`. Text is stored as one `std::string` per
line (character widths vary); colors as two flat row-major `std::vector<Color>`. Cell
accessors: `char_at`/`set_text`, `style_at`/`set_style`, `background_at`/`set_background` -
all bounds-checked, all addressing columns in characters.

Rendering is one routine, `render_rect(terminfo, position, rect)`; `render()` (whole grid) and
`render_cell()` (1×1) are thin wrappers over it. Per row it emits one CUP, then walks the
line's UTF-8 incrementally (never re-scanning from column 0). The whole frame is built in a
`std::string` and flushed with a single `fwrite`. Two things keep it fast:

- **SGR coalescing** - a color sequence is emitted only when fg/bg/style differ from the
  previous cell; long runs of same-colored cells cost their characters only.
- **damage-based repaint** - callers repaint only the rectangle that changed. A keystroke is
  one pane's rect (~a few hundred bytes), not a 200×50 frame (~400 KB). Measured: ~850×
  faster on typing than the naive full-redraw version.

Cropping: anything falling outside `terminfo`'s current size is silently clipped, so rendering
into a shrunken terminal cannot write garbage.

`Theme` (Theme.cpp) maps a `Usage` enum (text, background, selection, cursor, border,
border_focused, error, warning, info) to a `Color`, `std::array`-backed. Drawing code never
hardcodes a color; it asks the theme, so a palette change recolors everything.

`TermInfo` (TermInfo.cpp) is the trivial "ask the kernel" class: width/height via
`TIOCGWINSZ` (0×0 without a tty), pid via `getpid()`. It is queried, never cached: when a
`resize` event arrives (SIGWINCH via EventReader), main re-queries it, rebuilds the Grid at
the new size, re-lays-out the session and repaints a full frame.

## The pane system

Design straight from the README: buffers and panes are decoupled.

**`Buffer`** (Buffer.cpp) - content, no view state. `std::vector<std::string>` of UTF-8 lines,
never empty (always ≥ 1 line). Its `BufferCapability` gates mutation:

- `editable`: `insert` (newline-free text at a position), `erase` (one character; at
  end-of-line it joins the next line - that one rule is what makes backspace/delete behave),
  `split_line` (enter).
- `append_only`: those three all return false; only `append` (which splits on `\n`) grows the
  buffer. This is the PTY-scrollback model.

A buffer can be tied to a file: `path()`/`set_path()`, and a `dirty()` flag set by every
mutation (edits, undo/redo, appends) and cleared by `mark_saved()` - panes show it as a `*`
after the title of editable buffers. The disk side lives in FileIo.hpp (`read_file` /
`write_file`, `std::fstream`-based; write joins lines with a trailing POSIX newline, read
strips it) - free functions because file IO ultimately belongs to the Files plugin (README),
and these will become its backend.

The editing engine itself is `Editor.hpp`: `apply_editing_key()` maps one key onto a pane -
characters insert (replacing any selection), ENTER splits, BACKSPACE/DELETE remove or join,
movement keys travel (SHIFT extends the selection, ESCAPE clears it, SHIFT-DELETE cuts) - all
through the capability-checked Buffer API. It lives in core because the README does ("editing
lives in the core, not in plugins") and so it is unit-tested like everything else.

Buffers own their undo history (the README puts undo in the core, and history belongs to the
content, not the view - two panes sharing a buffer share its history). Every successful edit
records an invertible `Edit` (kind + position + text); `undo()`/`redo()` replay inverses off
two stacks, returning where the change happened so the caller can place its cursor. Runs are
coalesced - typing forward, backspacing, or deleting at one spot each merge into a single
edit, so one undo removes the whole run. A new edit clears the redo stack; `append` records
nothing (arriving content is not an edit); history is capped at 1000 edits.

**`Pane`** (Pane.cpp) - a view: `PaneType` (edit / grid / pty), a `shared_ptr<Buffer>` (shared: two panes can view one buffer; a
buffer with no pane just lives on), a cursor and a scroll offset - the only view-specific
state. `draw(grid, area, focused, theme)` paints the pane chrome: a **title bar** across the
top (dark text on a light background, brighter and bold when focused) carrying a pane-type
dot (green edit, amber pty, blue grid), the buffer's name and two red buttons - `F`
floats/docks the pane, `x` closes it (`float_button`/`close_button` expose their cells for
hit-testing; panes narrower than 12 cells show a bare bar) - then side borders with a
**scrollbar thumb** once content overflows the view, a rounded `╰─╯` bottom, and the visible
slice of the buffer, the (focused) cursor's line on a subtly lifted background. Floating
panes and the palette call `drop_shadow` (Grid.hpp), which would darken the cells one row
below / one column right in place - currently disabled at its source pending a nicer look.
Edit panes auto-scroll to keep
the cursor visible; grid/pty panes keep whatever scroll they were given (`scroll_to_bottom`
for scrollback-style following). `position_from_screen` maps a click to a buffer position;
`cursor_screen_position` maps the cursor back to a screen cell. A pane can be **read-only**
(`set_read_only`, shown as `[ro]` in the title): mutations through it are rejected by the
editing engine and the clipboard commands while cursor, selection and copying keep working.
It is a view property, distinct from the buffer's capability - the Welcome pane is read-only
over an editable buffer, and the event log and the list floats are flagged too.

**`Layout`** (Layout.cpp) - where panes are, knowing only pane ids. Two structures, exactly as
the README prescribes:

- the **split tree**: nodes are either a leaf (pane id) or a split (orientation + two
  children). `tiles(screen)` partitions the screen recursively, each split halving its
  rectangle exactly - tiles always cover the screen with no gaps. `insert` splits a leaf in
  two; `remove` grafts the sibling into the parent's place.
- the **float list**: `(pane, Rectangle)` pairs; z-order is list rank, last on top.

On top of those: `pane_at` (hit test, topmost float first, then tiles), `neighbor`
(directional focus: probe one cell beyond the pane's edge, level with its center, and see
which tile is there), `move_float` (reposition a float), `raise_float` (to the top of the
z-order - `Spice::focus` calls it, so a focused float always sits above the other floats),
and `swap` (exchange the places of any two panes - two tiles, two floats, or a tile and a
float, which trades tiled for floating).

**`Pty` / `PtyFilter`** (Pty.cpp) - the process side of PTY panes. `Pty` runs a child on a
pseudo-terminal (see the POSIX table above), master side non-blocking: `read_output()` drains,
`write_input()` forwards, `resize()` re-sizes (the child gets SIGWINCH), `terminate()`
SIGHUPs/SIGKILLs and reaps. `PtyFilter` reduces the child's byte stream to appendable
scrollback: escape sequences (CSI/OSC/ESC-prefixed) stripped, control bytes dropped, tabs to
spaces - deliberately *not* terminal emulation; full-screen programs will look wrong until a
real emulator lands. The session owns a `Pty`+`PtyFilter` per PTY pane: `open_pty_pane(argv)`
spawns sized to the pane's content, `pump_ptys()` (called every main-loop turn) appends
filtered output to the append-only scrollback buffer and reports which panes changed, and
closing the pane kills the process while the scrollback buffer survives.

**`Spice`** (Spice.cpp) - the session tying it together: owns the buffers
(`vector<shared_ptr<Buffer>>`), the panes (`map<id, Pane>`), the `Layout`, and the focus.
`open_welcome_pane()` creates the startup pane; `open_pane` splits the focused tile (wide
tiles split side-by-side, tall ones stacked - "wide" meaning width ≥ 2×height, because
terminal cells are roughly twice as tall as wide); `close_focused_pane()` erases the pane but
never the buffer; `pane_count() == 0` is the program-should-end signal. `draw()` paints
tiles first, then floats bottom-to-top, so floats overlay the tree.

## Commands and the palette

**`CommandRegistry`** (Command.cpp) - the command system. A `Command` is a unique `name`
("pane.close"), a display `title` ("Close current pane"), and an `std::function` action.
The registry rejects duplicate names (a name is an identity: config keybinds and, later,
plugins address commands by it), supports `remove` (a restarting plugin re-registers), and
`run(name)`. The built-ins are registered by main.cpp with lambdas over the session; plugins
will register theirs over the wire protocol, name-spaced by plugin name.

**`Palette`** (Palette.cpp) - the command palette UI, a modal overlay (not a pane). `open()`
takes a snapshot of `(name, title)` items and sorts by title; typing filters
(case-insensitive substring), up/down move the selection, RETURN picks, ESCAPE closes.
`open_input(title)` reuses the same overlay as a free-text prompt (no items; RETURN picks the
typed text, read back via `query()`) - that is how Save as asks for a path. `open_picker(title,
source)` is the third mode: the query is fed to a `source` callback that computes the items
(it is the source's input, not a filter), re-run on every edit; RETURN picks the selection, or
the typed text itself when nothing is listed.

Open file is built on the picker plus `PathCompletion.hpp`: by default `complete_path()`
completes the typed path VSCode-Remote-style - the query's directory part is listed, the rest
prefix-filters it case-insensitively, so `..`, nested paths and absolute `/` all just work;
picking a directory descends (the picker reopens with the query set inside it), picking a file
opens it, and RETURN with no completion takes the typed path as a new file. A query starting
with a **space** switches to `fuzzy_find_files()`: a recursive, case-insensitive subsequence
match over the tree (hidden directories skipped, scanning capped), shortest paths first.
`handle(KeyEvent)` returns an `Outcome` (`ignored / updated / closed / picked`) so the caller
knows what to repaint and whether to run `selected_name()` - the palette itself never runs
anything. `draw()` paints a centered, capped-size box (focused-border color, `Commands`
title, `> query` line, selection highlighted with the theme's selection colors) into the
Grid *after* the panes, so it overlays them. SHIFT-RETURN key binding from the palette
(README) waits on the config system.

The Master key is `ctrl-space` (CONFIG.md's default) - terminals send it as NUL, which the
parser maps to ctrl+`' '`. While the palette is open, main routes every key to it (modal);
Master toggles it closed again.

## Configuration (spice::config)

CONFIG.md's design, implemented (plugins excepted until they exist). TOML via a vendored
toml++ (`third_party/tomlplusplus/`, a PRIVATE include of the config library - it never leaks
into public headers). Two files, exactly as specified:

- `$XDG_CONFIG_HOME/spice/config.toml` (fallback `~/.config/spice/`) - user-owned, never
  written by Spice. Sections: `[keys]` (`master`, `palette_run`, `palette_bind`),
  `[[keybind]]` (`key` + `command`, or `key` + `plugin` + `command`, namespaced to
  `<plugin>.<command>`), `[lifecycle]` (`shutdown_grace`, `sigterm_grace`, "2s"/"250ms"
  durations) and `[log]` (`level`, `file`, with a leading `$VARIABLE` expanded). Lifecycle
  and log are parsed and carried in the `Config` for their consumers-to-be (plugin shutdown,
  the logger); everything else is live today.
- `$XDG_STATE_HOME/spice/keybinds.toml` (fallback `~/.local/state/spice/`) - Spice-owned,
  regenerated wholesale by `save_keybinds()` (comments do not survive; the file says so).

`load()` never fails: absent files and bad entries fall back to defaults and are reported in
`Config::warnings` (surfaced in the event log at startup). Precedence per CONFIG.md:
config.toml wins - a state keybind whose key the user (or the Master key) holds is dropped.

`KeyName.hpp` translates between config-file key names and event ids, both directions:
`parse_key("ctrl-space")` → `C-' '` for building the binding map,
`key_name("C-'u'")` → `ctrl-u` for writing keybinds.toml. Modifiers compose in any order;
bases are single characters or named keys (`return`, `page-up`, `f1`...).

The binding flow from the README works end to end: in the palette, the `palette_bind` key
(default shift-return - configure a plain chord on terminals that don't distinguish it)
arms a bind for the selected command, the next keypress becomes its key, the bind goes live
immediately and is written to keybinds.toml. Binding a key held by config.toml or the Master
key is refused with a message saying where the existing binding lives.

## main.cpp: wiring it together

main.cpp is the composition root: one `App` class owning the session, command registry,
palette, clipboard and render state, with a documented method per concern (commands,
bindings, click/drag handling, palette routing, repaint). Nothing algorithmic lives here -
editing, event naming, key encoding and base64 all sit in core behind tests - the App only
assembles and dispatches.

The command line: `spice [options] [file...]`. File arguments open straight into panes,
taking the Welcome pane's place. `--help`, `--version` (from the CMake project version),
`--config <file>` (use a specific config.toml), `--no-config` (built-in defaults, no files
read) and `--list-commands` (print every command name and title - the reference for writing
config.toml keybinds; works without a terminal). Bad options exit 2 with the usage.

Startup order matters: query `TermInfo` → build the session (Welcome pane + the event log, an
append-only buffer in a floating grid pane at the bottom right) → enter the alternate screen →
construct `EventReader` (raw mode) → first full repaint.

Event dispatch is layered. First the modal checks: a pending palette bind captures the next
key; while the palette is open it takes every key. Otherwise a table: `event_id()` gives
every event a position-independent identity string (`C-'w'`, `C-up`, `press left`), and an
`unordered_map<string, function<void(Event const&)>>` maps those ids to command runners. The
map is built in layers - built-in defaults (a data table), then keybinds.toml, then
config.toml keybinds, then the Master key - each overriding the last, so every default key
is user-overridable. Copy/paste keep their terminal meaning on PTY panes (the runner
forwards the control bytes instead); clicks stay interactive handlers. Unbound plain keys fall through to
`edit_pane()`, which applies characters/enter/backspace/delete/arrows to the focused pane's
buffer through the capability-checked `Buffer` API. The same `event_id` feeds `describe()`,
so the on-screen event log and the dispatcher share one naming scheme.

When the focused pane is a PTY pane, unbound keys - modifiers included, so ctrl-c reaches the
child - are translated back into terminal bytes (`key_to_bytes`, KeyBytes.hpp) and written to its pty; the
output comes back through `pump_ptys()`, which runs every loop turn (event or 100 ms timeout)
so shell output appears without user input.

Panes are mouse-movable by their border: a press on a border cell (inside the pane's area but
outside its content area) starts a drag. While dragging, a floating pane follows the pointer
(`move_float`, clamped to the screen, damage-repainted); a docked pane shows no motion but
swaps places with whatever pane it is dropped on (`swap_panes`). A press on content is a
normal click: focus + cursor placement.

Repaint per event: handlers accumulate damage (`vector<Rectangle>`; layout changes request a
full frame), then one `repaint()` clears the grid, has the session redraw every pane into it,
renders only the damaged rects, and parks the terminal cursor on the focused pane's cursor.
CPU-side drawing is done every event (cheap); terminal I/O is only the damage (the expensive
part).

## Conventions

- **Errors are values.** Mutators return `bool` (false = rejected: out of bounds, capability,
  unknown id); lookups return `std::optional` or an empty view/nullptr. No exceptions are
  thrown by Spice code; nothing is expected to be caught.
- **No inheritance, no virtuals.** Types compose; enums (`PaneType`, `EventType`,
  `BufferCapability`...) select behavior where variation exists.
- **Degrade without a tty.** Every terminal-touching class has a defined no-tty behavior
  (TermInfo 0×0, EventReader inert, Grid renders nothing) so the test binary runs anywhere.
- Headers in `inc/spice/core/`, one concept per header (a class, or a family of small value
  types like Event.hpp's), `#ifndef SPICE_CORE_*_H` guards, trailing-return-type style,
  `_underscore` members, doc comments with `//!`.

## Testing

Unit tests are doctest, one `test_<Class>.cpp` per class (see BUILD.md for the mechanics).
The split that makes them possible: everything except `EventReader`'s poll loop and `Grid`'s
final `fwrite` is pure computation. `EventParser` is tested byte-sequence-in/events-out;
`Buffer`/`Layout`/`Pane`/`Spice` are tested as plain data structures, with `Pane::draw`
asserted against `Grid` cells directly - no terminal needed. Classes that do touch the
terminal get no-tty smoke tests (must not crash, must return nothing).

End-to-end behavior (raw mode, mouse reports, rendering, the whole main loop) is verified
outside ctest by driving the real binary under a pseudo-terminal (Python `pty` +
`TIOCSWINSZ`), sending real byte sequences and reconstructing the screen from the emitted
escape stream. That is how the pane-system flow (split, type, click, float, dock, close-all →
exit) was validated.

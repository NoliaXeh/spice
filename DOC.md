# Spice internals

How the code is actually built, as of today. The [README](README.md) describes the design and
where the project is going; [BUILD.md](BUILD.md) describes how to build it; this file describes
what exists: the classes, how they relate, and how the terminal is driven at the syscall and
escape-sequence level.

Everything so far lives in one module, `core` (headers in `inc/spice/core/`, sources in
`src/core/`, tests in `tests/core/`), plus the executable's `src/main.cpp`.

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
      Event        └─ Layout
   (Key, Mouse*)       (split tree + floats)

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

**`Pane`** (Pane.cpp) - a view: `PaneType` (edit / grid / pty - pty is declared but has no
process plumbing yet), a `shared_ptr<Buffer>` (shared: two panes can view one buffer; a
buffer with no pane just lives on), a cursor and a scroll offset - the only view-specific
state. `draw(grid, area, focused, theme)` paints a box-drawing border (`┌─┐│└┘`) with the
buffer's name as title, then the visible slice of the buffer. Edit panes auto-scroll to keep
the cursor visible; grid/pty panes keep whatever scroll they were given (`scroll_to_bottom`
for scrollback-style following). `position_from_screen` maps a click to a buffer position;
`cursor_screen_position` maps the cursor back to a screen cell.

**`Layout`** (Layout.cpp) - where panes are, knowing only pane ids. Two structures, exactly as
the README prescribes:

- the **split tree**: nodes are either a leaf (pane id) or a split (orientation + two
  children). `tiles(screen)` partitions the screen recursively, each split halving its
  rectangle exactly - tiles always cover the screen with no gaps. `insert` splits a leaf in
  two; `remove` grafts the sibling into the parent's place.
- the **float list**: `(pane, Rectangle)` pairs; z-order is list rank, last on top.

On top of those: `pane_at` (hit test, topmost float first, then tiles) and `neighbor`
(directional focus: probe one cell beyond the pane's edge, level with its center, and see
which tile is there).

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
`handle(KeyEvent)` returns an `Outcome` (`ignored / updated / closed / picked`) so the caller
knows what to repaint and whether to run `selected_name()` - the palette itself never runs
anything. `draw()` paints a centered, capped-size box (focused-border color, `Commands`
title, `> query` line, selection highlighted with the theme's selection colors) into the
Grid *after* the panes, so it overlays them. SHIFT-RETURN key binding from the palette
(README) waits on the config system.

The Master key is `ctrl-space` (CONFIG.md's default) - terminals send it as NUL, which the
parser maps to ctrl+`' '`. While the palette is open, main routes every key to it (modal);
Master toggles it closed again.

## main.cpp: wiring it together

Startup order matters: query `TermInfo` → build the session (Welcome pane + the event log, an
append-only buffer in a floating grid pane at the bottom right) → enter the alternate screen →
construct `EventReader` (raw mode) → first full repaint.

Event dispatch is layered. First the modal check: while the palette is open it takes every
key. Otherwise a table: `event_id()` gives every event a position-independent identity
string (`C-'w'`, `C-up`, `press left`), and an
`unordered_map<string, function<void(Event const&)>>` maps those ids to handlers. The
ctrl-letter bindings just `registry.run()` a command by name - the same shape config keybinds
will take (`(plugin, command)` by name, per CONFIG.md) - while focus moves and clicks are
interactive handlers that also track damage. Unbound plain keys fall through to
`edit_pane()`, which applies characters/enter/backspace/delete/arrows to the focused pane's
buffer through the capability-checked `Buffer` API. The same `event_id` feeds `describe()`,
so the on-screen event log and the dispatcher share one naming scheme.

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

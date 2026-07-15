# Spice

The buffer manager made to match any taste.

## What is Spice

Spice is a buffer manager - think of it as a stripped-down, very basic Emacs.
It aims to be a simple core with a powerful API for plugins. Several plugins ship built in.
Spice is fully TUI.

You can write a plugin in any language, as long as it can talk to Spice over stdin/stdout.
Plugins read and write buffers, drive panes, and receive events.
[PLUGIN.md](PLUGIN.md) shows you how, starting from a complete plugin in 50 lines of Python.

## Buffers and panes

Buffers and panes are decoupled.

- A buffer is content. The core owns a buffer's bytes.
- A pane is a view onto a buffer.

The same buffer can be shown in several panes at once, and a buffer can exist with no pane
showing it at all (a background buffer). Closing a pane does not destroy its buffer.

Every pane has a buffer, including PTY panes. But not every buffer accepts the same operations,
so each buffer carries a capability flag:

- `editable` - arbitrary insert/delete anywhere (EditPane).
- `append-only` - content grows at the end; history is read-only (PTYPane scrollback).
  Plugins can read and search it, but "insert at line 12" is not a valid operation.

Plugins must respect the flag; the core rejects writes a buffer doesn't support.

## Panes

There are 3 types of panes:

- **EditPane** - text editing. The core handles keyboard input, mouse events, selection,
  line editing, and undo. See "Where editing lives" below.

- **GridPane** - raw pane. Everything is handled by a plugin.

- **PTYPane** - interactive shell pane, to run an external program.

### Layout

Tiled panes live in a **split tree**. Floating panes live in a flat list on top of the tree,
each with a position, a size, and a z-order.

- **Float** takes a pane out of the split tree and makes it floating.
- **Dock** puts a floating pane back into the tree.

When all panes are closed, the program ends.
On startup, the "Welcome" pane is opened.

## Where editing lives

The core owns text editing. Input, selection, line editing, and undo are implemented in the
core, not in a plugin.

This is deliberate. A fully "dumb grid" core, where every keystroke round-trips through a plugin
subprocess, would be laggy, and every text plugin would have to reimplement selection and undo
from scratch. Editors that keep editing in-core (Neovim, Kakoune) do so for exactly this reason.

Plugins layer *on top* of editing - file IO, syntax highlighting, LSP, completion - rather than
providing it.

## Command palette

From any pane, press the **Master key** to open the command palette. The palette lists the
commands offered by plugins, plus the built-ins.

The Master key is a **prefix**, not a mode: `Master` followed by a key reaches Spice, and every
other key passes straight through to the pane - including to a child program in a PTYPane. This is
how a PTYPane can run vim or a REPL without Spice swallowing its keys.

The Master key is **configurable from day one**, and its default is still to be decided.
It is *not* `ESCAPE`: terminal escape sequences (arrows, function keys, alt-combos) are themselves
ESC-prefixed, so a bare ESC can only be distinguished from the start of a sequence by a timeout -
which is either laggy or occasionally wrong. An unambiguous chord is preferred.

Defaults inside the palette:

- Run the selected command: `RETURN`
- Bind a key to the selected command: `SHIFT-RETURN`

### Built-in commands

Session:
- Close Spice
- Open Welcome Pane

Plugins:
- List plugins
- Restart plugin

Buffers:
- List buffers
- Switch buffer
- New buffer
- Kill buffer

Panes:
- List panes
- Open pane
- Close current pane
- Move focus left / right / up / down
- Move pane left / right / up / down
- Float pane
- Dock pane

PTY:
- Run command in new PTY
- Run command in current PTY

## Plugins

Plugins are listed in a config file. Each entry has:

- A name
- A command to run (path + argv)
- A mode:
  - **Global** - one instance watching all panes
  - **Pane** - one instance per opened pane

On startup a plugin receives information on stdin and announces itself on stdout, then declares
its commands. Plugins may request a socket for parallel interaction.

Plugins run as ordinary subprocesses with the **full privileges of the user**. There is no
sandboxing. This is a deliberate choice (and normal for this class of tool), but it means
installing a plugin is as much a trust decision as installing any other program.

### Protocol shape

The full wire protocol gets its own spec. What is already settled:

- **Framing.** Length-prefixed frames (à la LSP's `Content-Length`), *not* newline-delimited -
  a payload containing a newline must not be able to break framing.

- **Encoding.** JSON to start. msgpack is the intended upgrade path if JSON becomes a bottleneck
  (it's what Neovim uses, and it keeps the "library already exists in every language" property
  that makes any-language plugins actually viable). A bespoke binary format is a last resort:
  it taxes every plugin author in every language.

- **Asynchronous, both directions.** The core emits events and **does not wait** for a response.
  Plugins act whenever they want.
  - core → plugin: fire-and-forget events.
  - plugin → core: commands, carrying an optional request-id when the plugin wants a reply.

- **Versioning.** A protocol version is exchanged in the handshake. A major-version mismatch is
  rejected outright rather than limped through.

## Built-in plugins

### Files

A Pane plugin providing file IO for an EditPane. It does not *make* the pane an editor - the core
already does that - it adds the filesystem layer on top.

- `CTRL+S` - save
- `CTRL+O` - open
- `CTRL+SHIFT+S` - save as

### More to come...

## Open questions

- **Stable positions under async.** Because the core never waits on plugins, a plugin acting on an
  event may be working from a stale view: the user kept typing, so a raw offset the plugin captured
  no longer points where it thought. Two candidate fixes, not yet chosen:
  - **Marks/anchors** - stable handles that the core moves as text changes; plugins address text
    through them rather than by raw offset.
  - **Buffer versions** - every buffer carries a version number, and a plugin's write must cite the
    version it was computed against; the core rejects writes citing a stale version.

  Whichever is chosen must be baked into the buffer API early. It is painful to retrofit.

- **GridPane's buffer model.** If every pane has a buffer, what *is* a GridPane's buffer?
  A grid of styled cells (glyph + attributes), like a real terminal screen? Or lines of text like
  the others? A single buffer abstraction across all three pane types probably has to bottom out in
  styled cells, with EditPane's line buffer as a convenience view on top. The alternative is to
  accept that GridPane doesn't really share the model, and that "every pane has a buffer" is a
  slogan rather than an API guarantee.

## Implementation

Spice is a C++ project, C++23 or later.
It builds with GCC 16.1+, using CMake.

Guidelines are in GUIDELINES.md

**Compilers.** GCC is the primary target. The code should stay portable enough for Clang, which
means Clang runs in CI from the start - portability that isn't tested rots immediately. MSVC is not
supported and is not a goal: both GCC and Clang target Windows.

**Modules.** Spice does **not** use C++20 modules. Every use of the word "module" below is purely
semantic.

Folders:

- `src/` - source files
- `tests/` - tests
- `inc/spice/` - header files

`src/` is split into modules. Each module builds to a static library and has its headers in `inc/`.

For example, the `palette` module:

- `src/palette` - sources
- `inc/spice/palette` - headers
- `tests/palette` - tests

The `src` part links into `palette.a`. When building the executable, that `.a` (and the others) are
linked against `src/main.cpp`.

The `tests` part links against the same `palette.a`, with its own main file. So each module gets
its own `palette_test` executable.

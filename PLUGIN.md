# Writing a Spice plugin

A plugin is **a program**. Not a script Spice loads, not a shared library it links: a process it
starts, talks to over a pipe, and stops. Spice sends it messages on its stdin and reads messages
from its stdout.

That is the whole integration. It means:

- **Any language.** Python, JavaScript, Go, Rust, a shell script with a msgpack helper. If it can
  read stdin and write stdout, it can be a Spice plugin.
- **A crashing plugin cannot crash the editor.** Different process, different memory.
- **A hung plugin cannot freeze the editor.** Spice never waits for you. See [The one rule that
  shapes everything](#the-one-rule-that-shapes-everything).

This document is the practical guide. [PROTOCOL.md](PROTOCOL.md) is the design rationale - read it
when you want to know *why*; read this when you want to build something.

> **Version 0.1.** The protocol is young. [What is not built yet](#what-is-not-built-yet) is an
> honest list, not a roadmap tease - check it before designing around a capability.

---

## Hello, world

A complete plugin. It adds a **Word count** command to the palette; running it counts the words in
the focused buffer and shows the result.

```python
#!/usr/bin/env python3
import sys, msgpack

# --- plumbing: frames in, frames out -------------------------------------

def read_frame():
    """Read one message. Returns None when Spice closes the pipe."""
    header = sys.stdin.buffer.read(4)
    if len(header) < 4:
        return None
    length = int.from_bytes(header, "big")
    return msgpack.unpackb(sys.stdin.buffer.read(length), raw=False)

def write_frame(message):
    payload = msgpack.packb(message, use_bin_type=True)
    sys.stdout.buffer.write(len(payload).to_bytes(4, "big") + payload)
    sys.stdout.buffer.flush()          # never forget this

def notify(method, params):
    write_frame([1, None, method, params])

_next_id = 0
def request(method, params):
    global _next_id
    _next_id += 1
    write_frame([2, _next_id, method, params])
    return _next_id

# --- the plugin ----------------------------------------------------------

pending = {}                            # request id -> what to do with the answer

while (message := read_frame()) is not None:
    kind, id, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "commands": [{"name": "count", "title": "Word count"}],
        })

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break                           # exit promptly and quietly

    elif kind == 0 and method == "spice.palette.command_invoked":
        if params["command"] == "count" and "buffer" in params:
            id = request("buffer.get_lines", {"buffer": params["buffer"], "start": 0, "end": -1})
            pending[id] = "count"

    elif kind == 3 and pending.get(id) == "count":
        del pending[id]
        if "code" in params:            # the request failed
            notify("status.error", {"code": "wc.failed", "message": params["code"]})
        else:
            words = sum(len(line.split()) for line in params["lines"])
            notify("status.message", {"text": f"{words} words"})
```

Install it by making it executable and adding it to your `config.toml`:

```toml
[[plugin]]
name    = "wordcount"
command = ["/path/to/wordcount.py"]
```

Start Spice, open the palette, type `Word count`. That is a plugin.

The rest of this document explains every piece of that program, and everything else you can do.

---

## The wire format

### Framing

Messages are length-prefixed so the reader knows where each one ends:

```
[ 4 bytes: payload length, big-endian ] [ payload: one msgpack value ]
```

Read exactly 4 bytes, decode a big-endian unsigned integer, read exactly that many more bytes,
unpack them. Nothing else is on the pipe - no newlines, no separators.

A frame declaring more than **64 MiB** gets your plugin killed. Nothing legitimate is that big.

### The envelope

Every payload is an **array of exactly four elements**:

```
[ kind, id, method, params ]
```

| Field    | Type           | Meaning                                               |
| -------- | -------------- | ----------------------------------------------------- |
| `kind`   | integer 0-4    | what sort of message this is (below)                  |
| `id`     | integer or nil | correlates a request with its response; nil otherwise |
| `method` | string         | the topic (for events) or the method name             |
| `params` | map            | the payload; always a map, `{}` when empty            |

Map keys are always strings. An envelope that is not a 4-element array is dropped silently - Spice
will not tell you, it will just look like your message vanished.

### The five kinds

| Kind | Name         | Direction   | Answered?                              |
| ---- | ------------ | ----------- | -------------------------------------- |
| `0`  | **event**    | Spice → you | never                                  |
| `1`  | **notify**   | you → Spice | never                                  |
| `2`  | **request**  | you → Spice | with a `response`                      |
| `3`  | **response** | Spice → you | answers your request, carries its `id` |
| `4`  | **error**    | Spice → you | unsolicited: your *notify* was bad     |

You only ever *send* kinds `1` and `2`. You only ever *receive* kinds `0`, `3`, and `4`.

---

## The one rule that shapes everything

> **Spice never waits for you.**

There is no message Spice sends that blocks the editor pending your reply. This is a correctness
guarantee, not a performance target, and it has consequences you must design around:

- **You cannot veto anything.** There is no "before save" hook you can cancel. By the time you hear
  about something, it has happened.
- **Nothing you send is urgent.** Take a second to answer if you need to; the user keeps typing.
- **The world moves while you think.** The buffer you read may have changed before your write
  lands. This is what [versions](#buffers) are for.
- **Take your time, but drain your pipe.** If you never read your stdin, Spice's writes to you will
  eventually fill the pipe and be dropped. Read continuously; do slow work without blocking your
  read loop (a thread, a queue, an async task - your language, your choice).

The mirror of the rule: **you must not wait for Spice either**, beyond an actual response you
asked for. Requests are answered; notifies never are. Waiting for an acknowledgement of a notify
will hang you forever.

---

## Lifecycle

### Startup: hello, then ready

Spice starts your process and immediately sends:

```
event  spice.lifecycle.hello
  { protocol: "0.1",
    plugin: 1,               # your numeric id, for this run
    name: "wordcount",       # your name from config.toml
    mode: "global",          # or "pane"
    pane: 4, buffer: 9 }     # pane mode only: your own grid pane and buffer
```

A **pane-mode** plugin (`mode = "pane"` in config) is given a dedicated grid pane and
buffer before its hello; draw to the pane with `grid.update` and treat it as yours. A
global plugin gets neither and works with what the session shows.

You answer with `ready` - this is the handshake, and until you send it **everything else you send
is ignored**:

```
notify  ready
  { protocol: "0.1",
    subscribe: ["spice.palette.command_invoked"],   # what you want to hear
    publishes: ["wc.results."],                     # optional, informational
    commands: [ {name, title, description?} ] }     # optional, see Commands
```

**Version negotiation is by major version, and it is strict.** Spice compares the part before the
first `.` of your `protocol` against its own. A mismatch is not negotiated or shimmed: Spice logs
it, tells the user, and terminates you. Send `"0.1"`.

### Shutdown

```
event  spice.lifecycle.shutdown  { grace_ms: 2000 }
```

Finish what matters and exit. You have `grace_ms` milliseconds, and **Spice keeps processing what
you send during them** - a plugin flushing a save on the way out will complete. A grace period
during which the core stopped listening would not be a grace period.

Miss it and you get `SIGTERM`, then `SIGKILL`. Exiting promptly is polite; there is no reply to
send.

### Crash

If you exit at any other time, Spice notices, unregisters your commands, and tells the user.
Keybinds pointing at your commands become no-ops that explain why.

Whether you come back is your `restart` policy in config (see
[Installing a plugin](#installing-a-plugin)): `on-crash` relaunches you unless you exited by
yourself with status 0 (a clean exit stays dead); `always` relaunches either way - each at
most `max_restarts` times per `restart_window`, then Spice gives up and says so. A relaunch
is a fresh process: you get `hello` again and re-register everything.

Dying *before* your handshake is reported too - so a plugin that crashes on startup is a visible
failure, not a mystery. **Your stderr is captured and logged**, at `warn`. It is the only channel a
crashing plugin still has; use it freely, and check it when a plugin "does nothing".

---

## Subscriptions

**You receive only what you subscribe to.** No subscription, no events - a plugin that forgets
`subscribe` in its `ready` looks completely dead.

Matching is by **topic prefix**, which makes exact matches and whole families the same mechanism:

```
"spice."                        # everything from the core
"spice.pane."                   # every pane event
"spice.palette.command_invoked" # exactly this one
"lint.diagnostics."             # a broadcast family from the "lint" plugin
```

Change your subscriptions any time - the list **replaces** the previous one:

```
notify  subscribe  { subscribe: ["spice.pane.", "lint."] }
```

### The events Spice sends today

This is the complete list.

```
event  spice.lifecycle.hello           { protocol, plugin, name, mode }
event  spice.lifecycle.shutdown        { grace_ms }
event  spice.palette.command_invoked   { plugin, command, pane, buffer? }
event  spice.palette.keybind_triggered { plugin, command, pane }
event  spice.palette.command_cancel    { plugin, command, pane }
event  spice.pane.opened               { pane, kind, buffer }
event  spice.pane.closed               { pane }
event  spice.pane.focused              { pane }
event  spice.pane.unfocused            { pane }
event  spice.pane.floated              { pane }
event  spice.pane.docked               { pane }
event  spice.pane.resized              { pane, rows, cols }
event  spice.pane.buffer_set           { pane, buffer }
event  spice.buffer.created            { buffer, caps }
event  spice.buffer.killed             { buffer }
event  spice.buffer.changed            { buffer, from_version, to_version, splices? }
event  spice.buffer.dirty_changed      { buffer, dirty }
event  spice.input.key                 { pane, key, mods }
event  spice.input.mouse               { pane, kind, row, col, mods }
```

**Joining late is fine.** When your handshake completes, Spice replays the current world:
you receive `created` / `opened` / `focused` events for everything that already exists.
The other plugins see those repeats too - treat `created` and `opened` as upserts.

Worth knowing before you design:

- Pane and buffer events report the session as it *is*, whoever changed it - the user, you,
  or another plugin. `spice.buffer.changed` fires for **every** change, user typing included,
  and carries the incremental `splices` (byte-column ranges, see [Buffers](#buffers)) - the
  mirror strategy from PROTOCOL.md works. When `splices` is absent the change journal
  overflowed: refetch the buffer instead of patching.
- `spice.input.mouse` includes motion, which floods during drags. Subscribe to
  `spice.input.key` alone if keys are all you need.
- The lifecycle events arrive whether or not you subscribe to them.

---

## Commands

A command is how a plugin gets into the palette. Declare them in `ready`, or later:

```
notify  command.register    { commands: [{name, title, description?, cancellable?}] }
notify  command.unregister  { names: ["count"] }
```

`name` is your identifier (`count`). `title` is what the user reads in the palette
(`Word count`) - write it for a human. `description` shows dimmed after the title when
there is room. Registration is dynamic: an LSP plugin should offer *Go to definition* only
once a server has attached, and withdraw it when the server dies.

A command declared `cancellable: true` makes Spice offer a *Cancel: <title>* palette entry
once it has been invoked; picking it sends you `spice.palette.command_cancel`. You are free
to have already finished - treat a late cancel as a no-op.

When the user runs one, you get an event - **if you subscribed to it**:

```
event  spice.palette.command_invoked
  { plugin: "wordcount",   # which plugin's command (yours, if you subscribed narrowly)
    command: "count",      # which command
    pane: 3,               # the focused pane
    buffer: 7 }            # its buffer - absent if no pane is focused
```

**Commands are fire-and-forget.** No id, no lifetime, no progress, no cancellation, no completion.
You are not expected to reply - you do the work and report through the ordinary channels:
`status.message` for progress, `status.error` for failure.

Buffer ids also arrive in `spice.buffer.created` and `spice.pane.opened` events, but
`command_invoked` hands you the one the user *means* - it is where most plugins start.

### Keybinds

```
notify  keybind.set  { key: "ctrl-g", plugin: "wordcount", command: "count" }
notify  keybind.set  { key: "g", mods: ["ctrl"], plugin: "wordcount", command: "count" }
```

`key` is a config-style key name - `ctrl-g`, `alt-left`, `f5`, `ctrl-space`, `shift-return` -
with modifiers either folded into the name or passed separately in `mods`; both forms above
bind the same key.
Binds point at **`(plugin, command)`, never at an id**, so restarting or re-registering never
orphans a user's keybinds. A bind pointing at a command nobody has registered is a no-op that tells
the user why.

---

## Buffers

A buffer is content: text, decoupled from any pane showing it. Buffers are addressed by an integer
id, which you learn from `command_invoked` or from creating one.

### Versions: the rule for every write

**Every read gives you a version. Every write must cite the version it was based on.**

```
request  buffer.info       { buffer }              → { line_count, version, caps, dirty, name }
request  buffer.get_lines  { buffer, start, end }  → { lines: [str], version }
request  buffer.get_text   { buffer, range }       → { text, version }
```

`end` is exclusive; `-1` means "to the end". `get_text`'s range is clamped into the buffer,
so `{start: {line: 0, col: 0}, end: {line: -1, col: -1}}` reads everything.

```
request  buffer.splice  { buffer, range, text, version }
         → { version: <new> }                                    # applied
         → { code: "spice.core.stale_version", version: <now> }  # refused
```

If the buffer changed since your read, your write is **refused, not merged and not silently
dropped**. Re-read, redo your work against the new content, try again. This is the whole
concurrency model: staleness is loud.

Insert is a splice with an empty `range`. Delete is a splice with empty `text`.

Many edits computed against one read - a formatter's output - go in **one batch**, atomically:

```
request  buffer.splice_many  { buffer, splices: [{range, text}, ...], version }
         → { version: <new> }
```

Every range cites the base `version` - do not adjust later splices for earlier ones, Spice
does that. Splices must be non-overlapping and inside the buffer, or the whole batch is
refused with `spice.core.bad_params` and nothing is applied. The batch costs exactly one
version bump and undoes as a single step.

### Marks: remembering a place

A position you computed goes stale the moment the user types above it. A **mark** is a
position the core keeps current for you:

```
request  mark.set     { buffer, pos: {line, col}, gravity?: "left" | "right" }  → { mark }
request  mark.get     { buffer, mark }  → { pos: {line, col}, valid, version }
request  mark.delete  { buffer, mark }  → { }
```

Set it once, read it whenever you need the *current* location. `gravity` says what happens
when text is inserted exactly at the mark: `left` (default) stays put, `right` rides along.
If the text around a mark is deleted, the mark parks at the deletion point with
`valid: false` - your cue to re-derive the location rather than trust it.

### Positions are (line, byte-column)

```
range: { start: {line, col}, end: {line, col} }
```

- `line` is 0-based.
- **`col` is a byte offset into that line's UTF-8**, not a character count. In a line containing
  `héllo`, the `l` is at col `3`, because `é` occupies two bytes. Get this wrong and you will
  corrupt multi-byte text.
- Ranges are half-open: `[start, end)`.
- `text` may contain `\n`; a splice is one undoable edit.

### Creating

```
request  buffer.create  { caps: "editable" | "append", name? }  → { buffer }
```

`append` buffers only grow at the end - the right choice for logs and output. A splice into one is
refused with `spice.core.capability_denied`.

### Highlights: coloring text you don't own

```
notify  buffer.set_highlights  { buffer, highlights: [{range, fg: 0xRRGGBB}, ...] }
```

Paints foreground colors over a buffer's text wherever it is shown - the syntax-highlighting
primitive. The list **replaces your own layer** on that buffer; other plugins' layers are
untouched. Layers stack in `[[plugin]]` declaration order - later entries paint over earlier
ones, earlier ones show through the gaps - so a regex keyword-colorer declared *before* an
LSP highlighter fills in exactly what the LSP leaves uncolored. Ranges are byte-addressed;
after an edit your spans drift until you recompute, so subscribe to `spice.buffer.changed`
and resend.

Two complete working examples live in this repository, both self-contained Python:

- [`plugins/cpp-keywords/cpp_keywords.py`](plugins/cpp-keywords/cpp_keywords.py) colors C++
  keywords pink with a regex - the minimal highlighter, ~200 lines.
- [`plugins/lsp-highlight/lsp_highlight.py`](plugins/lsp-highlight/lsp_highlight.py) bridges
  **any LSP server** (clangd, rust-analyzer, pylsp...): semantic tokens in, highlight spans
  out, including the LSP UTF-16-to-byte column conversion that must never leak into the core.

### How a response tells you it failed

**A response carries a `code` key if and only if it failed.** There is no separate success flag:

```python
if "code" in params:
    ...  # it failed; params["code"] says how
else:
    ...  # it worked; the result is in params
```

---

## Panes

A pane is a view onto a buffer. All of these are notifies - fire-and-forget, no reply:

```
notify  pane.open        { kind: "edit" | "grid" | "pty", buffer?, split?: "h" | "v" }
notify  pane.close       { pane }
notify  pane.focus       { pane }
notify  pane.float       { pane }
notify  pane.dock        { pane }
notify  pane.set_buffer  { pane, buffer }
notify  buffer.kill      { buffer }
```

`pane.open` without a `buffer` opens a fresh scratch buffer. Without a `split`, the pane opens
however the session sees fit.

Since `pane.open` is fire-and-forget, **it does not tell you the new pane's id.** Subscribe to
`spice.pane.opened` (or `spice.pane.focused`) if you need to know where things went.

`pane.set_buffer` repoints a pane at another buffer; the pane's cursor and scroll reset.
`buffer.kill` drops a buffer from the session - silently refused while any pane still shows
it (close or repoint the pane first). Success is visible as `spice.buffer.killed`.

## Drawing: GridPanes

A `grid` pane can show **your** cells instead of a buffer. Send an update and it is on
screen - there is no flush, and Spice coalesces updates within a frame:

```
notify  grid.update      { pane, rect: {row, col, rows, cols},
                           chars?: [str], fg?: [u32], bg?: [u32], style?: [u32] }
notify  grid.clear       { pane }                       # back to buffer content
notify  grid.set_cursor  { pane, pos: {line, col}, visible? }
```

- **Dirty rects.** Send only what changed; a blinking cursor is one cell, not 80x24.
- Each layer is optional and, when present, flat, row-major, exactly `rows x cols` long
  (anything else is `spice.core.bad_params`). `chars` holds one grapheme cluster per cell;
  an empty string is the continuation cell of a double-width character.
- Colors are `0xRRGGBB`. `style` bits: 1 bold, 2 italic, 4 underline, 8 strikethrough,
  16 reverse, 32 dim, 64 blink (dim and the underline-style bits are accepted but not yet
  rendered).
- Coordinates are relative to the pane's **content area**, whose size arrives via
  `spice.pane.resized`. On a resize the surface starts blank - redraw it then.

Any plugin may draw to any grid pane it knows the id of; a pane-mode plugin gets its own
(see below). Cell coordinates in `spice.input.mouse` line up with what you drew.

---

## Talking to the user

```
notify  status.message  { text }                    # progress, results
notify  status.error    { code, message, data? }    # something went wrong
notify  palette.open    { }                         # open the command palette
notify  log             { level, message }          # trace|debug|info|warn|error
```

`status.error` is for **failures that are results** - a file you could not read, a server that
would not start. Namespace the `code` under your own plugin name (`wordcount.no_buffer`), because
`spice.core.*` codes mean *Spice* failed, and confusing the two sends users to the wrong place.
Every error, of either sort, carries `code` and a human-readable `message`.

`log` lines below the user's `[log] level` (CONFIG.md, default `info`) skip the log pane;
when a `[log] file` is configured, every line lands there regardless of level. Your stderr
still arrives separately, logged at `warn`.

---

## Talking to other plugins

```
notify  broadcast  { topic: "lint.diagnostics.ready", payload: { ... } }
```

Subscribers to that topic prefix receive it as an ordinary event:

```
event  lint.diagnostics.ready  { source: 2, payload: { ... } }
```

Spice is a dumb broker here: it does not parse, validate, or understand your `payload` - any
msgpack value survives the trip. Three rules:

- **`spice.` is reserved.** A broadcast on a `spice.` topic is dropped, so a plugin can never forge
  a core event.
- **You never receive your own broadcast**, even if you subscribe to your own topic.
- **100 broadcasts per second, per plugin.** Past that, the rest of the second is dropped and
  logged - a broadcast loop between two plugins throttles instead of melting the session.

What topics exist is discoverable - every ready plugin's declared `publishes`:

```
request  topics.list  { }  → { topics: [{plugin, prefix}, ...] }
```

Note the asymmetry: you *publish* with the `broadcast` method, but subscribers *receive* a plain
event on your topic. There is one topic namespace and one filter for core events and plugin
broadcasts alike. Declaring your topics in `ready`'s `publishes` is informational - documentation
for humans, not a requirement.

---

## Errors

Two kinds, deliberately on different channels.

**Your request failed** - the answer is the `response`, carrying `code` (see
[above](#how-a-response-tells-you-it-failed)).

**Your notify was bad** - a notify has no reply, so Spice sends an unsolicited `error` (kind `4`)
naming the offending method:

```
[4, nil, "buffer.slice", {code: "spice.core.unknown_method"}]
```

`error` is a *kind*, not a topic, precisely so you cannot unsubscribe from your own bug reports.

The codes Spice sends:

| Code                           | Meaning                                                          |
| ------------------------------ | ---------------------------------------------------------------- |
| `spice.core.unknown_method`    | no such method - typo, or a capability that does not exist yet   |
| `spice.core.no_such_id`        | that buffer or pane is gone                                      |
| `spice.core.stale_version`     | your write cited an old version; the current one is in `version` |
| `spice.core.capability_denied` | that buffer does not accept that (splicing an `append` buffer)   |
| `spice.core.bad_params`        | the params were malformed                                        |
| `spice.core.plugin_crashed`    | reported to the *user* about you, not sent to you                |

---

## A note on other languages

Nothing above is Python. The two things a language needs are a msgpack library and blocking reads
on stdin. In JavaScript, the plumbing from the example becomes:

```javascript
import { encode, decode } from "@msgpack/msgpack";

function writeFrame(message) {
    const payload = encode(message);
    const header = Buffer.alloc(4);
    header.writeUInt32BE(payload.length);
    process.stdout.write(Buffer.concat([header, Buffer.from(payload)]));
}

let buffer = Buffer.alloc(0);
process.stdin.on("data", chunk => {
    buffer = Buffer.concat([buffer, chunk]);
    while (buffer.length >= 4) {
        const length = buffer.readUInt32BE(0);
        if (buffer.length < 4 + length) break;     // wait for the rest
        const message = decode(buffer.subarray(4, 4 + length));
        buffer = buffer.subarray(4 + length);
        handle(message);
    }
});
```

The event-driven shape is a better fit than the Python example's loop, and the protocol does not
care: read frames, write frames, never block.

---

## Installing a plugin

Plugins are declared in your `config.toml` (see [CONFIG.md](CONFIG.md)):

```toml
[[plugin]]
name    = "wordcount"                       # unique; your namespace for topics and errors
command = ["/path/to/wordcount.py"]         # argv: the program and its arguments
mode    = "global"                          # "global" (one instance) or "pane"
restart = "on-crash"                        # "never" | "on-crash" | "always"
```

Only `name` and `command` are required. `command` is an argv array, not a shell line - no quoting,
no shell expansion, no `$PATH` surprises beyond the usual `execvp` lookup.

Your plugin's `name` is the namespace you own: your broadcast topics and your error codes should
start with it.

---

## Rules of the road

1. **Flush after every write.** The single most common reason a plugin "does nothing": the message
   is sitting in your language's stdout buffer. Python needs `.flush()`; most languages buffer
   stdout when it is a pipe.
2. **Never print to stdout.** Stdout is the protocol. One stray `print()` corrupts the frame stream
   and desynchronizes it. Debug on **stderr** - it is captured and logged for you.
3. **Send `ready` first.** Nothing else is honored before the handshake.
4. **Subscribe, or hear nothing.**
5. **Never block your read loop.** Do slow work elsewhere.
6. **Cite versions on writes**, and handle `stale_version` by retrying, not by ignoring it.
7. **Columns are byte offsets**, not characters.
8. **Exit promptly on shutdown.**

---

## What is not built yet

The protocol describes more than the core currently implements. The list is short now:

| Not yet                            | What that means for you                                                          |
| ---------------------------------- | -------------------------------------------------------------------------------- |
| `ul_color`, underline-style bits, `dim` | accepted in `grid.update`, but not rendered; the other style bits work      |
| command progress reporting         | report progress with `status.message`; cancellation exists                       |
| one pane-plugin instance *per pane* | pane mode means one dedicated pane per plugin, created at startup               |

Each of these is additive: nothing above changes the shape of a message defined in this document.
A plugin you write today keeps working.

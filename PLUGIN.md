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
    mode: "global" }         # or "pane"
```

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

This is the complete list. It is short, and honestly so.

```
event  spice.lifecycle.hello        { protocol, plugin, name, mode }
event  spice.lifecycle.shutdown     { grace_ms }
event  spice.palette.command_invoked { plugin, command, pane, buffer? }
event  spice.pane.focused           { pane }
event  spice.buffer.changed         { buffer, to_version }
```

Two caveats worth knowing before you design:

- `spice.buffer.changed` currently fires **only for splices plugins make**, and carries no
  detail of the change beyond the new version. **You will not be told when the user types.**
  A plugin that must track user edits cannot be written yet.
- The lifecycle events arrive whether or not you subscribe to them.

---

## Commands

A command is how a plugin gets into the palette. Declare them in `ready`, or later:

```
notify  command.register    { commands: [{name, title, description?}] }
notify  command.unregister  { names: ["count"] }
```

`name` is your identifier (`count`). `title` is what the user reads in the palette
(`Word count`) - write it for a human. Registration is dynamic: an LSP plugin should offer
*Go to definition* only once a server has attached, and withdraw it when the server dies.

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

This is the *only* way to learn a `buffer` id, so `command_invoked` is where most plugins start.

### Keybinds

```
notify  keybind.set  { key: "ctrl-g", plugin: "wordcount", command: "count" }
```

`key` is a config-style key name - `ctrl-g`, `alt-left`, `f5`, `ctrl-space`, `shift-return`.
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
```

`end` is exclusive; `-1` means "to the end".

```
request  buffer.splice  { buffer, range, text, version }
         → { version: <new> }                                    # applied
         → { code: "spice.core.stale_version", version: <now> }  # refused
```

If the buffer changed since your read, your write is **refused, not merged and not silently
dropped**. Re-read, redo your work against the new content, try again. This is the whole
concurrency model: staleness is loud.

Insert is a splice with an empty `range`. Delete is a splice with empty `text`.

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
notify  pane.open   { kind: "edit" | "grid" | "pty", buffer?, split?: "h" | "v" }
notify  pane.close  { pane }
notify  pane.focus  { pane }
notify  pane.float  { pane }
notify  pane.dock   { pane }
```

`pane.open` without a `buffer` opens a fresh scratch buffer. Without a `split`, the pane opens
however the session sees fit.

Since `pane.open` is fire-and-forget, **it does not tell you the new pane's id.** Subscribe to
`spice.pane.focused` if you need to know where things went.

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
msgpack value survives the trip. Two rules:

- **`spice.` is reserved.** A broadcast on a `spice.` topic is dropped, so a plugin can never forge
  a core event.
- **You never receive your own broadcast**, even if you subscribe to your own topic.

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

The protocol describes more than the core currently implements. These are honest gaps - a
`spice.core.unknown_method` error, or silence, is what you will get:

| Not yet                                 | What that means for you                                                                 |
| --------------------------------------- | --------------------------------------------------------------------------------------- |
| `buffer.get_text`, `buffer.splice_many` | use `get_lines` and one `splice` at a time                                              |
| `spice.buffer.changed` for *user* edits | you cannot react to typing; only your own splices are echoed, without detail            |
| `spice.input.*`, `spice.edit.*` events  | no keystroke or edit stream to subscribe to                                             |
| `pane.set_buffer`, `buffer.kill`        | accepted and silently ignored - they do nothing                                         |
| a command's `description`               | accepted, but nothing displays it yet; put what matters in `title`                      |
| `grid.update` (drawing to a GridPane)   | plugins cannot render custom UI; a plugin's output goes through buffers and status      |
| `mode = "pane"` semantics               | parsed and passed in `hello`, but a pane plugin is not yet given its own pane or buffer |
| `restart` policy                        | parsed, but a crashed plugin is not restarted                                           |
| Marks, command cancellation             | see PROTOCOL.md's "Not in v1"                                                           |

Each of these is additive: nothing above changes the shape of a message defined in this document.
A plugin you write today keeps working.

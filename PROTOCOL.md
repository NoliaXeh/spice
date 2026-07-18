# Spice Plugin Protocol

Version `0.1` (unstable - expect breaking changes until `1.0`)

This document specifies the wire protocol between the Spice core and a plugin.
A plugin may be written in any language that can read and write msgpack over stdin/stdout.

## Design invariants

These are load-bearing. Every rule in this document follows from them.

1. **The core never blocks on a plugin.** There is no message the core sends that it waits for
   a reply to. A slow, hung, or dead plugin can never stall the editor. This is not a performance
   goal, it is a correctness invariant.

2. **The core owns the truth.** Buffer bytes, the split tree, focus, and the palette live in the
   core. Plugins observe and request; they never hold authoritative state.

3. **Plugins cannot veto.** A direct consequence of (1). Nothing a plugin does can prevent an
   action from happening. Where a veto is genuinely needed (unsaved-changes prompts), it is
   core behaviour, because the core owns the dirty flag.

4. **Staleness is loud, never silent.** Because the core never waits, every plugin's view of a
   buffer is a snapshot from the past. The protocol makes acting on a stale view *fail*, rather
   than succeed against the wrong bytes.

## Transport

- **Framing.** Length-prefixed. A 4-byte big-endian unsigned length, followed by exactly that
  many bytes of msgpack payload. Newline-delimited framing is *not* used: a payload containing a
  newline must not be able to break the stream.

- **Encoding.** msgpack.

- **Channels.** stdin and stdout only. Both are full-duplex and asynchronous, so no side channel
  is needed. There is no socket API.

- **stderr** is captured by the core and routed to the log at `warn` level. A plugin that dies
  outside the protocol (segfault, panic, uncaught exception) says its last words here, so this is
  the fallback diagnostic path and must not be discarded.

- **Frames must be written atomically.** If a plugin forks or uses multiple threads, all writers
  must serialise on the output stream. Interleaved partial frames corrupt the stream irrecoverably
  and will look like an msgpack bug. Use a single writer or a mutex.

## Envelope

Every message is a msgpack array of exactly four elements:

```
[kind, id, method, params]
```

| kind | name       | direction     | reply |
|------|------------|---------------|-------|
| `0`  | `event`    | core → plugin | never |
| `1`  | `notify`   | plugin → core | never |
| `2`  | `request`  | plugin → core | yes   |
| `3`  | `response` | core → plugin | -     |
| `4`  | `error`    | core → plugin | never |

- `id` - `u64` for `request` and the `response` that matches it. `nil` for all other kinds.
  Ids are chosen by the plugin and need only be unique among that plugin's in-flight requests.
- `method` - a string. For `event`, this is the **topic**. For `notify`/`request`, the **method
  name**. For `response`, the method being answered (echoed, for debugging). For `error`, the
  method that could not be processed.
- `params` - a map. May be empty, never `nil`.

There is **no core → plugin request**. That is invariant (1), and it is not negotiable: adding one
would make the core's liveness depend on a subprocess.

## Two namespaces

Do not confuse these. They are different vocabularies with different rules.

- **Topics** (`event`) - what a plugin *subscribes to* and receives. Dotted, hierarchical.
  `spice.` is reserved for the core; every other prefix belongs to a plugin (see Broadcast).

- **Methods** (`notify` / `request`) - what a plugin *calls on the core*. A flat, closed vocabulary
  defined in this document. Plugins cannot add methods.

## Ids

`plugin`, `pane`, `buffer` and `mark` ids are opaque `u64`, allocated monotonically per type, and
**never reused within a process lifetime**. A long-lived session may create millions of panes; id
reuse would let a plugin holding a stale `pane 7` write into an unrelated new `pane 7`. The 64-bit
space makes reuse unnecessary.

Commands are *not* addressed by id. See Commands.

## Lifecycle

### Startup

The core spawns the plugin process, then sends:

```
event  spice.lifecycle.hello
  { protocol: "0.1",
    plugin: <plugin_id>,
    name: "lsp",
    mode: "global" | "pane",
    pane: <pane_id>,        # pane mode only
    buffer: <buffer_id> }   # pane mode only
```

The plugin replies:

```
notify ready
  { protocol: "0.1",
    subscribe: ["spice.edit.", "spice.palette.command_invoked"],
    publishes: ["lsp.diagnostics."],     # optional, see Broadcast
    commands: [ {name, title, description?}, ... ] }
```

**Version negotiation.** The core compares the major version. A mismatch is rejected outright:
the core logs, reports to the user, and does not run the plugin. There is no compatibility shimming.

### Shutdown

```
event  spice.lifecycle.shutdown  { grace_ms: 2000 }
```

The core **continues to process incoming notifies during the grace period** - a plugin flushing a
save on the way out must be able to complete. A grace period during which the core stops reading is
not a grace period.

After `grace_ms`: `SIGTERM`. After a further interval: `SIGKILL`.

### Crash

The core detects the process exiting. It:

1. Unregisters the plugin's commands. Keybinds pointing at them become no-ops that report
   *"plugin `lsp` is not running"* to the user when pressed.
2. Reports the death to the user.
3. Restarts or does not, per the plugin's `restart` policy in config (see CONFIG.md).

Commands have no lifetime and are never "in flight" (see Commands), so there is nothing to cancel.

## Subscriptions

A plugin receives **only** the events it subscribed to in `ready`. Subscription is by
**topic prefix** or exact topic:

```
"spice."                 # everything from the core
"spice.input."           # all input events
"spice.input.key"        # exactly this one
"lsp.diagnostics."       # a broadcast topic from the lsp plugin
```

Subscriptions can be changed at runtime with the `subscribe` method.

This is deliberately the *same* mechanism for core events and inter-plugin broadcast. There is one
namespace and one filter.

## Core topics

### `spice.lifecycle.`
`hello`, `shutdown`

### `spice.pane.`
`opened` `{pane, kind, buffer}` · `closed` `{pane}` · `focused` `{pane}` · `unfocused` `{pane}` ·
`resized` `{pane, rows, cols}` · `floated` `{pane}` · `docked` `{pane}` ·
`buffer_set` `{pane, buffer}`

### `spice.buffer.`
`created` `{buffer, caps}` · `killed` `{buffer}` · `changed` (see Buffers) ·
`dirty_changed` `{buffer, dirty}`

### `spice.input.`
`key` `{pane, key, mods}` · `mouse` `{pane, kind, row, col, mods}`

In an **EditPane** these are informational - the core has already applied them. In a **GridPane**
they are the entire point. They are emitted identically in both cases; plugins that do not care
simply do not subscribe.

### `spice.palette.`
`command_invoked` `{plugin, command, pane, buffer}` · `keybind_triggered` `{plugin, command, pane}` ·
`command_cancel` `{plugin, command, pane}` (see Commands)

## Buffers

### Positions

A position is `{line: u64, col: u64}` where **`col` is a byte offset into that line's UTF-8**.
A range is `{start, end}`, **half-open**: `[start, end)`.

Rationale: the buffer is line-structured, so a line-local edit invalidates nothing else. A flat
file offset would require a cumulative byte index that every edit invalidates.

Two hard rules:

- **UTF-16 never enters this protocol.** LSP addresses text in UTF-16 code units, for historical
  JavaScript reasons. That conversion happens *inside the LSP plugin*. Do not leak it into the core.
- **Grapheme clusters are a rendering concern, not an addressing one.** Addressing is bytes.
  Clusters appear only in the grid (see Grid).

### Capabilities

Every buffer carries capabilities:

- `editable` - arbitrary splices anywhere. (EditPane buffers.)
- `append` - content grows at the end; history is immutable. (PTYPane scrollback.) Plugins may
  read and search it; a splice into it fails with `spice.core.capability_denied`.

### Versions

Every buffer has a `version: u64`, starting at `0` and incremented on **every** change, whatever
the source.

**Every write must cite the version it was computed against.** If it does not match the core's
current version, the write is **rejected** with `spice.core.stale_version`, which carries the
current version.

This exists because of invariant (4). Worked example of the race it prevents:

> Buffer is `hello world`. A formatter reads it and computes *delete `[0,5)–[0,6)`* (the space).
> While it computes, the user types `X` at the start; the buffer is now `Xhello world`.
> The formatter's splice arrives. Byte 5 is now `o`. Without a version check, the core deletes
> the `o` - silent corruption, from a plugin that was not buggy, about a buffer that no longer
> exists.

**Retries do not livelock,** provided the plugin maintains a *mirror*: `spice.buffer.changed`
carries the incremental splices, so a plugin applies them to its own copy and rebases its edit
locally. It only ever needs to be faster than a single keystroke, not faster than the session.
This is what LSP clients do, and why LSP versions documents.

**The core never rebases a plugin's edit.** That is operational transform, it is ruinously subtle,
and it is not going in the core.

### Reads

```
request  buffer.get_lines  { buffer, start, end }  → { lines: [str], version }
request  buffer.get_text   { buffer, range }       → { text, version }
request  buffer.info       { buffer }              → { line_count, version, caps, dirty, name? }
```

Every read returns the version it was taken at. Use it for the write that follows.

### Writes

One primitive:

```
request  buffer.splice  { buffer, range, text, version }
         → ok  { version: <new> }
         → err spice.core.stale_version { version: <current> }
```

Insert is an empty `range`. Delete is an empty `text`.

Batched, atomic, **one** version bump and one undo step:

```
request  buffer.splice_many  { buffer, splices: [{range, text}, ...], version }
         → ok { version: <new> }
```

Splices must be **non-overlapping**, and all positions are relative to the cited base version.
Overlapping or out-of-buffer splices fail with `spice.core.bad_params`, applying nothing.

`splice_many` is not an optimisation, it is a requirement: "format the whole file" as 400 individual
splices would be 400 version bumps, rejecting every other plugin 400 times.

### Change events

```
event  spice.buffer.changed
  { buffer, from_version, to_version, splices: [{range, text}, ...] }
```

Incremental, always. Without this a plugin must refetch the entire buffer on every keystroke, and
the mirror strategy that makes versions livable becomes impossible. (`splices` is omitted only
when the core's change journal overflowed between events; refetch then.)

### Highlights

Decoration, not content: colored spans painted over a buffer's text wherever it is shown.

```
notify  buffer.set_highlights  { buffer, highlights: [{range, fg}, ...] }
```

- Replaces the buffer's **whole** set - one set per buffer, last writer wins.
- `fg` is `0xRRGGBB`; ranges are byte-addressed like every range here. Selection beats
  decoration when both cover a cell.
- The core never rebases or invalidates them: after an edit they may briefly sit on the
  wrong bytes until their owner recomputes against the new content. Subscribe to
  `spice.buffer.changed` and resend; a stale frame of color is acceptable where a stale
  write never is.

### Marks

Stable anchors the core moves as text changes around them. Versions solve *"my write is
stale"*; marks solve *"my remembered location is stale"* - diagnostics underlines,
breakpoints, folds.

```
request  mark.set     { buffer, pos: {line, col}, gravity?: "left" | "right" }  → { mark }
request  mark.get     { buffer, mark }  → { pos, valid, version }
request  mark.delete  { buffer, mark }  → { }
```

- **Gravity** decides where an insertion *exactly at* the mark leaves it: `left` (default)
  keeps the mark before the inserted text; `right` pushes it after.
- **Invalidation is loud.** Deleting the text a mark sits inside does not silently relocate
  it: the mark parks at the deletion point with `valid: false`. It keeps tracking from
  there; treat `valid: false` as "re-derive this location".
- Mark ids are per buffer, monotonically allocated, never reused.

## Grid (GridPane)

A GridPane's content is **four parallel layers** over the same cell rectangle:

| layer   | cell type | notes |
|---------|-----------|-------|
| `chars` | string    | one **grapheme cluster** per cell |
| `fg`    | `u32`     | `0xRRGGBB` truecolor; core downsamples for poorer terminals |
| `bg`    | `u32`     | as above |
| `style` | `u32`     | bitfield |

Layers compress well (`fg` and `bg` are long runs of one value) and are updated independently.

**Cells hold grapheme clusters, not codepoints.** A cell is a short UTF-8 string. Anything else
cannot render a decomposed `é` or any ZWJ emoji.

**Double-width characters occupy two cells**: the cluster in the first, and an **empty string** in
the second as the continuation marker. This convention is mandatory - without it, every plugin
invents a different answer and the grid tears.

**Style bits:** `bold` `italic` `underline` `strikethrough` `reverse` `dim` `blink`, plus a 3-bit
underline-style field (`straight` `double` `curly` `dotted` `dashed`). An optional fifth layer
`ul_color` carries underline colours. You will want curly coloured underlines the day someone
writes the LSP plugin.

### Updates

```
notify  grid.update
  { pane,
    rect: {row, col, rows, cols},
    chars?: [str],  fg?: [u32],  bg?: [u32],  style?: [u32],  ul_color?: [u32] }
```

- **Dirty rects.** Send only the rectangle that changed. A blinking cursor is one cell, not 80×24.
- Each layer is **optional**: send only the layers that actually changed. Present layers are flat,
  row-major, and exactly `rows × cols` long.

```
notify  grid.clear       { pane }
notify  grid.set_cursor  { pane, pos, shape, visible }
```

### Flushing and compositing

**There is no flush call.** A `grid.update` *is* the update. The core redraws on grid change,
resize, selection change, or float overlay, and **coalesces updates within a frame** so that a
chatty plugin cannot tear the screen or melt the terminal.

The plugin owns only its own grid content. The core owns compositing: selection, floating pane
overlays, z-order, borders.

## Panes, buffers, palette

```
notify  pane.open        { kind, buffer?, split?: "h"|"v" }
notify  pane.close       { pane }
notify  pane.focus       { pane }
notify  pane.float       { pane }
notify  pane.dock        { pane }
notify  pane.set_buffer  { pane, buffer }

request buffer.create    { caps?, name? } → { buffer }
notify  buffer.kill      { buffer }

notify  palette.open     { }
notify  status.message   { text }
notify  status.error     { code, message, data? }
```

## Commands

Commands are **fire-and-forget**. A command is not a task: it has no id, no lifetime, no progress,
and no cancellation.

```
notify  command.register    { commands: [{name, title, description?}] }
notify  command.unregister  { names: [str] }
```

Commands may be registered and unregistered **dynamically** - an LSP plugin only offers
*go to definition* once a server has attached.

Invocation is an event:

```
event  spice.palette.command_invoked  { plugin, command, pane, buffer }
```

The plugin then does its work and reports through ordinary channels: progress via `status.message`,
failure via `status.error`. Neither requires the core to track a task.

**Keybinds bind to `(plugin_name, command_name)`, never to an id.** Otherwise every plugin restart
or re-registration orphans the user's keybinds. A bind pointing at a command that is not currently
registered is a no-op that tells the user why.

```
notify  keybind.set  { key, mods, plugin, command }
```

**Cancellation** stays fire-and-forget: a command declared with `cancellable: true` makes the
core offer a transient *Cancel: <title>* palette entry once the command is invoked. Picking it
emits:

```
event  spice.palette.command_cancel  { plugin, command, pane }
```

The entry withdraws itself once used (and with the plugin's death). Nothing is tracked: the
plugin may already have finished, and must tolerate a cancel for work it no longer remembers.

## Broadcast

A plugin may publish to a topic; the core relays it to every plugin subscribed to that prefix.

```
notify  broadcast  { topic: "lsp.diagnostics.changed", payload: <any msgpack> }
```

Delivered to subscribers as:

```
event  lsp.diagnostics.changed  { source: <plugin_id>, payload: <any msgpack> }
```

Rules:

- **The core is a dumb broker.** The payload is opaque. The core never interprets it. It routes on
  prefix, stamps the source, and forgets.
- **`spice.` is reserved.** A plugin cannot publish to it, so core events cannot be spoofed.
- **Never delivered back to the source.** The cheapest loop-breaker there is.
- **No request/response over broadcast, ever.** If A needs an answer from B, that is two broadcasts
  and a correlation id *inside the payload* - and A must tolerate never getting one. Letting a
  plugin block on another plugin would reinvent synchronous IPC through the core and hand the core
  liveness obligations it cannot honour (B is dead? slow? absent?). Invariant (1) must not leak out
  through this channel.
- **Ordering** is preserved per source→destination pair. Nothing is guaranteed across pairs.
- **Loops are the plugins' problem.** A reacts to B, B reacts to A. The core cannot detect this
  semantically and will not try. It rate-limits and logs: past **100 broadcasts per second**
  from one plugin, the rest of that second is dropped (logged once per window).

The topic registry built on `publishes` is queryable:

```
request  topics.list  { }  → { topics: [{plugin, prefix}, ...] }
```

Declaring `publishes` in `ready` is still informational - undeclared topics broadcast fine -
but declared ones are discoverable here instead of being social convention.

**Known long-term risk, accepted with open eyes:** broadcast can become the de facto API, with
plugins coupling over undocumented topics - a second, unspecified protocol inside the specified one.
This is roughly what happened to Emacs, and Emacs is fine, but it means "what topics exist" becomes
social convention rather than spec. The optional `publishes` field in `ready` exists to mitigate
this: it lets the core expose a discoverable topic registry.

## Logging

```
notify  log  { level: "trace"|"debug"|"info"|"warn"|"error", message, data? }
```

The core owns the sink (file, log pane, or discard) and the level filter, so plugins do not each
invent their own logging configuration. Plugin **stderr** is captured separately and logged at
`warn` - it is the only channel a crashing plugin still has.

## Errors

Two kinds, and they must not travel on the same channel.

### Protocol errors - the core could not process what you sent

Audience: the plugin **developer**.

For a `request`, the failure comes back as the `response`. For a `notify` - which has no reply -
the core sends an unsolicited `error` (kind `4`) carrying the offending method.

`error` is a **message kind, not a topic**, and this is deliberate: topics are subscribable, and
therefore *un*subscribable. If "I could not process your notify" were an event, a plugin could
filter away its own bug reports, and someone would, and they would lose an evening to it.

Core codes, all under `spice.core.`:

| code | meaning |
|------|---------|
| `bad_version` | protocol major mismatch at handshake |
| `bad_params` | malformed or invalid params (incl. overlapping splices) |
| `unknown_method` | no such method |
| `no_such_id` | pane/buffer/mark id does not exist |
| `stale_version` | write cited an out-of-date buffer version; carries `{version}` |
| `capability_denied` | e.g. splice into an `append` buffer |

### Functional errors - a command ran and failed

Audience: the **user**. `LSP: server not responding.` `Files: permission denied writing /etc/hosts.`

These are *results*, not protocol failures. They are reported with `status.error` and namespaced
under the plugin's own prefix:

```
notify  status.error  { code: "files.permission_denied", message: "...", data: {...} }
```

Every error, of either class, carries `code` (namespaced string), `message` (human-readable), and
optional `data`.

## Not in v1

Deliberately deferred. Each is **additive** - none requires changing a message defined above.

- **Command progress reporting** (cancellation exists; progress is `status.message` for now).
- **`ul_color` and the underline-style bits' rendering** - accepted on the wire, not yet drawn.
- **Multi-instance pane mode** - a pane-mode plugin gets one dedicated pane at startup, not an
  instance per pane the user opens.

## Never

- **Core → plugin requests.** Breaks invariant (1).
- **Core-side rebase / operational transform.** See Buffers.

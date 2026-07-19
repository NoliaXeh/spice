# 05 — Listening to events

No commands this time: a plugin that watches the session live — every
buffer that appears, every edit as it happens — and narrates it to the
log. Boring on purpose; the *watching* is the lesson, and tutorials 06–08
all build on it.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/bell.py
from plumbing import notify, read_frame

seen = set()

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.buffer.", "spice.pane.focused"],
        })

    elif kind == 0 and method == "spice.buffer.created":
        if params["buffer"] not in seen:       # replays repeat: upsert
            seen.add(params["buffer"])
            notify("log", {"level": "info",
                           "message": f"buffer {params['buffer']} appeared"
                                      f" ({params['caps']})"})

    elif kind == 0 and method == "spice.buffer.changed":
        splices = params.get("splices")
        notify("log", {"level": "debug", "message":
            f"buffer {params['buffer']}"
            f" v{params['from_version']} -> v{params['to_version']}"
            + (f", {len(splices)} splice(s)" if splices is not None
               else ", too many changes: refetch")})

    elif kind == 0 and method == "spice.pane.focused":
        notify("log", {"level": "debug",
                       "message": f"pane {params['pane']} focused"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break
```

Run it with `[log] level = "debug"` in your config, type a few characters,
switch panes — and watch the narration.

## What's new

**Subscription is by prefix.** `"spice.buffer."` means every buffer event;
`"spice.pane.focused"` means exactly that one. You receive *only* what you
subscribed to — a plugin that forgets to subscribe looks completely dead.
Change the list at runtime with the `subscribe` notify (it replaces the
previous list).

**Joining late is fine.** Buffers that existed before your handshake are
*replayed* to you: `created` / `opened` / `focused` events for the current
world arrive right after you become ready. The flip side: everyone else
sees those repeats too, so treat `created` and `opened` as upserts — the
`seen` set above is not decoration.

**`spice.buffer.changed` is incremental.** It carries `from_version`,
`to_version`, and the actual `splices` (ranges + text, byte columns) for
every change — user typing, another plugin's writes, undo, PTY output. A
serious plugin keeps a local mirror of the buffer and *applies* those
splices instead of refetching everything per keystroke; that mirror is
what makes the version-retry loop of tutorial 04 cheap. When `splices` is
absent, the journal overflowed: refetch.

**The event vocabulary** (see PLUGIN.md for the full list): panes open,
close, focus, float, dock, resize; buffers appear, die, change, flip their
dirty flag; keys and mouse arrive under `spice.input.` if you want a
front-row seat. Subscribe narrowly — `spice.input.mouse` includes motion,
which floods during drags.

**Don't block the loop.** Events arrive whenever they arrive. If your
reaction is slow (a network call, a compile), do it elsewhere — a thread,
a queue — and keep reading stdin. If you stop reading, Spice's pipe to
you fills, and its writes to you start dropping. Tutorial 08 shows the
select-based loop that handles "wake up on input *or* on a timer".

## Exercise

Track dirtiness: subscribe stays the same, but on `spice.buffer.
dirty_changed` keep a set of dirty buffer ids and log "N unsaved buffers"
whenever it changes. You have just written the model behind every
"unsaved files" indicator ever.

Next: [06 — Syntax highlighting](06-syntax-highlighting.md) — the first
plugin that changes what the user *sees*.

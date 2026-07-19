# 02 — Commands and keys

A plugin that does something *when asked*: it registers a palette command,
reacts when the user runs it, and binds a key to it. This is the shape of
most plugins.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/fortune.py
import random
from plumbing import notify, read_frame

FORTUNES = [
    "your build will pass on the first try",
    "a segfault you did not write awaits",
    "someone will finally read your commit messages",
    "the bug is in the code you are most proud of",
]

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "commands": [
                {"name": "tell", "title": "Tell my fortune"},
            ],
        })
        notify("keybind.set", {
            "key": "f6", "plugin": "fortune", "command": "tell",
        })

    elif kind == 0 and method == "spice.palette.command_invoked":
        if params["command"] == "tell":
            notify("status.message", {"text": random.choice(FORTUNES)})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break
```

```toml
[[plugin]]
name    = "fortune"
command = ["python3", "tutorial/fortune.py"]
```

Open the palette (`ctrl-p`), type "fortune", RETURN — or just press F6.

## What's new

**Declaring commands.** The `commands` list in `ready` puts entries in the
palette. `name` is your identifier; `title` is what humans read — write it
for them. An optional `description` shows dimmed after the title.

**Invocation is an event** — and you only get it because you subscribed to
`spice.palette.command_invoked`. Forget the subscription and your command
sits in the palette doing nothing: *commands and their invocations are
separate mechanisms*, deliberately. The event carries which plugin, which
command, the focused pane, and (when a pane is focused) its buffer id —
hold that thought for tutorial 03.

**Fire and forget.** Nothing tracks your command: no task id, no progress,
no completion. You do the work and report through `status.message` (or
`status.error` for failures — namespace the code under your own name,
like `fortune.out_of_luck`).

**Keybinds** target `(plugin, command)` by *name*, never an id — so a
restart never orphans them. `key` is a config-style name (`f6`, `ctrl-g`,
`shift-return`); the palette shows it as a hint next to your command.

**Registration is dynamic.** You can `command.register` and
`command.unregister` at any time after ready — offer *Go to definition*
only once a server is attached, withdraw it when the server dies:

```python
notify("command.register", {"commands": [{"name": "later", "title": "..."}]})
notify("command.unregister", {"names": ["later"]})
```

## Exercise

Add a second command `bind-me` that, when invoked, uses `keybind.set` to
bind itself to `f7`. Then check the palette: the hint appears on the
entry.

Next: [03 — Reading buffers](03-reading-buffers.md), where the event's
`buffer` field starts earning its keep.

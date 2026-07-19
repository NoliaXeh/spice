# 03 — Reading buffers

The first plugin that looks at *content*: a command that counts the TODOs
in the buffer you are editing. It introduces the request/response half of
the protocol.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/todos.py
from plumbing import notify, read_frame, request

pending = {}  # request id -> what we asked and about which buffer

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "commands": [{"name": "count", "title": "Count TODOs"}],
        })

    elif kind == 0 and method == "spice.palette.command_invoked":
        if params["command"] == "count" and "buffer" in params:
            buffer = params["buffer"]
            pending[request("buffer.get_lines",
                            {"buffer": buffer, "start": 0, "end": -1})] = buffer

    elif kind == 3 and id_ in pending:
        buffer = pending.pop(id_)
        if "code" in params:
            notify("status.error", {
                "code": "todos.unreadable",
                "message": f"could not read buffer {buffer}: {params['code']}",
            })
        else:
            found = sum(line.count("TODO") for line in params["lines"])
            notify("status.message", {"text": f"{found} TODOs here"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break
```

Open a file with a few TODOs, run *Count TODOs* from the palette.

## What's new

**Requests are answered; notifies never are.** `request()` writes kind 2
with an id you choose, and the answer comes back later as kind 3 carrying
that id. *Later* is the important word — Spice never blocks on you, and
you must not block on it beyond matching up responses. The `pending` dict
is the standard pattern: remember what each id was about, act when the
response lands.

**The read API:**

```
buffer.info       {buffer}                → {line_count, version, caps, dirty, name}
buffer.get_lines  {buffer, start, end}    → {lines: [str], version}
buffer.get_text   {buffer, range}         → {text, version}
```

`end` is exclusive and `-1` means "to the end". Every read also returns a
`version` — ignore it today, tutorial 04 is entirely about it.

**How failure looks.** A response carries a `code` key *if and only if* it
failed — there is no separate success flag:

```python
if "code" in params:   # "spice.core.no_such_id" and friends
    ...
```

Buffers can vanish between the event and your request (the user killed
it); a plugin that doesn't check `code` works right up until it doesn't.

**Where buffer ids come from.** `command_invoked` hands you the buffer the
user *means* — the focused one. Ids also arrive in `spice.buffer.created`
and `spice.pane.opened` events (tutorial 05). There is no "list all
buffers" request, on purpose: you react to what you are told about.

**`status.error` vs `status.message`.** A failure that is a *result* (file
unreadable, nothing found) is `status.error` with a code in *your*
namespace (`todos.unreadable`). Codes under `spice.core.` mean Spice
itself failed — confusing the two sends users to the wrong bug tracker.

## Exercise

Use `buffer.info` first and only count TODOs in buffers whose `name` ends
in `.py` or `.cpp`; report "not a code buffer" otherwise. (Two chained
requests: the `pending` dict now stores *which step* you are on — exactly
how the reference plugins do it.)

Next: [04 — Editing buffers](04-editing-buffers.md), where writes meet
versions and lose politely.

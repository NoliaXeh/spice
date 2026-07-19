# 07 — Marks

Versions (tutorial 04) solve *"my write is stale."* Marks solve a
different problem: *"my remembered location is stale."* A plugin that
remembers a spot — a bookmark, a breakpoint, a diagnostic's underline —
needs that spot to move as the user edits above it. That is what a mark
is: a position the core keeps current for you.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/bookmark.py
from plumbing import notify, read_frame, request

marks = {}     # buffer -> mark id
pending = {}   # request id -> ("set", buffer) | ("get", buffer)

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "commands": [
                {"name": "drop", "title": "Bookmark: drop here"},
                {"name": "where", "title": "Bookmark: where is it now?"},
            ],
        })

    elif kind == 0 and method == "spice.palette.command_invoked":
        buffer = params.get("buffer")
        if buffer is None:
            continue
        if params["command"] == "drop":
            # anchor a mark at the top of the file for the demo; a real
            # plugin would use the cursor's line/col
            pending[request("mark.set", {
                "buffer": buffer,
                "pos": {"line": 0, "col": 0},
                "gravity": "right",
            })] = ("set", buffer)
        elif params["command"] == "where" and buffer in marks:
            pending[request("mark.get",
                            {"buffer": buffer, "mark": marks[buffer]})] = ("get", buffer)

    elif kind == 3 and id_ in pending:
        what, buffer = pending.pop(id_)
        if "code" in params:
            notify("status.error", {"code": "bookmark.gone",
                                    "message": params["code"]})
        elif what == "set":
            marks[buffer] = params["mark"]
            notify("status.message", {"text": f"bookmarked (mark {params['mark']})"})
        elif what == "get":
            pos, valid = params["pos"], params["valid"]
            state = "" if valid else " (text around it was deleted)"
            notify("status.message",
                   {"text": f"bookmark now at line {pos['line']}, col {pos['col']}{state}"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break
```

Run *drop*, type several new lines at the top of the file, run *where* —
the reported line has moved down to follow the text, without you tracking
a single edit.

## What's new

**The mark API:**

```
mark.set     {buffer, pos: {line, col}, gravity?}  → {mark}
mark.get     {buffer, mark}   → {pos: {line, col}, valid, version}
mark.delete  {buffer, mark}   → {}
```

You set once and read whenever; the core moves the mark as text shifts
around it. Ids are per buffer and never reused.

**Gravity** decides one edge case: when text is inserted *exactly at* the
mark, does the mark stay before it or ride after it? `"left"` (default)
stays put; `"right"` follows the inserted text. A bookmark that should
grow with what you type at it wants `right`; an end-of-region marker
usually wants `left`.

**Invalidation is loud.** If the user deletes the text a mark sits inside,
the core does *not* quietly relocate it somewhere plausible. It parks the
mark at the deletion point and sets `valid: false`. Treat that as "this
location is gone, re-derive it" — never as a position to trust.

**Why not just remember `(line, col)` yourself?** Because you would have
to replay every `spice.buffer.changed` splice against your stored position
to keep it correct — which is precisely what the core already does for a
mark. Marks are that bookkeeping, done once, correctly, for you.

## Exercise

Store the *cursor's* position instead of `(0,0)`: `command_invoked` gives
you the focused `pane`, and you can `mark.set` at wherever makes sense.
Then keep a *list* of marks per buffer and add a "jump to next bookmark"
command.

Next: [08 — Drawing panes](08-drawing-panes.md): a plugin with its own
pane and its own picture.

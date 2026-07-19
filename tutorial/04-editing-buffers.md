# 04 — Editing buffers

A command that rewrites the buffer: every `TODO` becomes `DONE`, all at
once, correctly — even if the user is typing while you compute. This is
the tutorial about **versions**, the heart of the protocol.

## The rule

> **Every read returns a version. Every write must cite the version it
> was computed against.** If the buffer changed in between, the write is
> refused — never merged, never silently dropped.

Why: you read the buffer, you compute byte positions, and while you
compute the user types. Your positions now point at different text.
Without the version check Spice would apply your edit to bytes you never
saw — silent corruption. With it, you get told, you re-read, you retry.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/done.py
from plumbing import notify, read_frame, request

pending = {}  # request id -> ("lines" | "splice", buffer)

def resolve(buffer):
    pending[request("buffer.get_lines",
                    {"buffer": buffer, "start": 0, "end": -1})] = ("lines", buffer)

def splices_for(lines):
    """One splice per TODO, positions in BYTES against these lines."""
    out = []
    for lineno, line in enumerate(lines):
        data = line.encode()          # byte columns: encode first
        at = data.find(b"TODO")
        while at != -1:
            out.append({
                "range": {"start": {"line": lineno, "col": at},
                          "end": {"line": lineno, "col": at + 4}},
                "text": "DONE",
            })
            at = data.find(b"TODO", at + 4)
    return out

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "commands": [{"name": "resolve", "title": "TODO -> DONE"}],
        })

    elif kind == 0 and method == "spice.palette.command_invoked":
        if params["command"] == "resolve" and "buffer" in params:
            resolve(params["buffer"])

    elif kind == 3 and id_ in pending:
        what, buffer = pending.pop(id_)
        if what == "lines" and "code" not in params:
            splices = splices_for(params["lines"])
            if not splices:
                notify("status.message", {"text": "nothing to resolve"})
            else:
                pending[request("buffer.splice_many", {
                    "buffer": buffer,
                    "splices": splices,
                    "version": params["version"],   # citing the read
                })] = ("splice", buffer)
        elif what == "splice":
            if params.get("code") == "spice.core.stale_version":
                resolve(buffer)   # the world moved: re-read, recompute, retry
            elif "code" in params:
                notify("status.error", {"code": "done.failed",
                                        "message": params["code"]})
            else:
                notify("status.message", {"text": "all TODOs resolved"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break
```

Open a file with TODOs, run *TODO -> DONE*, undo once — the whole batch
reverts as a single step.

## What's new

**Columns are byte offsets.** `col` counts bytes into the line's UTF-8,
not characters. In `héllo TODO`, the T sits at byte 7 (é is two bytes).
That is why `splices_for` searches `line.encode()` — the byte string —
instead of the Python string. Get this wrong and multi-byte text corrupts;
it is the single most common plugin bug.

**One splice, three shapes.** Replace is the general form; *insert* is an
empty range, *delete* is empty text:

```
buffer.splice  {buffer, range, text, version}  → {version: new}
```

**Batches are atomic.** `buffer.splice_many` applies every splice against
the *cited* version — do not adjust later splices for earlier ones, Spice
does that. All-or-none: overlapping or out-of-bounds splices refuse the
whole batch with `spice.core.bad_params`. One version bump, one undo step.
Four hundred individual splices would be four hundred chances for another
writer to slip in between.

**The retry loop.** `stale_version` is not an error to report — it is the
protocol working. Re-read (new version), recompute, resend. The loop
terminates in practice because you only need to be faster than a
keystroke, not faster than the session.

**Capabilities.** Splicing an append-only buffer (a PTY scrollback) fails
with `spice.core.capability_denied`. `buffer.info`'s `caps` tells you in
advance.

## Exercise

Make it surgical: only resolve TODOs on the line range the command should
affect — say, the first 10 lines (`end: 10`). Note what the response's
`version` is for: it is what your *next* write must cite.

Next: [05 — Listening to events](05-listening-to-events.md): reacting to
the session instead of waiting to be invoked.

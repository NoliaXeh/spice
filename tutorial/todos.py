#!/usr/bin/env python3
# tutorial/todos.py — see 03-reading-buffers.md
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

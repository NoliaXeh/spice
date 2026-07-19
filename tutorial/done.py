#!/usr/bin/env python3
# tutorial/done.py — see 04-editing-buffers.md
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

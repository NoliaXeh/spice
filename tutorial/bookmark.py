#!/usr/bin/env python3
# tutorial/bookmark.py — see 07-marks.md
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

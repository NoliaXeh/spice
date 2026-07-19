#!/usr/bin/env python3
# tutorial/paint.py — see 06-syntax-highlighting.md
import re
from plumbing import notify, read_frame, request

PINK = 0xFF69B4
WORDS = ["TODO", "FIXME", "XXX", "HACK", "NOTE"]
WORD_RE = re.compile(rb"(?:" + b"|".join(w.encode() for w in WORDS) + rb")")

tracked = set()   # buffers we are painting
pending = {}      # request id -> buffer


def repaint(buffer):
    pending[request("buffer.get_lines",
                    {"buffer": buffer, "start": 0, "end": -1})] = buffer


def spans(lines):
    out = []
    for lineno, line in enumerate(lines):
        for m in WORD_RE.finditer(line.encode()):   # bytes -> byte columns
            out.append({
                "range": {"start": {"line": lineno, "col": m.start()},
                          "end": {"line": lineno, "col": m.end()}},
                "fg": PINK,
            })
    return out


while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.buffer.created", "spice.buffer.changed"],
        })

    elif kind == 0 and method == "spice.buffer.created":
        buffer = params["buffer"]
        tracked.add(buffer)
        repaint(buffer)

    elif kind == 0 and method == "spice.buffer.changed":
        if params["buffer"] in tracked:
            repaint(params["buffer"])   # recolor after every edit

    elif kind == 3 and id_ in pending:
        buffer = pending.pop(id_)
        if "code" in params:
            tracked.discard(buffer)
        else:
            notify("buffer.set_highlights",
                   {"buffer": buffer, "highlights": spans(params["lines"])})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break

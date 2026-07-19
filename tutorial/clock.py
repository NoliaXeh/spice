#!/usr/bin/env python3
# tutorial/clock.py — see 08-drawing-panes.md  (declare with mode = "pane")
import os
import selectors
import sys
import time
from plumbing import notify, unpack

sel = selectors.DefaultSelector()
os.set_blocking(sys.stdin.fileno(), False)
sel.register(sys.stdin.fileno(), selectors.EVENT_READ)

buf = bytearray()
my_pane = None


def frames():
    """Whatever complete frames are on stdin right now."""
    while len(buf) >= 4:
        n = int.from_bytes(buf[:4], "big")
        if len(buf) < 4 + n:
            break
        value, _ = unpack(bytes(buf[4:4 + n]))
        del buf[:4 + n]
        yield value


def draw(pane):
    text = time.strftime("  %H:%M:%S  ")
    notify("grid.update", {
        "pane": pane,
        "rect": {"row": 0, "col": 0, "rows": 1, "cols": len(text)},
        "chars": list(text),               # one grapheme cluster per cell
        "fg": [0x00FF00] * len(text),       # green
    })


running = True
while running:
    # wake on stdin, but never sleep longer than a second: the clock ticks
    for _ in sel.select(timeout=1.0):
        try:
            chunk = os.read(sys.stdin.fileno(), 65536)
        except BlockingIOError:
            continue
        if not chunk:
            running = False
            break
        buf.extend(chunk)
        for kind, id_, method, params in frames():
            if kind == 0 and method == "spice.lifecycle.hello":
                my_pane = params.get("pane")
                notify("ready", {"protocol": "0.1", "subscribe": []})
            elif kind == 0 and method == "spice.lifecycle.shutdown":
                running = False

    if my_pane is not None and running:
        draw(my_pane)   # once a second, whether or not anything arrived

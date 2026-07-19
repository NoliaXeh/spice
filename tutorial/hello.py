#!/usr/bin/env python3
# tutorial/hello.py — see 01-hello-spice.md
from plumbing import notify, read_frame

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": [],
        })
        notify("status.message", {"text": "hello from python!"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break

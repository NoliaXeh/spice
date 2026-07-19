#!/usr/bin/env python3
# tutorial/pong.py — see 09-plugin-to-plugin.md
from plumbing import notify, read_frame

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["demo.ping"],          # another plugin's topic
        })

    elif kind == 0 and method == "demo.ping":
        notify("status.message",
               {"text": f"pong! (from plugin {params['source']})"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break

#!/usr/bin/env python3
# tutorial/ping.py — see 09-plugin-to-plugin.md
from plumbing import notify, read_frame

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "publishes": ["demo.ping"],          # documentation, discoverable
            "commands": [{"name": "ping", "title": "Send a ping"}],
        })

    elif kind == 0 and method == "spice.palette.command_invoked":
        if params["command"] == "ping":
            notify("broadcast", {
                "topic": "demo.ping",
                "payload": {"when": "now", "from": "ping"},
            })

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break

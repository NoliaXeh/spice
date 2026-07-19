#!/usr/bin/env python3
# tutorial/fortune.py — see 02-commands-and-keys.md
import random
from plumbing import notify, read_frame

FORTUNES = [
    "your build will pass on the first try",
    "a segfault you did not write awaits",
    "someone will finally read your commit messages",
    "the bug is in the code you are most proud of",
]

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.palette.command_invoked"],
            "commands": [
                {"name": "tell", "title": "Tell my fortune"},
            ],
        })
        notify("keybind.set", {
            "key": "f6", "plugin": "fortune", "command": "tell",
        })

    elif kind == 0 and method == "spice.palette.command_invoked":
        if params["command"] == "tell":
            notify("status.message", {"text": random.choice(FORTUNES)})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break

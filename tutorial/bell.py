#!/usr/bin/env python3
# tutorial/bell.py — see 05-listening-to-events.md
from plumbing import notify, read_frame

seen = set()

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.buffer.", "spice.pane.focused"],
        })

    elif kind == 0 and method == "spice.buffer.created":
        if params["buffer"] not in seen:       # replays repeat: upsert
            seen.add(params["buffer"])
            notify("log", {"level": "info",
                           "message": f"buffer {params['buffer']} appeared"
                                      f" ({params['caps']})"})

    elif kind == 0 and method == "spice.buffer.changed":
        splices = params.get("splices")
        notify("log", {"level": "debug", "message":
            f"buffer {params['buffer']}"
            f" v{params['from_version']} -> v{params['to_version']}"
            + (f", {len(splices)} splice(s)" if splices is not None
               else ", too many changes: refetch")})

    elif kind == 0 and method == "spice.pane.focused":
        notify("log", {"level": "debug",
                       "message": f"pane {params['pane']} focused"})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break

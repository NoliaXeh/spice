# 09 — Plugin to plugin

Plugins can talk to each other, with the core as a dumb relay. This
tutorial is two tiny plugins: one *broadcasts* an event, the other
*subscribes* to it — the same subscription mechanism you already use for
core events, pointed at a plugin's own topic.

## The broadcaster

```python
#!/usr/bin/env python3
# tutorial/ping.py
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
```

## The listener

```python
#!/usr/bin/env python3
# tutorial/pong.py
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
```

Run both, invoke *Send a ping* — the log shows `pong! (from plugin 1)`.

```toml
[[plugin]]
name    = "ping"
command = ["python3", "tutorial/ping.py"]

[[plugin]]
name    = "pong"
command = ["python3", "tutorial/pong.py"]
```

## What's new

**One namespace, one filter.** You *publish* with the `broadcast` notify,
but a subscriber receives an ordinary **event** on your topic — the same
kind-0 messages as core events, distinguished only by the topic prefix.
There is nothing new to learn on the receiving side; `pong` subscribes to
`"demo.ping"` exactly as it would to `"spice.buffer."`.

**The core is a dumb broker.** It routes on the topic prefix, stamps the
`source` plugin id, and forgets. Your `payload` is opaque — any msgpack
value survives the trip, uninspected.

**Three rules that matter:**

- **`spice.` is reserved.** A broadcast on a `spice.` topic is dropped, so
  a plugin can never forge a core event.
- **You never receive your own broadcast**, even subscribed to your own
  topic — the cheapest loop-breaker there is.
- **100 broadcasts per second, per plugin.** Past that, the rest of the
  second is dropped and logged. A ping-pong loop throttles instead of
  melting the session.

**No request/response over broadcast, ever.** If `ping` needs an answer
from `pong`, that is two broadcasts and a correlation id *inside your
payload* — and `ping` must tolerate never hearing back. Letting one plugin
block on another would reinvent synchronous IPC through the core and hand
it liveness obligations it cannot honor (what if `pong` is dead? slow?).

**`publishes`** in `ready` is informational — it does not gate anything
(undeclared topics broadcast fine) — but it makes your topics discoverable
by other plugins via the `topics.list` request. Declare what you emit;
future-you writing the companion plugin will thank present-you.

## The honest warning

Broadcast can quietly become the *real* API of your plugin ecosystem, with
plugins coupling over undocumented topics — a second, unspecified protocol
inside the specified one. This happened to Emacs, and Emacs is fine, but
"what topics exist" becomes social convention. Document your topics; keep
payloads versioned if they will travel far.

Next: [10 — Shipping it](10-shipping-it.md): the difference between a demo
and a plugin someone can rely on.

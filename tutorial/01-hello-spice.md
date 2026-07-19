# 01 — Hello, Spice

The smallest possible plugin: it starts, shakes hands, says hello in the
status log, and leaves politely when asked. Everything later builds on
these thirty lines.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/hello.py
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
```

Run it:

```toml
# tutorial/try.toml
[[plugin]]
name    = "hello"
command = ["python3", "tutorial/hello.py"]
```

```
./build/spice --config tutorial/try.toml
```

The event-log pane (bottom right) shows `hello: hello from python!`.
That's a plugin.

## What just happened

**Frames.** Spice and your plugin exchange *frames* on stdin/stdout: a
4-byte big-endian length, then that many bytes of msgpack. `plumbing.py`'s
`read_frame`/`write_frame` do exactly that and nothing else — read them
once, then never think about it again.

**The envelope.** Every frame decodes to the same 4-element array:

```
[kind, id, method, params]
```

| kind | meaning | direction |
|------|---------|-----------|
| 0 | event | Spice → you |
| 1 | notify | you → Spice, never answered |
| 2 | request | you → Spice, answered by a... |
| 3 | response | Spice → you, carries your request's id |
| 4 | error | Spice → you: your notify was bad |

You only ever *send* kinds 1 and 2. The `while` loop above is the whole
architecture of every plugin: read a frame, decide, maybe write frames.

**The handshake.** The first thing Spice sends is
`spice.lifecycle.hello` — your id, your name from the config, your mode.
Nothing you send counts until you answer with the `ready` notify:
protocol version (send `"0.1"`, matched on the major part, strictly) and
the event topics you want. We subscribed to nothing: this plugin is deaf
by choice, and that is fine.

**Saying something.** `status.message` puts a line in the event log,
prefixed with your plugin's name. It is the `printf` of plugin
development.

**Leaving.** `spice.lifecycle.shutdown` arrives when Spice quits (it comes
even with an empty subscribe list — lifecycle events always do). Exit
promptly; dawdle past the grace period and you get SIGTERM, then SIGKILL.

## When it "does nothing"

The number one cause: something printed to **stdout** and corrupted the
frame stream. Debug on stderr — it shows up in the log pane:

```python
import sys
print("I am alive", file=sys.stderr)
```

## Exercise

Make the greeting count: keep a counter in a file next to the plugin and
report "hello for the Nth time". You now know everything you need.

Next: [02 — Commands and keys](02-commands-and-keys.md), where the user
gets to *invoke* you.

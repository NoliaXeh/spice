# Writing Spice plugins, one step at a time

A graded series: each tutorial is a complete, runnable plugin that adds one
idea to the previous one. By the end you will have touched every part of
the protocol a real plugin uses.

A Spice plugin is **a program** - Spice starts it, writes protocol messages
to its stdin and reads messages from its stdout. These tutorials use
Python, but nothing in them is Python-specific; see
[PLUGIN.md](../PLUGIN.md) for the protocol itself and
[PROTOCOL.md](../PROTOCOL.md) for the design behind it.

## What you need

- Python 3.8+ (standard library only - no pip installs).
- [`plumbing.py`](plumbing.py), sitting next to your plugin: the msgpack
  encoding and the length-prefixed framing every tutorial shares. Tutorial
  01 explains what it does; after that you get to take it for granted.

## The series

| # | Tutorial | You learn |
|---|----------|-----------|
| 01 | [Hello, Spice](01-hello-spice.md) | frames, the envelope, the handshake |
| 02 | [Commands and keys](02-commands-and-keys.md) | palette commands, events, keybinds |
| 03 | [Reading buffers](03-reading-buffers.md) | requests, responses, failures |
| 04 | [Editing buffers](04-editing-buffers.md) | versions, splices, staleness |
| 05 | [Listening to events](05-listening-to-events.md) | subscriptions, the event stream |
| 06 | [Syntax highlighting](06-syntax-highlighting.md) | highlights, layers, byte columns |
| 07 | [Marks](07-marks.md) | positions that survive edits |
| 08 | [Drawing panes](08-drawing-panes.md) | pane mode, the grid, timers |
| 09 | [Plugin to plugin](09-plugin-to-plugin.md) | broadcast, topics |
| 10 | [Shipping it](10-shipping-it.md) | lifecycle, debugging, robustness |

## Running a tutorial plugin

Every tutorial ends with the same move: declare the plugin in a config
file and start Spice with it. From the repository root:

```toml
# tutorial/try.toml
[[plugin]]
name    = "hello"
command = ["python3", "tutorial/hello.py"]
```

```
./build/spice --config tutorial/try.toml
```

`--config` bypasses your real configuration, so experiments stay
experiments. The **event-log pane** (bottom right) is your friend: status
messages, log lines, and your plugin's stderr all land there.

## The three rules you will break anyway

1. **Never print to stdout.** Stdout *is* the protocol; one stray
   `print()` corrupts the stream. Debug with `print(..., file=sys.stderr)`
   - stderr is captured and shown in the log pane.
2. **Flush after every write.** `plumbing.write_frame` does it for you;
   remember it when you write your own.
3. **Send `ready` before anything else.** Until the handshake, Spice
   ignores you.

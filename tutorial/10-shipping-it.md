# 10 — Shipping it

Everything so far was a demo. This last tutorial is about the difference
between a demo and a plugin someone else can rely on: the lifecycle done
right, failure handled, and the config knobs that decide how your plugin
behaves when things go wrong.

## The lifecycle, in full

```
spawn                → your process starts
event hello          → who you are; you answer with ready
   (you run)
event shutdown       → { grace_ms }; finish and exit
SIGTERM              → if you overstayed grace_ms
SIGKILL              → if you ignored SIGTERM
```

Two things people get wrong:

- **Exit promptly on shutdown.** You have `grace_ms` to flush work in
  progress — Spice keeps reading your notifies during it, so a save on the
  way out completes — but then it escalates. Don't linger.
- **A crash before `ready` is still reported.** If you throw during
  startup, Spice tells the user *and* logs your stderr. A plugin that dies
  in silence is a plugin nobody can debug — so let your stderr talk.

## When you crash, do you come back?

That is the `restart` policy, set by the *user* in config, but you should
know how it behaves:

```toml
[[plugin]]
name    = "myplugin"
command = ["python3", "myplugin.py"]
restart = "on-crash"        # never | on-crash (default) | always
max_restarts   = 3          # within the window, then Spice gives up
restart_window = "60s"
```

- `on-crash` relaunches you unless you exited cleanly (status 0). A clean
  exit stays dead — the way to say "I'm done, don't restart me."
- `always` relaunches even a clean exit.
- After `max_restarts` inside `restart_window`, Spice stops trying and
  tells the user. A crash-loop throttles instead of pinning a CPU.

A relaunch is a *fresh process*: you get `hello` again and must
re-register everything. Keep your startup idempotent.

## Robustness, concretely

**Check every response for `code`.** Buffers die, panes close, ids go
stale between your request and Spice's answer. The plugin that assumes
success works until the first race.

```python
if "code" in params:
    ...   # handle it; do not index params["lines"] and crash
```

**Never block the read loop.** Slow work (a subprocess, a network call, a
compile) goes on a thread or behind a `selectors` timeout (tutorial 08),
never in a `sleep` that stops you reading stdin. A plugin that stops
reading backs up Spice's pipe, and Spice starts dropping messages to you.

**Guard your own bugs.** One uncaught exception ends your process. In a
long-running plugin, wrap the per-message handler:

```python
for frame in frames:
    try:
        handle(frame)
    except Exception as e:
        print(f"handler error: {e}", file=sys.stderr)   # into the log
```

Different process, different memory — your crash cannot take Spice down.
But your *state* is gone, so failing one message beats failing the
process.

**Debounce expensive reactions.** A highlighter that refetches on every
keystroke is fine; one that reparses a 10k-line file on every keystroke is
not. Coalesce: on `spice.buffer.changed`, set a "dirty" flag and do the
work on a short `selectors` timeout, so a burst of keystrokes costs one
recompute, not fifty.

## Debugging checklist

When a plugin "does nothing", in order of likelihood:

1. **Something printed to stdout.** One stray `print()` corrupts the frame
   stream. Everything after it desyncs. Debug on **stderr**.
2. **You forgot to flush.** `plumbing.write_frame` flushes; your own
   writes must too.
3. **You never sent `ready`** — or sent it with the wrong `protocol` major
   version. Nothing is honored before a successful handshake.
4. **You forgot to subscribe.** No subscription, no events; the plugin
   looks dead.
5. **Check the log pane and your stderr.** Set `[log] level = "debug"` in
   config. Your stderr, status lines, and log notifies all land there.

## Packaging

Ship the plugin as a single executable (a script with `#!/usr/bin/env
python3` and the execute bit, or `["python3", "yourplugin.py"]` in the
config). `command` is an argv array — no shell, no quoting, no `$PATH`
surprises beyond the usual `execvp` lookup. Your plugin `name` is the
namespace you own: broadcast topics and error codes should start with it.

## Where to go next

You have now used every part of the protocol a real plugin touches. The
two reference plugins in this repository are the same ideas at full size:

- [`plugins/cpp-keywords`](../plugins/cpp-keywords/cpp_keywords.py) —
  tutorial 06, shipped.
- [`plugins/lsp-highlight`](../plugins/lsp-highlight/lsp_highlight.py) — a
  full LSP bridge: tutorials 03–06 plus a second protocol (JSON-RPC over
  the language server's stdio) and the UTF-16→byte column conversion.

For the whole method vocabulary and every event, keep
[PLUGIN.md](../PLUGIN.md) open; for *why* the protocol is shaped this way,
read [PROTOCOL.md](../PROTOCOL.md). Now go build something.

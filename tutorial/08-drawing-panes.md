# 08 — Drawing panes

Every plugin so far worked *through* buffers and the status line. This one
draws its own picture: a live clock in its own pane, redrawing itself once
a second. It introduces **pane mode**, the **grid**, and the loop that
wakes on input *or* on a timer.

## Pane mode

Declare a plugin `mode = "pane"` and Spice creates a dedicated grid pane
(and a backing buffer) for it *before* the handshake, handing you their
ids in the hello event:

```toml
[[plugin]]
name    = "clock"
command = ["python3", "tutorial/clock.py"]
mode    = "pane"
```

```
event  spice.lifecycle.hello  { ..., mode: "pane", pane: 4, buffer: 9 }
```

That `pane` is yours to draw into.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/clock.py
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
```

Start Spice — a green clock ticks in the plugin's pane.

## What's new

**The grid is four parallel layers** over the same rectangle: `chars`
(one grapheme cluster per cell — a *string* per cell, so `é` and emoji
work), `fg`, `bg` (both `0xRRGGBB`), and `style` (a bitfield: 1 bold,
2 italic, 4 underline, 8 strikethrough, 16 reverse, 64 blink). Every layer
is optional; send only what changed. A present layer must be flat,
row-major, and exactly `rows × cols` long — get the length wrong and the
update is rejected as `bad_params`.

**Dirty rectangles.** `rect` is *what changed*, not the whole pane. A
blinking cursor is a 1×1 update, not 80×24. There is no flush call — a
`grid.update` *is* the update, and the core coalesces several within a
frame so a chatty plugin can't tear the screen.

**Coordinates are the pane's content area**, whose size you learn from
`spice.pane.resized`. On a resize the surface is cleared — redraw it then.
`grid.clear` drops your surface and returns the pane to its buffer;
`grid.set_cursor {pane, pos, visible}` parks the cursor.

**Wide characters** take two cells: the cluster in the first, an **empty
string** in the second as the continuation marker. This convention is
mandatory — skip it and every wide glyph shoves the rest of the row over.

**The timer loop is the real lesson.** `selectors.select(timeout=1.0)`
returns when stdin has data *or* after a second, whichever comes first —
so the plugin stays responsive to Spice and still ticks on its own
schedule. This is the pattern for anything that must act without being
prompted: a poll, a debounce, an animation. Never `time.sleep()` in a way
that stops you reading stdin; a plugin that stops reading gets its pipe
backed up and its messages dropped.

## Exercise

Add a `bg` layer that pulses (alternate two dark colors each second), and
handle `spice.pane.resized` to center the clock in the new width. You are
one animation loop away from a status bar.

Next: [09 — Plugin to plugin](09-plugin-to-plugin.md): talking to *other*
plugins, not just the core.

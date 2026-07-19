# 06 — Syntax highlighting

The first plugin that changes what the user *sees*: it colors every
occurrence of a set of words, live, as you type. This is the real
`cpp-keywords` plugin in miniature, and it is your introduction to
`buffer.set_highlights`.

## The whole plugin

```python
#!/usr/bin/env python3
# tutorial/paint.py
import re
from plumbing import notify, read_frame, request

PINK = 0xFF69B4
WORDS = ["TODO", "FIXME", "XXX", "HACK", "NOTE"]
WORD_RE = re.compile(rb"(?:" + b"|".join(w.encode() for w in WORDS) + rb")")

tracked = set()   # buffers we are painting
pending = {}      # request id -> buffer

def repaint(buffer):
    pending[request("buffer.get_lines",
                    {"buffer": buffer, "start": 0, "end": -1})] = buffer

def spans(lines):
    out = []
    for lineno, line in enumerate(lines):
        for m in WORD_RE.finditer(line.encode()):   # bytes -> byte columns
            out.append({
                "range": {"start": {"line": lineno, "col": m.start()},
                          "end": {"line": lineno, "col": m.end()}},
                "fg": PINK,
            })
    return out

while (message := read_frame()) is not None:
    kind, id_, method, params = message

    if kind == 0 and method == "spice.lifecycle.hello":
        notify("ready", {
            "protocol": "0.1",
            "subscribe": ["spice.buffer.created", "spice.buffer.changed"],
        })

    elif kind == 0 and method == "spice.buffer.created":
        buffer = params["buffer"]
        tracked.add(buffer)
        repaint(buffer)

    elif kind == 0 and method == "spice.buffer.changed":
        if params["buffer"] in tracked:
            repaint(params["buffer"])   # recolor after every edit

    elif kind == 3 and id_ in pending:
        buffer = pending.pop(id_)
        if "code" in params:
            tracked.discard(buffer)
        else:
            notify("buffer.set_highlights",
                   {"buffer": buffer, "highlights": spans(params["lines"])})

    elif kind == 0 and method == "spice.lifecycle.shutdown":
        break
```

Open any file, scatter some TODOs and FIXMEs — they turn pink, and stay
pink as you edit around them.

## What's new

**`buffer.set_highlights` is a notify, not a request** — you tell Spice to
paint; there is no reply. `fg` is `0xRRGGBB`. Ranges are byte-addressed,
exactly like splices — hence `finditer(line.encode())` again. Selection
beats decoration: highlighted text under a selection shows the selection
color.

**Highlights are decoration, not truth.** The core never rebases them:
after an edit your spans sit on slightly-wrong bytes until you recompute.
That is why you subscribe to `spice.buffer.changed` and repaint — and why
a stale frame of color is fine where a stale *write* never is.

**Layers, and why order matters.** Each plugin owns *one layer* per
buffer; `set_highlights` replaces only *your* layer. Layers stack in the
order plugins are declared in `config.toml` — later entries paint over
earlier ones, earlier ones show through the gaps. So this is exactly how
you compose a coarse highlighter with a precise one:

```toml
[[plugin]]                       # declared first: the fallback coat
name    = "paint"
command = ["python3", "tutorial/paint.py"]

[[plugin]]                       # declared second: paints on top
name    = "lsp-highlight"
command = ["python3", "plugins/lsp-highlight/lsp_highlight.py", "clangd"]
```

Where the LSP says nothing, your pink shows through; where both speak, the
LSP wins. Swap the blocks to flip the priority.

**Sending an empty list drops your layer** — the way to clear your
highlighting without disturbing anyone else's.

## From regex to real syntax

Regexes color keywords-in-strings by mistake; they do not know a type from
a variable. The grown-up version asks a language server for *semantic
tokens* and paints those — see
[`plugins/lsp-highlight`](../plugins/lsp-highlight/lsp_highlight.py), which
does exactly what this tutorial does but with clangd's understanding
instead of a word list (and handles LSP's UTF-16 columns, converting them
to bytes before they ever reach Spice).

## Exercise

Give each word its own color (TODO green, FIXME red, HACK orange). One
`set_highlights` call still carries them all — just vary `fg` per span.

Next: [07 — Marks](07-marks.md): remembering a *place* that survives the
user's edits.

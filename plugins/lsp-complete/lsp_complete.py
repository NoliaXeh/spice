#!/usr/bin/env python3
"""lsp-complete: autocomplete from any LSP server.

A bridge like lsp-highlight: it speaks the Spice plugin protocol on its own
stdin/stdout and JSON-RPC to a language server you name on the command
line. On the `complete` command it asks the server what fits at the cursor,
replaces the half-typed identifier with the best match, and lets you cycle
through the rest with `complete_next` / `complete_prev`.

    [[plugin]]
    name    = "lsp-complete"
    command = ["python3", "/path/to/lsp_complete.py", "clangd"]

There is no modal popup: a Spice plugin cannot capture keystrokes (it cannot
veto input), so completion works by inserting-and-cycling, the way vim's
ctrl-n does. A status line shows "getValue (1/8)" as you cycle. Bind the
three commands to keys for a fast loop.

Self-contained: stdlib only, msgpack subset inlined.
"""

import json
import os
import re
import selectors
import struct
import subprocess
import sys

IDENT = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*\Z")  # the word ending at the cursor
MAX_ITEMS = 25

LANGUAGES = {
    ".c": "c", ".h": "cpp", ".cpp": "cpp", ".cc": "cpp", ".cxx": "cpp",
    ".hpp": "cpp", ".hxx": "cpp", ".hh": "cpp", ".ipp": "cpp", ".inl": "cpp",
    ".py": "python", ".rs": "rust", ".go": "go", ".js": "javascript",
    ".ts": "typescript", ".java": "java",
}

# --- msgpack (the subset Spice speaks) -----------------------------------

def pack(value):
    if value is None:
        return b"\xc0"
    if value is True:
        return b"\xc3"
    if value is False:
        return b"\xc2"
    if isinstance(value, int):
        if 0 <= value <= 0x7F:
            return struct.pack("B", value)
        if -32 <= value < 0:
            return struct.pack("b", value)
        if 0 <= value <= 0xFFFFFFFFFFFFFFFF:
            return b"\xcf" + struct.pack(">Q", value)
        return b"\xd3" + struct.pack(">q", value)
    if isinstance(value, str):
        data = value.encode()
        if len(data) <= 31:
            return struct.pack("B", 0xA0 | len(data)) + data
        if len(data) <= 0xFF:
            return b"\xd9" + struct.pack(">B", len(data)) + data
        return b"\xda" + struct.pack(">H", len(data)) + data
    if isinstance(value, (list, tuple)):
        head = (struct.pack("B", 0x90 | len(value)) if len(value) <= 15
                else b"\xdc" + struct.pack(">H", len(value)))
        return head + b"".join(pack(v) for v in value)
    if isinstance(value, dict):
        head = (struct.pack("B", 0x80 | len(value)) if len(value) <= 15
                else b"\xde" + struct.pack(">H", len(value)))
        return head + b"".join(pack(k) + pack(v) for k, v in value.items())
    raise TypeError(f"cannot pack {type(value)}")


def unpack(data, at=0):
    tag = data[at]
    at += 1
    if tag <= 0x7F:
        return tag, at
    if tag >= 0xE0:
        return tag - 0x100, at
    if 0xA0 <= tag <= 0xBF:
        n = tag & 0x1F
        return data[at:at + n].decode(errors="replace"), at + n
    if 0x90 <= tag <= 0x9F:
        return _seq(data, at, tag & 0x0F, False)
    if 0x80 <= tag <= 0x8F:
        return _seq(data, at, tag & 0x0F, True)
    if tag == 0xC0:
        return None, at
    if tag == 0xC2:
        return False, at
    if tag == 0xC3:
        return True, at
    if tag in (0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3):
        w = 1 << ((tag - 0xCC) & 0x03)
        return int.from_bytes(data[at:at + w], "big", signed=tag >= 0xD0), at + w
    if tag == 0xCB:
        return struct.unpack(">d", data[at:at + 8])[0], at + 8
    if tag in (0xD9, 0xDA, 0xDB, 0xC4, 0xC5, 0xC6):
        w = {0xD9: 1, 0xDA: 2, 0xDB: 4, 0xC4: 1, 0xC5: 2, 0xC6: 4}[tag]
        n = int.from_bytes(data[at:at + w], "big")
        at += w
        raw = data[at:at + n]
        text = raw.decode(errors="replace") if tag in (0xD9, 0xDA, 0xDB) else raw
        return text, at + n
    if tag in (0xDC, 0xDD, 0xDE, 0xDF):
        w = 2 if tag in (0xDC, 0xDE) else 4
        n = int.from_bytes(data[at:at + w], "big")
        return _seq(data, at + w, n, tag >= 0xDE)
    raise ValueError(f"bad msgpack tag {tag:#x}")


def _seq(data, at, count, is_map):
    if is_map:
        out = {}
        for _ in range(count):
            k, at = unpack(data, at)
            v, at = unpack(data, at)
            out[k] = v
        return out, at
    out = []
    for _ in range(count):
        v, at = unpack(data, at)
        out.append(v)
    return out, at

# --- the Spice side ------------------------------------------------------

def spice_send(message):
    payload = pack(message)
    sys.stdout.buffer.write(len(payload).to_bytes(4, "big") + payload)
    sys.stdout.buffer.flush()


def notify(method, params):
    spice_send([1, None, method, params])


_next_spice_id = 0


def request(method, params):
    global _next_spice_id
    _next_spice_id += 1
    spice_send([2, _next_spice_id, method, params])
    return _next_spice_id


def take_frames(buf):
    while len(buf) >= 4:
        n = int.from_bytes(buf[:4], "big")
        if len(buf) < 4 + n:
            break
        value, _ = unpack(bytes(buf[4:4 + n]))
        del buf[:4 + n]
        yield value


def status(text):
    notify("status.message", {"text": text})

# --- columns: bytes <-> LSP's UTF-16 units -------------------------------

def byte_to_utf16(line, byte):
    # `byte` always lands on a character boundary (the core hands us the
    # cursor as a utf8 offset), so this slice never splits a codepoint
    prefix = line.encode()[:byte].decode(errors="replace")
    return len(prefix.encode("utf-16-le")) // 2

# --- the LSP side --------------------------------------------------------

class Lsp:
    def __init__(self, argv):
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self.incoming = bytearray()
        self.next_id = 1
        self.pending = {}      # lsp id -> ("complete", context)
        self.ready = False
        self.queued = []

    def _write(self, message):
        payload = json.dumps(message).encode()
        try:
            self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(payload) + payload)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def notify(self, method, params):
        msg = {"jsonrpc": "2.0", "method": method, "params": params}
        (self._write if self.ready or method == "initialized" else self.queued.append)(msg)

    def call(self, method, params):
        id_ = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": id_, "method": method, "params": params}
        (self._write if self.ready else self.queued.append)(msg)
        return id_

    def initialize(self):
        self._write({
            "jsonrpc": "2.0", "id": 0, "method": "initialize",
            "params": {
                "processId": os.getpid(),
                "rootUri": "file://" + os.getcwd(),
                "capabilities": {
                    "textDocument": {"completion": {"completionItem": {
                        "snippetSupport": False,
                    }}},
                },
            },
        })

    def on_initialized(self):
        self._write({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        self.ready = True
        for msg in self.queued:
            self._write(msg)
        self.queued.clear()

    def messages(self):
        out = []
        while True:
            head = self.incoming.find(b"\r\n\r\n")
            if head < 0:
                break
            length = 0
            for line in bytes(self.incoming[:head]).split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    length = int(line.split(b":")[1])
            if len(self.incoming) < head + 4 + length:
                break
            payload = bytes(self.incoming[head + 4:head + 4 + length])
            del self.incoming[:head + 4 + length]
            try:
                out.append(json.loads(payload))
            except json.JSONDecodeError:
                pass
        return out

# --- completion items ----------------------------------------------------

def candidate_text(item):
    """The plain text an item inserts (snippets flattened to their label)."""
    fmt = item.get("insertTextFormat", 1)
    if "textEdit" in item and fmt == 1:
        return item["textEdit"].get("newText", item["label"])
    if fmt == 2:            # a snippet: we asked not to get these, but be safe
        return item["label"].split("(")[0]
    return item.get("insertText") or item["label"]


def rank(items):
    """LSP order via sortText, capped."""
    items = sorted(items, key=lambda i: (i.get("sortText") or i["label"]))
    seen, out = set(), []
    for it in items:
        text = candidate_text(it)
        if text and text not in seen:
            seen.add(text)
            out.append((it["label"], text))
        if len(out) >= MAX_ITEMS:
            break
    return out

# --- the bridge ----------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: lsp_complete.py <server> [args...]\n")
        return 1
    lsp = Lsp(sys.argv[1:])
    lsp.initialize()

    docs = {}      # spice buffer -> {"uri", "lang", "version", "lines"}
    pending = {}   # spice request id -> ("step", context dict)
    session = None # active completion: replace-range, candidates, index, version

    def track(buffer, name):
        lang = LANGUAGES.get(os.path.splitext(name)[1])
        if lang:
            docs[buffer] = {"uri": "file://" + os.path.abspath(name),
                            "lang": lang, "version": 0, "lines": None}

    def sync(buffer, lines):
        d = docs[buffer]
        d["lines"] = lines
        d["version"] += 1
        text = "\n".join(lines)
        if d["version"] == 1:
            lsp.notify("textDocument/didOpen", {"textDocument": {
                "uri": d["uri"], "languageId": d["lang"], "version": 1, "text": text}})
        else:
            lsp.notify("textDocument/didChange", {
                "textDocument": {"uri": d["uri"], "version": d["version"]},
                "contentChanges": [{"text": text}]})

    def begin_complete(pane):
        # step 1: where is the cursor? (carry the pane through the chain)
        pending[request("pane.info", {"pane": pane})] = ("cursor", {"pane": pane})

    def apply(sess, index):
        """Replace the current insertion with candidate `index`; move cursor."""
        label, text = sess["candidates"][index]
        start = sess["start_byte"]
        end = sess["end_byte"]
        rng = {"start": {"line": sess["line"], "col": start},
               "end": {"line": sess["line"], "col": end}}
        pending[request("buffer.splice", {
            "buffer": sess["buffer"], "range": rng, "text": text,
            "version": sess["version"],
        })] = ("splice", {"index": index, "label": label, "text": text})

    def cycle(step):
        nonlocal session
        if session is None:
            return
        session["index"] = (session["index"] + step) % len(session["candidates"])
        apply(session, session["index"])

    def handle_completion_result(ctx, result):
        nonlocal session
        items = result.get("items", result) if isinstance(result, dict) else result
        if not items:
            status("no completions")
            return
        cands = rank(items)
        if not cands:
            status("no completions")
            return
        line_bytes = ctx["line"].encode()
        m = IDENT.search(line_bytes[:ctx["cursor_byte"]])
        start = m.start() if m else ctx["cursor_byte"]
        session = {
            "buffer": ctx["buffer"], "pane": ctx["pane"], "line": ctx["cursor_line"],
            "start_byte": start, "end_byte": ctx["cursor_byte"],
            "candidates": cands, "index": 0, "version": ctx["version"],
        }
        apply(session, 0)

    def handle_spice(frame):
        nonlocal session
        kind, id_, method, params = frame
        if kind == 0 and method == "spice.lifecycle.hello":
            notify("ready", {
                "protocol": "0.1",
                "subscribe": ["spice.buffer.created", "spice.buffer.changed",
                              "spice.palette.command_invoked"],
                "commands": [
                    {"name": "complete", "title": "Complete (LSP)"},
                    {"name": "complete_next", "title": "Complete: next"},
                    {"name": "complete_prev", "title": "Complete: previous"},
                ],
            })
        elif kind == 0 and method == "spice.lifecycle.shutdown":
            return False
        elif kind == 0 and method == "spice.buffer.created":
            pending[request("buffer.info", {"buffer": params["buffer"]})] = (
                "info", {"buffer": params["buffer"]})
        elif kind == 0 and method == "spice.buffer.changed":
            buffer = params["buffer"]
            # our own splice bumps the version we recorded; anything else is
            # the user typing, which ends the completion session
            if session and session["buffer"] == buffer and \
                    params.get("to_version") != session.get("version"):
                session = None
            if buffer in docs:
                pending[request("buffer.get_lines",
                                {"buffer": buffer, "start": 0, "end": -1})] = (
                    "resync", {"buffer": buffer})
        elif kind == 0 and method == "spice.palette.command_invoked":
            cmd = params["command"]
            if cmd == "complete" and "pane" in params:
                begin_complete(params["pane"])
            elif cmd == "complete_next":
                cycle(1)
            elif cmd == "complete_prev":
                cycle(-1)
        elif kind == 3 and id_ in pending:
            handle_response(id_, params)
        return True

    def handle_response(id_, params):
        nonlocal session
        step, ctx = pending.pop(id_)

        if "code" in params:            # any failure ends the attempt quietly
            if step == "splice" and params.get("code") == "spice.core.stale_version":
                session = None
            return

        if step == "info":
            # buffer.info gives the name; track it if it's a language we know
            if params.get("name"):
                track(ctx["buffer"], params["name"])
                if ctx["buffer"] in docs:
                    pending[request("buffer.get_lines",
                                    {"buffer": ctx["buffer"], "start": 0, "end": -1})] = (
                        "resync", {"buffer": ctx["buffer"]})
        elif step == "resync":
            if ctx["buffer"] in docs:
                sync(ctx["buffer"], params["lines"])
        elif step == "cursor":
            # got the cursor; now fetch the buffer text at that moment
            pending[request("buffer.get_lines",
                            {"buffer": params["buffer"], "start": 0, "end": -1})] = (
                "text", {"buffer": params["buffer"], "pane": ctx["pane"],
                         "cursor_line": params["cursor"]["line"],
                         "cursor_byte": params["cursor"]["col"]})
        elif step == "text":
            buffer = ctx["buffer"]
            if buffer not in docs:
                return
            lines = params["lines"]
            sync(buffer, lines)
            line_index = ctx["cursor_line"]
            line = lines[line_index] if line_index < len(lines) else ""
            character = byte_to_utf16(line, ctx["cursor_byte"])
            lsp.pending[lsp.call("textDocument/completion", {
                "textDocument": {"uri": docs[buffer]["uri"]},
                "position": {"line": line_index, "character": character},
            })] = {"buffer": buffer, "pane": ctx["pane"], "line": line,
                   "cursor_line": line_index, "cursor_byte": ctx["cursor_byte"],
                   "version": params["version"]}
        elif step == "splice":
            # the insertion landed: advance our state and place the cursor
            if session is not None:
                session["version"] = params["version"]
                session["end_byte"] = session["start_byte"] + len(ctx["text"].encode())
                notify("pane.set_cursor", {"pane": session["pane"], "pos": {
                    "line": session["line"], "col": session["end_byte"]}})
                n = len(session["candidates"])
                status(f"{ctx['label'].strip()}  ({ctx['index'] + 1}/{n})")

    def handle_lsp(msg):
        if msg.get("id") == 0 and "result" in msg:
            lsp.on_initialized()
        elif msg.get("id") in lsp.pending:
            ctx = lsp.pending.pop(msg["id"])
            result = msg.get("result")
            if result:
                handle_completion_result(ctx, result)
            else:
                status("no completions")
        elif "id" in msg and "method" in msg:
            items = msg.get("params", {}).get("items", [])
            res = [None] * len(items) if msg["method"] == "workspace/configuration" else None
            lsp._write({"jsonrpc": "2.0", "id": msg["id"], "result": res})

    sel = selectors.DefaultSelector()
    os.set_blocking(sys.stdin.fileno(), False)
    os.set_blocking(lsp.proc.stdout.fileno(), False)
    sel.register(sys.stdin.fileno(), selectors.EVENT_READ, "spice")
    sel.register(lsp.proc.stdout.fileno(), selectors.EVENT_READ, "lsp")
    spice_buf = bytearray()

    running = True
    while running:
        for key, _ in sel.select(timeout=1.0):
            try:
                chunk = os.read(key.fd, 65536)
            except (BlockingIOError, OSError):
                continue
            if key.data == "spice":
                if not chunk:
                    running = False
                    break
                spice_buf += chunk
                for frame in take_frames(spice_buf):
                    if not handle_spice(frame):
                        running = False
            else:
                if chunk:
                    lsp.incoming += chunk
                    for msg in lsp.messages():
                        handle_lsp(msg)

    lsp._write({"jsonrpc": "2.0", "id": 999999, "method": "shutdown", "params": None})
    lsp._write({"jsonrpc": "2.0", "method": "exit", "params": None})
    try:
        lsp.proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        lsp.proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())

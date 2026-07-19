#!/usr/bin/env python3
"""lsp-highlight: semantic syntax highlighting from any LSP server.

A bridge with two faces: it speaks the Spice plugin protocol on its own
stdin/stdout, and spawns a Language Server (whatever you name on the
command line) speaking LSP over the server's stdio. Semantic tokens from
the server become buffer.set_highlights spans in Spice - so the coloring
is the compiler's understanding of the code, not a pile of regexes.

Install (config.toml) - the server argv follows the script:

    [[plugin]]
    name    = "lsp-highlight"
    command = ["python3", "/path/to/lsp_highlight.py", "clangd"]

Works with any server that supports textDocument/semanticTokens/full
(clangd, rust-analyzer, pylsp, typescript-language-server, ...). The
UTF-16 column units LSP speaks by default are converted to the byte
columns Spice speaks right here, as PROTOCOL.md demands - that
conversion never crosses into the core.

Self-contained: stdlib only, msgpack subset included.
"""

import json
import os
import selectors
import struct
import subprocess
import sys

# What each semantic token type paints (0xRRGGBB). Types absent here -
# variables, parameters - keep the theme's text color: readable beats
# rainbow. Keywords are pink, of course.
COLORS = {
    "keyword": 0xFF69B4,
    "modifier": 0xFF69B4,
    "comment": 0x7F848E,
    "string": 0x98C379,
    "number": 0xD19A66,
    "regexp": 0xD19A66,
    "type": 0x56B6C2,
    "class": 0x56B6C2,
    "struct": 0x56B6C2,
    "enum": 0x56B6C2,
    "interface": 0x56B6C2,
    "typeParameter": 0x56B6C2,
    "concept": 0x56B6C2,
    "function": 0x61AFEF,
    "method": 0x61AFEF,
    "macro": 0xC678DD,
    "namespace": 0x4EC9B0,
    "enumMember": 0xE5C07B,
    "property": 0xE5C07B,
}

LANGUAGES = {
    ".c": "c", ".h": "cpp", ".cpp": "cpp", ".cc": "cpp", ".cxx": "cpp",
    ".hpp": "cpp", ".hxx": "cpp", ".hh": "cpp", ".ipp": "cpp", ".inl": "cpp",
    ".py": "python", ".rs": "rust", ".go": "go", ".js": "javascript",
    ".ts": "typescript", ".java": "java", ".rb": "ruby", ".sh": "shellscript",
}

# --- msgpack, the subset Spice speaks (as in cpp-keywords) ---------------

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
        return head + b"".join(pack(item) for item in value)
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
        return _sequence(data, at, tag & 0x0F, is_map=False)
    if 0x80 <= tag <= 0x8F:
        return _sequence(data, at, tag & 0x0F, is_map=True)
    if tag == 0xC0:
        return None, at
    if tag == 0xC2:
        return False, at
    if tag == 0xC3:
        return True, at
    if tag in (0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3):
        width = 1 << ((tag - 0xCC) & 0x03)
        raw = data[at:at + width]
        return int.from_bytes(raw, "big", signed=tag >= 0xD0), at + width
    if tag == 0xCB:
        return struct.unpack(">d", data[at:at + 8])[0], at + 8
    if tag in (0xD9, 0xDA, 0xDB, 0xC4, 0xC5, 0xC6):
        width = {0xD9: 1, 0xDA: 2, 0xDB: 4, 0xC4: 1, 0xC5: 2, 0xC6: 4}[tag]
        n = int.from_bytes(data[at:at + width], "big")
        at += width
        raw = data[at:at + n]
        text = raw.decode(errors="replace") if tag in (0xD9, 0xDA, 0xDB) else raw
        return text, at + n
    if tag in (0xDC, 0xDD, 0xDE, 0xDF):
        width = 2 if tag in (0xDC, 0xDE) else 4
        n = int.from_bytes(data[at:at + width], "big")
        return _sequence(data, at + width, n, is_map=tag >= 0xDE)
    raise ValueError(f"unsupported msgpack tag {tag:#x}")


def _sequence(data, at, count, is_map):
    if is_map:
        out = {}
        for _ in range(count):
            key, at = unpack(data, at)
            value, at = unpack(data, at)
            out[key] = value
        return out, at
    out = []
    for _ in range(count):
        value, at = unpack(data, at)
        out.append(value)
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


def take_spice_frames(buffer):
    """Complete [kind, id, method, params] frames off the front of buffer."""
    frames = []
    while len(buffer) >= 4:
        length = int.from_bytes(buffer[:4], "big")
        if len(buffer) < 4 + length:
            break
        value, _ = unpack(bytes(buffer[4:4 + length]))
        del buffer[:4 + length]
        frames.append(value)
    return frames

# --- the LSP side --------------------------------------------------------

class LspServer:
    """The child language server: JSON-RPC over its stdio."""

    def __init__(self, argv):
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self.incoming = bytearray()
        self.next_id = 1
        self.pending = {}       # request id -> spice buffer id awaiting tokens
        self.legend = []        # token type index -> name
        self.encoding = "utf-16"
        self.ready = False
        self.queued = []        # sends deferred until `initialized`

    def send(self, message):
        payload = json.dumps(message).encode()
        try:
            self.proc.stdin.write(
                b"Content-Length: %d\r\n\r\n" % len(payload) + payload
            )
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass  # the server died; Spice's restart policy is the recovery

    def notify(self, method, params):
        message = {"jsonrpc": "2.0", "method": method, "params": params}
        if self.ready or method == "initialized":
            self.send(message)
        else:
            self.queued.append(message)

    def request_tokens(self, uri, spice_buffer):
        id_ = self.next_id
        self.next_id += 1
        self.pending[id_] = spice_buffer
        message = {
            "jsonrpc": "2.0", "id": id_,
            "method": "textDocument/semanticTokens/full",
            "params": {"textDocument": {"uri": uri}},
        }
        if self.ready:
            self.send(message)
        else:
            self.queued.append(message)

    def initialize(self):
        token_types = sorted(COLORS)
        self.send({
            "jsonrpc": "2.0", "id": 0, "method": "initialize",
            "params": {
                "processId": os.getpid(),
                "rootUri": "file://" + os.getcwd(),
                "capabilities": {
                    "general": {"positionEncodings": ["utf-8", "utf-16"]},
                    "textDocument": {"semanticTokens": {
                        "requests": {"full": True},
                        "tokenTypes": token_types,
                        "tokenModifiers": [],
                        "formats": ["relative"],
                    }},
                },
            },
        })

    def handle_initialized(self, result):
        capabilities = result.get("capabilities", {})
        provider = capabilities.get("semanticTokensProvider") or {}
        self.legend = provider.get("legend", {}).get("tokenTypes", [])
        self.encoding = capabilities.get("positionEncoding", "utf-16")
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        self.ready = True
        for message in self.queued:
            self.send(message)
        self.queued.clear()

    def take_messages(self):
        """Complete JSON-RPC messages off the front of `incoming`."""
        messages = []
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
                messages.append(json.loads(payload))
            except json.JSONDecodeError:
                pass
        return messages

# --- columns: LSP units in, Spice bytes out ------------------------------

def to_byte_column(line, offset, encoding):
    """An LSP column (UTF-16 units by default) as a byte offset."""
    if encoding == "utf-8":
        return min(offset, len(line.encode()))
    units = 0
    byte = 0
    for char in line:
        if units >= offset:
            break
        units += len(char.encode("utf-16-le")) // 2
        byte += len(char.encode())
    return byte


def decode_tokens(data, legend, lines, encoding):
    """The flat delta-encoded token array as set_highlights spans."""
    spans = []
    line = 0
    column = 0
    for i in range(0, len(data) - 4, 5):
        delta_line, delta_column, length, token_type, _mods = data[i:i + 5]
        line += delta_line
        column = column + delta_column if delta_line == 0 else delta_column
        name = legend[token_type] if token_type < len(legend) else ""
        color = COLORS.get(name)
        if color is None or line >= len(lines):
            continue
        text = lines[line]
        start = to_byte_column(text, column, encoding)
        end = to_byte_column(text, column + length, encoding)
        if end > start:
            spans.append({
                "range": {"start": {"line": line, "col": start},
                          "end": {"line": line, "col": end}},
                "fg": color,
            })
    return spans

# --- the bridge ----------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: lsp_highlight.py <server> [args...]\n")
        return 1
    lsp = LspServer(sys.argv[1:])
    lsp.initialize()

    tracked = {}   # spice buffer id -> {"uri", "language", "version", "lines"}
    pending = {}   # spice request id -> ("info" | "lines", buffer id)

    def refresh(buffer):
        pending[request(
            "buffer.get_lines", {"buffer": buffer, "start": 0, "end": -1}
        )] = ("lines", buffer)

    def handle_spice(frame):
        kind, id_, method, params = frame
        if kind == 0 and method == "spice.lifecycle.hello":
            notify("ready", {
                "protocol": "0.1",
                "subscribe": ["spice.buffer.created", "spice.buffer.changed"],
            })
        elif kind == 0 and method == "spice.lifecycle.shutdown":
            return False
        elif kind == 0 and method == "spice.buffer.created":
            buffer = params["buffer"]
            pending[request("buffer.info", {"buffer": buffer})] = ("info", buffer)
        elif kind == 0 and method == "spice.buffer.changed":
            if params["buffer"] in tracked:
                refresh(params["buffer"])
        elif kind == 3 and id_ in pending:
            what, buffer = pending.pop(id_)
            if "code" in params:
                tracked.pop(buffer, None)
            elif what == "info":
                name = params.get("name", "")
                language = LANGUAGES.get(os.path.splitext(name)[1])
                if language and buffer not in tracked:
                    tracked[buffer] = {
                        "uri": "file://" + os.path.abspath(name),
                        "language": language, "version": 0, "lines": [],
                    }
                    refresh(buffer)
            elif what == "lines" and buffer in tracked:
                document = tracked[buffer]
                document["lines"] = params["lines"]
                document["version"] += 1
                text = "\n".join(document["lines"])
                if document["version"] == 1:
                    lsp.notify("textDocument/didOpen", {"textDocument": {
                        "uri": document["uri"],
                        "languageId": document["language"],
                        "version": 1, "text": text,
                    }})
                else:
                    lsp.notify("textDocument/didChange", {
                        "textDocument": {"uri": document["uri"],
                                         "version": document["version"]},
                        "contentChanges": [{"text": text}],
                    })
                lsp.request_tokens(document["uri"], buffer)
        return True

    def handle_lsp(message):
        if message.get("id") == 0 and "result" in message:
            lsp.handle_initialized(message["result"])
        elif message.get("id") in lsp.pending:
            buffer = lsp.pending.pop(message["id"])
            result = message.get("result") or {}
            if buffer in tracked and isinstance(result, dict):
                notify("buffer.set_highlights", {
                    "buffer": buffer,
                    "highlights": decode_tokens(
                        result.get("data", []), lsp.legend,
                        tracked[buffer]["lines"], lsp.encoding,
                    ),
                })
        elif "id" in message and "method" in message:
            # a server -> client request: answer, or the server stalls
            items = message.get("params", {}).get("items", [])
            result = [None] * len(items) \
                if message["method"] == "workspace/configuration" else None
            lsp.send({"jsonrpc": "2.0", "id": message["id"], "result": result})
        # notifications (diagnostics, progress, logs): not our department

    selector = selectors.DefaultSelector()
    os.set_blocking(sys.stdin.fileno(), False)
    os.set_blocking(lsp.proc.stdout.fileno(), False)
    selector.register(sys.stdin.fileno(), selectors.EVENT_READ, "spice")
    selector.register(lsp.proc.stdout.fileno(), selectors.EVENT_READ, "lsp")
    spice_incoming = bytearray()

    running = True
    while running:
        for key, _ in selector.select(timeout=1.0):
            try:
                chunk = os.read(key.fd, 65536)
            except (BlockingIOError, OSError):
                continue
            if key.data == "spice":
                if not chunk:
                    running = False  # Spice closed the pipe: we are done
                    break
                spice_incoming += chunk
                for frame in take_spice_frames(spice_incoming):
                    if not handle_spice(frame):
                        running = False
            else:
                if not chunk:
                    continue  # the server hung up; nothing more to color
                lsp.incoming += chunk
                for message in lsp.take_messages():
                    handle_lsp(message)

    lsp.send({"jsonrpc": "2.0", "id": 999999, "method": "shutdown", "params": None})
    lsp.send({"jsonrpc": "2.0", "method": "exit", "params": None})
    try:
        lsp.proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        lsp.proc.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())

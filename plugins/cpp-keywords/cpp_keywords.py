#!/usr/bin/env python3
"""cpp-keywords: paint C++ keywords pink, live, in every C++ buffer.

A deliberately small Spice plugin, self-contained (no msgpack dependency -
the subset it needs is inlined below). It watches buffers appear and
change, and for any buffer whose name looks like a C++ file it recomputes
keyword spans and hands them to the core with buffer.set_highlights.

Install (config.toml):

    [[plugin]]
    name    = "cpp-keywords"
    command = ["python3", "/path/to/cpp_keywords.py"]

It is also honest documentation-by-example for the wire protocol: framing,
the handshake, events, requests and notifies - in ~200 lines of stdlib
Python.
"""

import re
import struct
import sys

PINK = 0xFF69B4

CPP_SUFFIXES = (".cpp", ".cxx", ".cc", ".c", ".hpp", ".hxx", ".hh", ".h", ".ipp", ".inl")

KEYWORDS = (
    "alignas alignof and and_eq asm auto bitand bitor bool break case catch "
    "char char8_t char16_t char32_t class compl concept const consteval "
    "constexpr constinit const_cast continue co_await co_return co_yield "
    "decltype default delete do double dynamic_cast else enum explicit "
    "export extern false float for friend goto if inline int long mutable "
    "namespace new noexcept not not_eq nullptr operator or or_eq private "
    "protected public register reinterpret_cast requires return short "
    "signed sizeof static static_assert static_cast struct switch template "
    "this thread_local throw true try typedef typeid typename union "
    "unsigned using virtual void volatile wchar_t while xor xor_eq"
).split()

KEYWORD_RE = re.compile(
    rb"(?<![A-Za-z0-9_])(?:" + b"|".join(k.encode() for k in KEYWORDS) + rb")(?![A-Za-z0-9_])"
)

# --- msgpack, the subset Spice speaks ------------------------------------

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
    """One value from data[at:]; returns (value, next_offset)."""
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
        signed = tag >= 0xD0
        return int.from_bytes(raw, "big", signed=signed), at + width
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

# --- framing -------------------------------------------------------------

def read_frame():
    header = sys.stdin.buffer.read(4)
    if len(header) < 4:
        return None
    length = int.from_bytes(header, "big")
    payload = sys.stdin.buffer.read(length)
    if len(payload) < length:
        return None
    value, _ = unpack(payload)
    return value


def write_frame(message):
    payload = pack(message)
    sys.stdout.buffer.write(len(payload).to_bytes(4, "big") + payload)
    sys.stdout.buffer.flush()  # rule 1: flush after every write


def notify(method, params):
    write_frame([1, None, method, params])


_next_id = 0


def request(method, params):
    global _next_id
    _next_id += 1
    write_frame([2, _next_id, method, params])
    return _next_id

# --- the highlighter -----------------------------------------------------

def keyword_spans(lines):
    spans = []
    for lineno, line in enumerate(lines):
        for match in KEYWORD_RE.finditer(line.encode()):
            spans.append({
                "range": {
                    "start": {"line": lineno, "col": match.start()},
                    "end": {"line": lineno, "col": match.end()},
                },
                "fg": PINK,
            })
    return spans


def main():
    pending = {}      # request id -> ("info" | "lines", buffer id)
    cpp_buffers = set()

    while (message := read_frame()) is not None:
        kind, id_, method, params = message

        if kind == 0 and method == "spice.lifecycle.hello":
            notify("ready", {
                "protocol": "0.1",
                "subscribe": ["spice.buffer.created", "spice.buffer.changed"],
            })

        elif kind == 0 and method == "spice.lifecycle.shutdown":
            return

        elif kind == 0 and method == "spice.buffer.created":
            buffer = params["buffer"]
            pending[request("buffer.info", {"buffer": buffer})] = ("info", buffer)

        elif kind == 0 and method == "spice.buffer.changed":
            buffer = params["buffer"]
            if buffer in cpp_buffers:
                pending[request(
                    "buffer.get_lines", {"buffer": buffer, "start": 0, "end": -1}
                )] = ("lines", buffer)

        elif kind == 3 and id_ in pending:
            what, buffer = pending.pop(id_)
            if "code" in params:
                cpp_buffers.discard(buffer)  # gone or unreadable: drop it
            elif what == "info":
                name = params.get("name", "")
                if name.endswith(CPP_SUFFIXES) and buffer not in cpp_buffers:
                    cpp_buffers.add(buffer)
                    pending[request(
                        "buffer.get_lines", {"buffer": buffer, "start": 0, "end": -1}
                    )] = ("lines", buffer)
            elif what == "lines":
                notify("buffer.set_highlights", {
                    "buffer": buffer,
                    "highlights": keyword_spans(params["lines"]),
                })


if __name__ == "__main__":
    main()

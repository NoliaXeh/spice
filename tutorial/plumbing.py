"""The plumbing every tutorial plugin shares: msgpack and frames.

Copy this file next to your plugin and `from plumbing import *`. It is
dependency-free - the msgpack subset Spice speaks is small enough to
carry along (see tutorial 01 for what these bytes mean on the wire).
"""

import struct
import sys

# --- msgpack -------------------------------------------------------------

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

# --- frames --------------------------------------------------------------

def read_frame():
    """One [kind, id, method, params] message. None when Spice hangs up."""
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
    sys.stdout.buffer.flush()  # rule one: always flush


def notify(method, params):
    """Fire-and-forget, plugin -> Spice. Never answered."""
    write_frame([1, None, method, params])


_next_id = 0


def request(method, params):
    """Ask Spice something; the answer comes back with the returned id."""
    global _next_id
    _next_id += 1
    write_frame([2, _next_id, method, params])
    return _next_id

"""Minimal, test-only A record responses shared by local DNS peers."""

import socket
import struct


def answer(query, names):
    assert len(query) >= 17 and query[4:6] == b"\0\1", query.hex()
    labels = []
    offset = 12
    while query[offset]:
        size = query[offset]
        labels.append(query[offset + 1 : offset + 1 + size].decode("ascii"))
        offset += size + 1
    name = ".".join(labels).lower()
    question = query[12 : offset + 5]
    record_type, record_class = struct.unpack("!HH", question[-4:])
    assert record_class == 1, question.hex()
    address = names.get(name)
    if not address:
        return name, query[:2] + struct.pack("!5H", 0x8183, 1, 0, 0, 0) + question
    if record_type != 1:
        return name, query[:2] + struct.pack("!5H", 0x8180, 1, 0, 0, 0) + question
    record = struct.pack("!HHHIH", 0xC00C, 1, 1, 0, 4) + socket.inet_aton(address)
    return name, query[:2] + struct.pack("!5H", 0x8180, 1, 1, 0, 0) + question + record

"""FF FE FD FC fragmentation: build and reassembly.

Fragment header:
    magic(4) msginfo(4, big-endian: msgSeq<<16 | idx) count(2, big-endian) chunk

c->s "envelope-first" wrap: a fragmented client message rides inside an outer
command envelope whose body is SID(16) + fragment header + chunk, and whose
size field counts only the chunk, not the SID + fragment header.
"""

from collections import namedtuple

from . import envelope

MAGIC = b"\xff\xfe\xfd\xfc"
HEADER_LEN = 10  # magic(4) + msginfo(4) + count(2)

Fragment = namedtuple("Fragment", ["msg_seq", "idx", "count", "chunk"])


def pack_fragment(msg_seq, idx, count, chunk):
    msginfo = ((msg_seq << 16) | idx).to_bytes(4, "big")
    return MAGIC + msginfo + count.to_bytes(2, "big") + chunk


def split(msg_seq, blob, chunk_size):
    """Split blob into fragment datagram payloads for message msg_seq."""
    chunks = [blob[i : i + chunk_size] for i in range(0, len(blob), chunk_size)] or [b""]
    count = len(chunks)
    return [pack_fragment(msg_seq, idx, count, chunk) for idx, chunk in enumerate(chunks)]


def parse(payload, offset=0):
    """Parse the fragment at payload[offset:]. None if no fragment magic."""
    if payload[offset : offset + 4] != MAGIC:
        return None
    mi = int.from_bytes(payload[offset + 4 : offset + 8], "big")
    count = int.from_bytes(payload[offset + 8 : offset + 10], "big")
    return Fragment(mi >> 16, mi & 0xFFFF, count, payload[offset + HEADER_LEN :])


def wrap_client_fragment(cmd, sid, frag_payload, ver=0):
    """Envelope-first c->s wrap: outer env body = SID + frag header + chunk,
    with the outer size counting only the chunk (wire quirk)."""
    frag = parse(frag_payload)
    if frag is None:
        raise ValueError("not a fragment payload")
    size = len(frag.chunk).to_bytes(3, "big")
    return envelope.MAGIC + bytes([ver]) + size + cmd.to_bytes(2, "big") + sid + frag_payload


def unwrap_client_fragment(payload, sid):
    """Inverse of wrap_client_fragment on a datagram payload -> (cmd, Fragment)
    or None if the payload isn't an envelope-first wrapped fragment. Reads the
    full remainder as the body because the outer size counts only the chunk."""
    if payload[:4] != envelope.MAGIC or len(payload) < envelope.HEADER_LEN_CS:
        return None
    cmd = int.from_bytes(payload[8:10], "big")
    body = payload[envelope.HEADER_LEN_CS :]
    if body[:16] != sid:
        return None
    frag = parse(body, 16)
    if frag is None:
        return None
    return cmd, frag


class Reassembler:
    """Collect fragments keyed by (msg_seq, count); add() returns the
    reassembled blob once all `count` chunks of a message arrived."""

    def __init__(self):
        self._parts = {}

    def add(self, frag):
        key = (frag.msg_seq, frag.count)
        parts = self._parts.setdefault(key, {})
        parts[frag.idx] = frag.chunk
        if len(parts) == frag.count:
            del self._parts[key]
            return b"".join(parts[i] for i in range(frag.count))
        return None

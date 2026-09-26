"""FE BE DE EF command envelope.

Layout:
    magic(4) ver(1) size(3, big-endian) cmd(2, big-endian) [status(1) if s->c]

size counts the body bytes that follow the header. For c->s envelopes the body
starts with the cleartext 16-byte session ID (SID) prefix; for s->c the extra
header byte is a status code. One datagram payload may carry several envelopes
back to back.
"""

from collections import namedtuple

MAGIC = b"\xfe\xbe\xde\xef"
HEADER_LEN_CS = 10  # c->s: no status byte
HEADER_LEN_SC = 11  # s->c: status byte after cmd

Envelope = namedtuple("Envelope", ["ver", "cmd", "status", "size", "body", "consumed"])
# body     — the envelope body (for c->s this still includes the SID prefix)
# consumed — header + body bytes taken from the input


def build(cmd, body, ver=0, status=None):
    """Build one envelope. status set -> s->c layout; status None -> c->s."""
    hdr = MAGIC + bytes([ver]) + len(body).to_bytes(3, "big") + cmd.to_bytes(2, "big")
    if status is not None:
        hdr += bytes([status])
    return hdr + body


def parse(payload, from_client, offset=0):
    """Parse the envelope at payload[offset:]. from_client selects the header
    layout (c->s vs s->c). Raises ValueError if no magic / truncated header.
    A truncated body (size > available) yields a short body instead of raising,
    matching how the reference decoder handled truncated captures."""
    if payload[offset : offset + 4] != MAGIC:
        raise ValueError("no envelope magic at offset")
    ver = payload[offset + 4]
    size = int.from_bytes(payload[offset + 5 : offset + 8], "big")
    cmd = int.from_bytes(payload[offset + 8 : offset + 10], "big")
    if from_client:
        status = None
        hdr_len = HEADER_LEN_CS
    else:
        status = payload[offset + 10]
        hdr_len = HEADER_LEN_SC
    avail = len(payload) - offset - hdr_len
    take = min(size, avail)
    body = payload[offset + hdr_len : offset + hdr_len + take]
    return Envelope(ver, cmd, status, size, body, hdr_len + take)


def parse_all(payload, from_client):
    """Yield every envelope in a (possibly multi-envelope) datagram payload.

    After consuming one envelope, trailing bytes are scanned for another
    FE BE DE EF envelope; anything else ends iteration.
    """
    offset = 0
    while len(payload) - offset >= HEADER_LEN_CS and payload[offset : offset + 4] == MAGIC:
        env = parse(payload, from_client, offset)
        yield env
        if env.size > len(env.body):  # truncated envelope: nothing sane after it
            return
        offset += env.consumed

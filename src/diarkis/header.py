"""4-byte Diarkis datagram header: 3-byte little-endian sequence + 1 type byte."""

HEADER_LEN = 4

PTYPE = {1: "UDP", 2: "SYN", 3: "DAT", 4: "ACK", 5: "RST", 6: "EACK", 7: "FIN", 12: "DIR"}


def pack(seq, ptype):
    """seq (0..0xFFFFFF) little-endian 3 bytes + packet type byte."""
    return seq.to_bytes(3, "little") + bytes([ptype])


def parse(datagram):
    """-> (seq, ptype, payload). Raises ValueError on a short datagram."""
    if len(datagram) < HEADER_LEN:
        raise ValueError(f"datagram too short for header: {len(datagram)} bytes")
    seq = int.from_bytes(datagram[:3], "little")
    return seq, datagram[3], datagram[HEADER_LEN:]

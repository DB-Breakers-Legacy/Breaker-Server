from diarkis import envelope, fragment

SID = bytes(range(16))


def test_split_reassemble_in_order():
    blob = bytes(range(256)) * 3
    parts = fragment.split(7, blob, 100)
    assert len(parts) == 8  # ceil(768/100)
    reasm = fragment.Reassembler()
    result = None
    for p in parts:
        result = reasm.add(fragment.parse(p)) or result
    assert result == blob


def test_reassemble_out_of_order():
    blob = b"abcdef" * 50
    parts = fragment.split(9, blob, 64)
    reasm = fragment.Reassembler()
    result = None
    for p in reversed(parts):
        result = reasm.add(fragment.parse(p)) or result
    assert result == blob


def test_fragment_fields():
    wire = fragment.pack_fragment(0x1234, 2, 5, b"chunk")
    frag = fragment.parse(wire)
    assert (frag.msg_seq, frag.idx, frag.count, frag.chunk) == (0x1234, 2, 5, b"chunk")


def test_parse_no_magic_returns_none():
    assert fragment.parse(b"\xfe\xbe\xde\xef" + b"\x00" * 10) is None


def test_client_fragment_wrap_roundtrip():
    frag_payload = fragment.pack_fragment(3, 0, 1, b"the-chunk")
    wire = fragment.wrap_client_fragment(0x2FF0, SID, frag_payload, ver=1)

    # outer size counts only the chunk, not SID + fragment header (wire quirk)
    assert wire[:4] == envelope.MAGIC
    assert int.from_bytes(wire[5:8], "big") == len(b"the-chunk")

    cmd, frag = fragment.unwrap_client_fragment(wire, SID)
    assert cmd == 0x2FF0
    assert frag == fragment.parse(frag_payload)


def test_unwrap_wrong_sid_returns_none():
    wire = fragment.wrap_client_fragment(1, SID, fragment.pack_fragment(0, 0, 1, b"x"))
    assert fragment.unwrap_client_fragment(wire, b"\xff" * 16) is None


def test_unwrap_non_envelope_returns_none():
    assert fragment.unwrap_client_fragment(b"\x00" * 30, SID) is None

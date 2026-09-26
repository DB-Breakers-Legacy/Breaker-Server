import pytest

from diarkis import envelope


def test_build_parse_client_to_server():
    wire = envelope.build(0x2FF0, b"\x11" * 16 + b"secure-blob", ver=1)
    env = envelope.parse(wire, from_client=True)
    assert env.ver == 1
    assert env.cmd == 0x2FF0
    assert env.status is None
    assert env.size == 16 + 11
    assert env.body == b"\x11" * 16 + b"secure-blob"


def test_build_parse_server_to_client_status_byte():
    wire = envelope.build(0x2FF1, b"secure-blob", ver=1, status=0)
    env = envelope.parse(wire, from_client=False)
    assert env.status == 0
    assert env.body == b"secure-blob"
    assert env.consumed == envelope.HEADER_LEN_SC + 11


def test_multi_envelope_datagram():
    first = envelope.build(0x2FF0, b"aaa", status=0)
    second = envelope.build(0x2FF1, b"bbbb", status=1)
    envs = list(envelope.parse_all(first + second, from_client=False))
    assert [e.cmd for e in envs] == [0x2FF0, 0x2FF1]
    assert [e.body for e in envs] == [b"aaa", b"bbbb"]


def test_parse_all_stops_on_trailing_junk():
    wire = envelope.build(1, b"aa", status=0) + b"\x99\x88"
    envs = list(envelope.parse_all(wire, from_client=False))
    assert len(envs) == 1


def test_no_magic_raises():
    with pytest.raises(ValueError):
        envelope.parse(b"\xde\xad\xbe\xef" + b"\x00" * 10, from_client=False)


def test_truncated_body_yielded_short():
    wire = envelope.build(7, b"full-body", status=0)[:-3]
    env = envelope.parse(wire, from_client=False)
    assert env.size == 9
    assert env.body == b"full-b"

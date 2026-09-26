import pytest

from diarkis import header


def test_roundtrip_all_types():
    for ptype in (1, 2, 3, 4, 5, 6, 7, 12):
        datagram = header.pack(0x010203, ptype) + b"payload"
        seq, parsed_type, payload = header.parse(datagram)
        assert seq == 0x010203
        assert parsed_type == ptype
        assert payload == b"payload"


def test_seq_zero_and_max():
    for seq in (0, 0xFFFFFF):
        parsed_seq, _, _ = header.parse(header.pack(seq, 3))
        assert parsed_seq == seq


def test_short_datagram_raises():
    with pytest.raises(ValueError):
        header.parse(b"\x00\x01\x02")

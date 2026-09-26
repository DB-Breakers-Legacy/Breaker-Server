import os

from diarkis import crypto

KEYS = crypto.KeySet(
    sid=bytes(range(16)),
    key=os.urandom(16),
    iv=os.urandom(16),
    mac=os.urandom(16),
)


def test_roundtrip_server_to_client():
    pt = b"\x93\x01\x02\x03hello diarkis"
    blob = crypto.encrypt(pt, KEYS)
    res = crypto.decrypt(blob, KEYS, from_client=False)
    assert res.pt == pt
    assert res.plen == len(pt)
    assert res.hmac_ok
    assert res.note == ""


def test_roundtrip_client_to_server_strips_sid():
    pt = b"client says hi"
    blob = crypto.encrypt(pt, KEYS)
    res = crypto.decrypt(KEYS.sid + blob, KEYS, from_client=True)
    assert res.pt == pt
    assert res.sid_ok is True


def test_wrong_sid_flagged_but_decrypted():
    blob = crypto.encrypt(b"data", KEYS)
    res = crypto.decrypt(b"\xff" * 16 + blob, KEYS, from_client=True)
    assert res.sid_ok is False
    assert res.pt == b"data"


def test_hmac_fail_rejected():
    blob = bytearray(crypto.encrypt(b"some plaintext here", KEYS))
    blob[-1] ^= 0xFF  # tamper with the ciphertext
    res = crypto.decrypt(bytes(blob), KEYS, from_client=False)
    assert res.pt is None
    assert res.hmac_ok is False


def test_pad_extension_accepted_when_hmac_valid():
    # sender under-reports plen; HMAC still validates -> accept up to last
    # nonzero byte (M4 acceptance rule, observed on cmd 0x0018 ext)
    pt = b"0123456789abcdef"  # exactly one block
    blob = crypto.encrypt(pt, KEYS)
    under = (len(pt) - 3).to_bytes(4, "big") + blob[4:]  # plen lies, mac untouched
    res = crypto.decrypt(under, KEYS, from_client=False)
    assert res.pt == pt
    assert res.hmac_ok
    assert res.note.startswith("pad-ext")


def test_plen_zero_nonzero_tail_accepted():
    pt = b"no length field"
    blob = crypto.encrypt(pt, KEYS)
    zeroed = (0).to_bytes(4, "big") + blob[4:]
    res = crypto.decrypt(zeroed, KEYS, from_client=False)
    assert res.pt == pt
    assert res.note == "plen0-nonzero-tail"


def test_short_blob_rejected():
    assert crypto.decrypt(b"\x00" * 10, KEYS, from_client=False).pt is None


def test_odd_ciphertext_rejected():
    blob = crypto.encrypt(b"x", KEYS)[:-1]  # ct no longer block-aligned
    assert crypto.decrypt(blob, KEYS, from_client=False).pt is None


def test_empty_plaintext_null_packet():
    blob = crypto.encrypt(b"", KEYS)
    assert blob[:4] == (0).to_bytes(4, "big")
    assert len(blob) == 36  # plen + hmac, empty ciphertext

"""Secure envelope: AES-128-CBC + HMAC-SHA256.

Wire layout (per direction; c->s bodies additionally carry a 16-byte cleartext
SID prefix, handled by envelope.py):

    [4B big-endian plaintext length][32B HMAC-SHA256(ciphertext)][ciphertext]

Ciphertext is the plaintext zero-padded to AES blocks with the fixed IV from
the session key set. HMAC is over the ciphertext only.

Decrypt acceptance rules (capture-verified):
  * normal: 0 < plen <= len(ct) and everything after pt[plen:] is zero
  * plen == 0: accept, content length = index after last nonzero byte
  * padding check fails but HMAC validates: accept, keep up to last nonzero
    byte (sender length field can under-report; observed on cmd 0x0018 ext)
  * otherwise: reject
"""

import hashlib
import hmac
from collections import namedtuple

from Crypto.Cipher import AES

BLOCK = AES.block_size

KeySet = namedtuple("KeySet", ["sid", "key", "iv", "mac"])

# pt=None when rejected; note explains acceptance edge cases / failure reason
SecureResult = namedtuple(
    "SecureResult", ["plen", "pt", "hmac_ok", "sid_ok", "note"], defaults=[None]
)


def encrypt(plaintext, keys):
    """plaintext (bytes) -> secure blob [plen][hmac][ct].

    Zero-pads to a full extra block when already aligned (matching the
    reference server). Empty plaintext encrypts to an empty ciphertext with
    plen=0 (the NULL packet case)."""
    if plaintext:
        pad_len = BLOCK - (len(plaintext) % BLOCK)
        padded = plaintext + b"\x00" * pad_len
    else:
        padded = b""
    ct = AES.new(keys.key, AES.MODE_CBC, keys.iv).encrypt(padded)
    mac = hmac.new(keys.mac, ct, hashlib.sha256).digest()
    return len(plaintext).to_bytes(4, "big") + mac + ct


def decrypt(blob, keys, from_client):
    """Decrypt a secure blob (with SID prefix when from_client) -> SecureResult."""
    sid_ok = None
    if from_client:
        sid_ok = blob[:16] == keys.sid
        blob = blob[16:]
    if len(blob) < 36:
        return SecureResult(0, None, False, sid_ok, "short")
    plen = int.from_bytes(blob[:4], "big")
    mac_f = blob[4:36]
    ct = blob[36:]
    if len(ct) % BLOCK or len(ct) == 0:
        return SecureResult(plen, None, False, sid_ok, f"odd-ct len={len(ct)}")
    hmac_ok = hmac.compare_digest(hmac.new(keys.mac, ct, hashlib.sha256).digest(), mac_f)
    pt = AES.new(keys.key, AES.MODE_CBC, keys.iv).decrypt(ct)
    nz = len(pt.rstrip(b"\x00"))  # content length = index after last nonzero byte
    if 0 < plen <= len(ct) and pt[plen:] == b"\x00" * (len(ct) - plen):
        return SecureResult(plen, pt[:plen], hmac_ok, sid_ok, "")
    if plen == 0:
        return SecureResult(nz, pt[:nz], hmac_ok, sid_ok, "plen0-nonzero-tail")
    if hmac_ok:
        # padding check failed but HMAC validates: trust the crypto
        return SecureResult(plen, pt[:nz], True, sid_ok, f"pad-ext plen={plen} content={nz}")
    return SecureResult(plen, None, False, sid_ok, "hmac/pad fail")

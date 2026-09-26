"""Diarkis RUDP wire codec for DRAGON BALL: THE BREAKERS.

Bidirectional (encrypt + decrypt) implementation of the datagram format,
built from the capture-verified decoder logic:

  * header.py    — 4-byte datagram header (3-byte LE seq + type byte)
  * envelope.py  — FE BE DE EF command envelope
  * crypto.py    — secure envelope (AES-128-CBC + HMAC-SHA256)
  * fragment.py  — FF FE FD FC fragmentation build/reassembly

Public Diarkis constants do NOT apply to this game; this matches the observed
wire format only.
"""

from . import crypto, envelope, fragment, header

__all__ = ["crypto", "envelope", "fragment", "header"]

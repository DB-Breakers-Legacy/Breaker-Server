"""Diarkis RUDP server — standalone entry point (UDP matchmaking peer).

Phase 2: the protocol lives in src/diarkis_peer.py; this file is only the
entry point. Standalone mode shares NO key registry with the HTTP API
(separate processes): it pins the DIARKIS_*_KEY env key set (if present) so
dev clients using that set can connect. For real use, run
`python manage.py rundiarkis` (HTTP + UDP in one process, shared registry).
"""

import os
import sys
from pathlib import Path

from dotenv import load_dotenv
from twisted.internet import reactor

sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))
load_dotenv(Path(__file__).resolve().parent / ".env")

from diarkis_peer import DiarkisProtocol, issue_keyset  # noqa: E402

if "DIARKIS_AES_KEY" in os.environ:
    issue_keyset()  # register the env-pinned key set for standalone dev

if __name__ == "__main__":
    udp_port = int(os.environ.get("UDP_PORT", "7100"))
    probe_port = int(os.environ.get("UDP_SESSION_PORT", "7102"))
    reactor.listenUDP(udp_port, DiarkisProtocol())
    reactor.listenUDP(probe_port, DiarkisProtocol(probe=True))
    print(f"Diarkis UDP peer on {udp_port} (+ probe {probe_port})")
    reactor.run()

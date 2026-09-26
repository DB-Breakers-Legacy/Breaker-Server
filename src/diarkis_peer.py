"""Diarkis UDP peer (server side) — Phase 2 session transport + bootstrap.

Built on the diarkis codec package; behavior follows the golden transcripts
(bootstrap / heartbeat / teardown / directory-0c0c), not public Diarkis docs.
Commands handled: 0x0001 (init/heartbeat echo), 0x0130/0x0131 (bootstrap
exchange), 0x0002 (session-host redirect push, probe listener), 0x55f0
(connection-room probe trigger). The 12xxx matchmaking family is Phase 3.

Key sharing: the HTTP API issues per-session key sets via issue_keyset(),
which registers them in KEY_REGISTRY; the UDP protocol looks keys up by SID.
The registry is a plain in-process dict — HTTP and UDP must run in ONE
process: use `python manage.py rundiarkis`. Standalone `RUDPserver.py` is a
dev entry point and only knows the env-pinned key set (if any).

# ponytail: per-session state is a plain dict keyed by SID, retransmission is
# a single 0.5 s LoopingCall (the observed wire behavior is a constant 0.5 s
# retransmit, no backoff). Ceiling: real RUDP windows/backoff if a capture
# ever shows them; a shared store if we ever go multi-process.
"""

import logging
import os

from twisted.internet.protocol import DatagramProtocol
from twisted.internet.task import LoopingCall

from diarkis import crypto, envelope, header

logger = logging.getLogger(__name__)

# SID (bytes) -> KeySet, written by the HTTP layer, read here.
KEY_REGISTRY = {}


def issue_keyset():
    """Fresh per-session random key set, registered for the UDP peer.

    Dev override: if DIARKIS_*_KEY env vars are set, that fixed set is
    registered instead (see .env.example).
    """
    if "DIARKIS_AES_KEY" in os.environ:
        ks = crypto.KeySet(
            sid=bytes.fromhex(os.environ["DIARKIS_SID_KEY"]),
            key=bytes.fromhex(os.environ["DIARKIS_AES_KEY"]),
            iv=bytes.fromhex(os.environ["DIARKIS_IV_KEY"]),
            mac=bytes.fromhex(os.environ["DIARKIS_HASH_KEY"]),
        )
    else:
        ks = crypto.KeySet(*(os.urandom(16) for _ in range(4)))
    KEY_REGISTRY[ks.sid] = ks
    return ks


# payload strings (all .env-configurable; defaults are RFC 5737/.invalid placeholders)
PUSH_ADDRS = os.environ.get(
    "DIARKIS_PUSH_ADDRS", "192.0.2.12.example.invalid:7100,192.0.2.12.example.invalid:7099"
).split(",")
REDIRECT_ADDR = os.environ.get("DIARKIS_REDIRECT_ADDR", "192.0.2.12.example.invalid:7100")
DIRECTORY_ADDR = os.environ.get("DIARKIS_DIRECTORY_ADDR", "192.0.2.13.example.invalid:7102")

DIRECTORY_CMD = b"\x0c\x0c\x00\x00\x00\x01"  # cleartext directory: no RUDP header, no crypto


class Session:
    """Per-session state, keyed by SID."""

    def __init__(self, sid, keys, addr):
        self.sid = sid
        self.keys = keys
        self.addr = addr
        self.seen = set()  # datagram dedupe (client bursts/retries)
        self.next_seq = 0  # server DAT sequence
        self.unacked = {}  # seq -> envelope payload, retransmitted (type 5) until ACKed
        self.client_public = None  # STUN'd address from the cmd-1 init (Phase 4 P2P)
        self.client_lan = None
        self.bootstrapped = False


class DiarkisProtocol(DatagramProtocol):
    """One listener instance per UDP port. probe=True for the session-host
    redirect port (7102): same bootstrap, but 0x012f pushes are suppressed and
    cmd 0x55f0 is answered with the 0x0002 redirect push."""

    def __init__(self, probe=False, retransmit_interval=0.5):
        self.probe = probe
        self.push_addrs = PUSH_ADDRS
        self.redirect_addr = REDIRECT_ADDR
        self.directory_addr = DIRECTORY_ADDR
        self.retransmit_interval = retransmit_interval
        self.registry = KEY_REGISTRY
        self.sessions = {}
        self._retx = LoopingCall(self._retransmit)

    def startProtocol(self):
        self._retx.start(self.retransmit_interval, now=False)

    def shutdown(self):
        """Stop the retransmit loop (entry points run forever; tests call this)."""
        if self._retx.running:
            self._retx.stop()

    def datagramReceived(self, data, addr):
        # cleartext 0x0c0c directory query: no RUDP header, no crypto
        if data[2:8] == DIRECTORY_CMD:
            return self._directory(data, addr)
        if len(data) < 4:
            return
        seq, ptype, payload = header.parse(data)
        if ptype == 2:
            return self._syn(payload, addr)
        if ptype == 7:
            return self._fin(payload, addr)
        if ptype in (4, 6):
            return self._ack(seq, payload)
        if ptype in (1, 3):
            return self._dat(data, seq, payload, addr)
        logger.info("unknown packet type %d from %s", ptype, addr)

    # --- connection lifecycle ---

    def _syn(self, payload, addr):
        sid = payload[:16]
        keys = self.registry.get(sid)
        if keys is None:
            logger.warning("SYN with unknown SID from %s — dropped", addr)
            return
        if sid not in self.sessions:
            self.sessions[sid] = Session(sid, keys, addr)
            logger.info("session open sid=%s addr=%s probe=%s", sid.hex(), addr, self.probe)
        self.transport.write(header.pack(0, 4), addr)  # bare ACK, seq 0

    def _fin(self, payload, addr):
        session = self.sessions.pop(payload[:16], None)
        if session is None:
            return
        # bare ACK carrying OUR next DAT sequence (teardown.txt)
        self.transport.write(header.pack(session.next_seq, 4), addr)
        logger.info("session closed sid=%s", session.sid.hex())

    def _ack(self, seq, payload):
        # client ACK: header seq = first acked server DAT; payload = SID, then
        # 20-byte records [3B LE seq][type][SID] for any extra acked seqs
        if len(payload) < 16:
            return
        session = self.sessions.get(payload[:16])
        if session is None:
            return
        session.unacked.pop(seq, None)
        rest = payload[16:]
        while len(rest) >= 20 and rest[4:20] == session.sid:
            session.unacked.pop(int.from_bytes(rest[:3], "little"), None)
            rest = rest[20:]

    def _dat(self, data, seq, payload, addr):
        if payload[:4] != envelope.MAGIC:
            return  # fragmented matchmaking commands are Phase 3
        sid = payload[envelope.HEADER_LEN_CS : envelope.HEADER_LEN_CS + 16]
        session = self.sessions.get(sid)
        if session is None:
            return
        if data in session.seen:  # dedupe client retries: re-ACK, don't reprocess
            self.transport.write(header.pack(seq, 4), addr)
            return
        session.seen.add(data)
        self.transport.write(header.pack(seq, 4), addr)  # bare ACK echoing the DAT seq
        for env in envelope.parse_all(payload, from_client=True):
            res = crypto.decrypt(env.body, session.keys, from_client=True)
            if res.pt is None:
                logger.warning("decrypt fail sid=%s cmd=%#06x: %s", sid.hex(), env.cmd, res.note)
                continue
            self._command(session, env, res.pt, addr)

    # --- commands ---

    def _command(self, session, env, pt, addr):
        if env.cmd == 0x0001:
            self._init_or_heartbeat(session, env, pt, addr)
        elif env.cmd == 0x0130 and not self.probe:
            self._push_server_addr(session, addr, self.push_addrs[1])
        elif env.cmd == 0x0131:
            pass  # bootstrap filler (16 zero bytes) — ACK was already sent
        elif env.cmd == 0x55F0 and self.probe:
            # connection-room probe on :7102 -> redirect push to the real session host
            pt = bytes.fromhex("beeffeed") + self.redirect_addr.encode()
            self._send_dat(session, 0x0002, pt, ver=0, status=0xFF, addr=addr)
        else:
            logger.info("unhandled cmd %#06x sid=%s", env.cmd, session.sid.hex())

    def _init_or_heartbeat(self, session, env, pt, addr):
        # init (130 B DAT): [4B ms counter][a0 01 00 00][4B BE len][public addr]
        #                   [4B BE len][LAN addr] — client addresses stored for Phase 4.
        # heartbeat (82 B DAT): [4B ms counter][a0 01 00 00].
        if len(pt) > 8:
            public, lan = _parse_init_addrs(pt)
            if public:
                session.client_public, session.client_lan = public, lan
        # reply: [01][counter echo][a0 01 00 00][server-observed public address]
        reply = b"\x01" + pt[:4] + bytes.fromhex("a0010000") + f"{addr[0]}:{addr[1]}".encode()
        self._send_dat(session, env.cmd, reply, ver=env.ver, status=1, addr=addr)
        if not self.probe and not session.bootstrapped:
            session.bootstrapped = True
            self._push_server_addr(session, addr, self.push_addrs[0])

    def _push_server_addr(self, session, addr, push_addr):
        pt = len(push_addr).to_bytes(2, "big") + push_addr.encode()
        self._send_dat(session, 0x012F, pt, ver=1, status=0xFF, addr=addr)

    # --- cleartext 0x0c0c directory ---

    def _directory(self, data, addr):
        # query: [2B flag][0c0c][00000001][2B BE len][SID]; answer with our own
        # session-host endpoint. Flag flips between answer and query.
        flag = b"\x0f\x0f" if data[:2] == b"\x00\x00" else b"\x00\x00"
        payload = self.directory_addr.encode()
        reply = flag + DIRECTORY_CMD + len(payload).to_bytes(2, "big") + payload
        self.transport.write(reply, addr)

    # --- send helpers ---

    def _send_dat(self, session, cmd, pt, ver, status, addr):
        payload = envelope.build(cmd, crypto.encrypt(pt, session.keys), ver=ver, status=status)
        seq = session.next_seq
        session.next_seq += 1
        session.unacked[seq] = payload
        self.transport.write(header.pack(seq, 3) + payload, addr)

    def _retransmit(self):
        # type-5 retransmission of unacked DATs, same seq/body. Observed on the
        # wire: constant 0.5 s interval, no backoff (teardown.txt).
        for session in self.sessions.values():
            for seq, payload in session.unacked.items():
                self.transport.write(header.pack(seq, 5) + payload, session.addr)


def _parse_init_addrs(pt):
    try:
        if pt[4:8] != b"\xa0\x01\x00\x00":
            return None, None
        len1 = int.from_bytes(pt[8:12], "big")
        public = pt[12 : 12 + len1].decode("ascii")
        off = 12 + len1
        len2 = int.from_bytes(pt[off : off + 4], "big")
        lan = pt[off + 4 : off + 4 + len2].decode("ascii")
        return public, lan
    except (IndexError, UnicodeDecodeError):
        return None, None

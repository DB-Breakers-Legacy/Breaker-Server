"""Socket-level tests for the Diarkis UDP peer (src/diarkis_peer.py).

A fake client (plain UDP socket, throwaway keys/SIDs) drives the transcript
sequences: SYN -> bare ACK -> cmd-1 init -> echo reply + 0x012f push ->
0x0131/0x0130 exchange -> heartbeat -> batched ACKs -> retransmission ->
FIN teardown, plus the cleartext 0x0c0c directory and the :7102 probe
redirect. Byte patterns asserted against bootstrap.txt / heartbeat.txt /
teardown.txt / directory-0c0c.txt (golden transcripts, private repo).
"""

import hashlib
import hmac
import socket
import threading
import time

import pytest
from Crypto.Cipher import AES
from twisted.internet import reactor
from twisted.internet.threads import blockingCallFromThread

from diarkis import crypto, envelope, header
from diarkis_peer import DIRECTORY_ADDR, PUSH_ADDRS, REDIRECT_ADDR, DiarkisProtocol, issue_keyset

MAGIC_CONST = bytes.fromhex("a0010000")
INIT_COUNTER = bytes.fromhex("01020304")


def null_blob(keys):
    """plen=0 secure blob as the real client sends it (16 zero bytes of ct)."""
    ct = AES.new(keys.key, AES.MODE_CBC, keys.iv).encrypt(b"\x00" * 16)
    return (0).to_bytes(4, "big") + hmac.new(keys.mac, ct, hashlib.sha256).digest() + ct


class FakeClient:
    def __init__(self, port, keys=None):
        self.keys = keys or issue_keyset()
        self.sid = self.keys.sid
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(3)
        self.server = ("127.0.0.1", port)
        self.seq = 0

    def send(self, data):
        self.sock.sendto(data, self.server)

    def recv(self):
        return self.sock.recv(4096)

    def recv_until(self, pred, timeout=3):
        """Read datagrams until pred matches; skips retransmissions/noise."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                data = self.recv()
            except TimeoutError:
                continue
            if pred(data):
                return data
        raise AssertionError("timed out waiting for expected datagram")

    def recv_ack(self, seq):
        """Bare ACK echoing the given client seq (or carrying it)."""
        want = header.pack(seq, 4)
        assert self.recv_until(lambda d: d[3] == 4) == want

    def drain(self, window=0.15):
        """Discard everything in flight; returns 0.15 s after the last packet."""
        self.sock.settimeout(window)
        try:
            while True:
                self.recv()
        except TimeoutError:
            pass
        finally:
            self.sock.settimeout(3)

    def expect_silence(self, window=0.4):
        self.sock.settimeout(window)
        try:
            with pytest.raises(TimeoutError):
                self.recv()
        finally:
            self.sock.settimeout(3)

    def syn(self):
        self.send(b"\x00\x00\x00\x02" + self.sid)
        assert self.recv() == b"\x00\x00\x00\x04"  # bare ACK, seq 0 (bootstrap.txt f2)

    def dat(self, cmd, pt, ver=0, blob=None):
        if blob is None:
            blob = crypto.encrypt(pt, self.keys)
        payload = envelope.build(cmd, self.sid + blob, ver=ver)
        self.send(header.pack(self.seq, 3) + payload)
        self.seq += 1

    def ack_server(self, *seqs):
        # client ACK: header seq = first acked seq, then 20-byte records
        # [3B LE seq][type 4][SID] for the rest (bootstrap.txt frame 8)
        first, *rest = seqs
        data = header.pack(first, 4) + self.sid
        for s in rest:
            data += s.to_bytes(3, "little") + b"\x04" + self.sid
        self.send(data)

    def fin(self):
        self.send(header.pack(self.seq, 7) + self.sid)

    def recv_dat(self, cmd=None, timeout=3):
        """Next server DAT (type 3 or retransmitted type 5), parsed+decrypted."""
        data = self.recv_until(
            lambda d: len(d) > 4 and d[3] in (3, 5) and d[4:8] == envelope.MAGIC, timeout
        )
        seq, ptype, payload = header.parse(data)
        env = envelope.parse(payload, from_client=False)
        res = crypto.decrypt(env.body, self.keys, from_client=False)
        assert res.hmac_ok
        if cmd is not None:
            assert env.cmd == cmd
        return seq, ptype, env, res.pt

    def init(self, counter=INIT_COUNTER):
        port = self.sock.getsockname()[1]
        public = f"192.0.2.100:{port}".encode()
        lan = f"192.168.0.100:{port}".encode()
        pt = (
            counter
            + MAGIC_CONST
            + len(public).to_bytes(4, "big")
            + public
            + len(lan).to_bytes(4, "big")
            + lan
        )
        self.dat(0x0001, pt, ver=0)


@pytest.fixture(scope="module", autouse=True)
def twisted_reactor():
    thread = threading.Thread(target=reactor.run, kwargs={"installSignalHandlers": False})
    thread.daemon = True
    thread.start()
    yield reactor


@pytest.fixture
def peer():
    proto = DiarkisProtocol(retransmit_interval=0.1)
    listening = blockingCallFromThread(reactor, reactor.listenUDP, 0, proto)
    yield proto, listening.getHost().port
    blockingCallFromThread(reactor, listening.stopListening)
    proto.shutdown()


def bootstrap(client):
    """SYN + cmd-1 init; ACKs the two server DATs so no retransmissions stay
    in flight. Returns ((reply_seq, reply_env, reply_pt), (push_seq, push_pt))."""
    client.syn()
    client.init()
    client.recv_ack(0)  # bare ACK echoing client seq 0
    reply_seq, _, reply_env, reply_pt = client.recv_dat(cmd=0x0001)
    push_seq, _, _, push_pt = client.recv_dat(cmd=0x012F)
    client.ack_server(reply_seq, push_seq)
    return (reply_seq, reply_env, reply_pt), (push_seq, push_pt)


def test_bootstrap_sequence(peer):
    proto, port = peer
    c = FakeClient(port)
    (reply_seq, reply_env, reply_pt), (push_seq, push_pt) = bootstrap(c)
    client_port = c.sock.getsockname()[1]

    # server init reply: DAT seq 0, ver 0, status 1; pt = 01 + counter echo +
    # a0 01 00 00 + server-observed public address (bootstrap.txt frame 6)
    assert reply_seq == 0
    assert (reply_env.ver, reply_env.status) == (0, 1)
    assert reply_pt == b"\x01" + INIT_COUNTER + MAGIC_CONST + f"127.0.0.1:{client_port}".encode()

    # first 0x012f push: DAT seq 1, ver 1, status 0xff (bootstrap.txt frame 9)
    assert push_seq == 1
    addr = PUSH_ADDRS[0].encode()
    assert push_pt == len(addr).to_bytes(2, "big") + addr

    # client addresses from the init stored for Phase 4
    session = proto.sessions[c.sid]
    assert session.client_public == f"192.0.2.100:{client_port}"
    assert session.client_lan == f"192.168.0.100:{client_port}"

    # 0x0131 (16 zero bytes): ACKed, no reply (bootstrap.txt frames 5/7)
    c.dat(0x0131, b"\x00" * 16, ver=1)
    c.recv_ack(1)

    # 0x0130 (plen=0): ACK + second 0x012f push (frames 11/12)
    c.dat(0x0130, b"", ver=1, blob=null_blob(c.keys))
    c.recv_ack(2)
    seq, _, _, pt = c.recv_dat(cmd=0x012F)
    assert seq == 2
    addr2 = PUSH_ADDRS[1].encode()
    assert pt == len(addr2).to_bytes(2, "big") + addr2
    c.ack_server(seq)


def test_heartbeat_echo(peer):
    _, port = peer
    c = FakeClient(port)
    bootstrap(c)

    counter = bytes.fromhex("0a0b0c0d")
    c.dat(0x0001, counter + MAGIC_CONST, ver=0)  # heartbeat DAT (client seq 1)
    c.recv_ack(1)  # bare ACK echoing the client seq (heartbeat.txt)
    seq, _, env, pt = c.recv_dat(cmd=0x0001)
    assert (env.ver, env.status) == (0, 1)
    # 01 + echoed ms counter + a0 01 00 00 + server-observed public address
    assert pt == b"\x01" + counter + MAGIC_CONST + f"127.0.0.1:{c.sock.getsockname()[1]}".encode()
    c.ack_server(seq)


def test_teardown_fin(peer):
    proto, port = peer
    c = FakeClient(port)
    bootstrap(c)
    next_seq = proto.sessions[c.sid].next_seq

    c.fin()
    # bare ACK carrying the SERVER's next DAT sequence (teardown.txt)
    c.recv_ack(next_seq)
    assert c.sid not in proto.sessions

    # session is gone: a stray heartbeat gets no answer
    c.dat(0x0001, b"\x00" * 4 + MAGIC_CONST, ver=0)
    c.expect_silence()


def test_directory_burst_and_flag_flip(peer):
    _, port = peer
    c = FakeClient(port)
    addr = DIRECTORY_ADDR.encode()
    want = len(addr).to_bytes(2, "big") + addr

    query = b"\x00\x00\x0c\x0c\x00\x00\x00\x01" + (16).to_bytes(2, "big") + c.sid
    for _ in range(5):  # client blasts the query 5x; server answers each
        c.send(query)
    for _ in range(5):
        assert c.recv() == b"\x0f\x0f\x0c\x0c\x00\x00\x00\x01" + want

    # flag flips after the first exchange (directory-0c0c.txt)
    c.send(b"\x0f\x0f" + query[2:])
    assert c.recv() == b"\x00\x00\x0c\x0c\x00\x00\x00\x01" + want


def test_dedupe_retry(peer):
    proto, port = peer
    c = FakeClient(port)
    bootstrap(c)

    # wait until the server has registered our ACKs, then drain any type-5
    # retransmission that was already in flight
    deadline = time.monotonic() + 2
    while proto.sessions[c.sid].unacked and time.monotonic() < deadline:
        time.sleep(0.02)
    c.drain()

    # identical retransmitted init datagram: re-ACKed, not reprocessed
    c.seq -= 1
    c.init()
    c.recv_ack(0)
    assert proto.sessions[c.sid].next_seq == 2  # still just reply + push
    c.expect_silence()  # no new DAT, no retransmissions (all acked)


def test_retransmission_and_batch_ack(peer):
    proto, port = peer
    c = FakeClient(port)
    c.syn()
    c.init()
    c.recv_ack(0)
    c.recv_dat(cmd=0x0001)
    c.recv_dat(cmd=0x012F)
    assert proto.sessions[c.sid].unacked  # two server DATs pending

    # unacked DATs come back as type 5 with the same seq (teardown.txt)
    deadline = time.monotonic() + 1.0
    retx_seqs = set()
    c.sock.settimeout(0.5)
    while time.monotonic() < deadline:
        try:
            data = c.recv()
        except TimeoutError:
            break
        if data[3] == 5:
            retx_seqs.add(int.from_bytes(data[:3], "little"))
    c.sock.settimeout(3)
    assert retx_seqs == {0, 1}

    # batched client ACK stops the retransmission
    c.ack_server(0, 1)
    time.sleep(0.25)  # a few retransmit intervals
    assert not proto.sessions[c.sid].unacked
    c.drain()  # discard type-5 retransmits that were already in flight
    c.expect_silence()


def test_two_clients_independent(peer):
    proto, port = peer
    c1, c2 = FakeClient(port), FakeClient(port)

    c1.syn()
    c2.syn()
    ctr1, ctr2 = bytes.fromhex("aaaaaaaa"), bytes.fromhex("bbbbbbbb")
    c1.init(ctr1)
    c2.init(ctr2)

    # interleaved: each client gets its own ACK and echo, own sequence space
    c1.recv_ack(0)
    c2.recv_ack(0)
    seq1, _, _, pt1 = c1.recv_dat(cmd=0x0001)
    seq2, _, _, pt2 = c2.recv_dat(cmd=0x0001)
    assert seq1 == seq2 == 0  # independent per-session server sequence counters
    assert pt1[1:5] == ctr1
    assert pt2[1:5] == ctr2
    assert pt1 != pt2  # different counters and observed ports
    c1.ack_server(seq1)
    c2.ack_server(seq2)

    # heartbeats stay independent
    c1.dat(0x0001, ctr1 + MAGIC_CONST, ver=0)
    c2.dat(0x0001, ctr2 + MAGIC_CONST, ver=0)
    c1.recv_ack(1)
    c2.recv_ack(1)
    assert c1.recv_dat(cmd=0x0001)[3][1:5] == ctr1
    assert c2.recv_dat(cmd=0x0001)[3][1:5] == ctr2

    assert set(proto.sessions) == {c1.sid, c2.sid}
    assert proto.sessions[c1.sid].keys != proto.sessions[c2.sid].keys


def test_probe_listener_redirect():
    proto = DiarkisProtocol(probe=True, retransmit_interval=0.1)
    listening = blockingCallFromThread(reactor, reactor.listenUDP, 0, proto)
    try:
        port = listening.getHost().port
        c = FakeClient(port)
        c.syn()
        c.init()
        c.recv_ack(0)
        seq, _, env, _ = c.recv_dat(cmd=0x0001)
        assert (env.ver, env.status) == (0, 1)
        c.ack_server(seq)

        # probe mode suppresses the 0x012f push (sessionhost-162-163.txt)
        time.sleep(0.25)
        assert proto.sessions[c.sid].next_seq == 1  # only the init reply

        # cmd 0x55f0 -> 0x0002 redirect push (beeffeed + target), ver 0, status 0xff
        c.dat(0x55F0, b"\x00" * 4, ver=1)
        c.recv_ack(1)
        seq, _, env, pt = c.recv_dat(cmd=0x0002)
        assert (env.ver, env.status) == (0, 0xFF)
        target = REDIRECT_ADDR.encode()
        assert pt == bytes.fromhex("beeffeed") + target
        c.ack_server(seq)
    finally:
        blockingCallFromThread(reactor, listening.stopListening)
        proto.shutdown()


def test_unknown_sid_syn_dropped(peer):
    _, port = peer
    c = FakeClient(port)
    c.sid = b"\xde" * 16  # never registered
    c.sock.settimeout(0.5)
    c.send(b"\x00\x00\x00\x02" + c.sid)
    with pytest.raises(TimeoutError):
        c.recv()


def test_http_handout_keys_open_udp_session(peer):
    """T3 wiring proof: keys from battle/get_diarkis_matching_server_info work
    on the UDP peer via the shared in-process registry."""
    import msgpack
    from django.test import Client

    _, port = peer
    body = msgpack.packb([{"session": ""}, []])
    resp = Client().post(
        "/dbtb-prd/LALALALALA/025348/api/battle/get_diarkis_matching_server_info",
        data=body,
        content_type="application/x-www-form-urlencoded",
    )
    _, _, sid, aes, iv, mac = msgpack.unpackb(resp.content, raw=False)[1][1]
    keys = crypto.KeySet(
        sid=bytes.fromhex(sid),
        key=bytes.fromhex(aes),
        iv=bytes.fromhex(iv),
        mac=bytes.fromhex(mac),
    )
    c = FakeClient(port, keys=keys)
    c.syn()  # raises unless the UDP peer ACKs — proves the registry is shared
    c.init()
    c.recv_ack(0)
    seq, _, _, pt = c.recv_dat(cmd=0x0001)
    assert pt[:5] == b"\x01" + INIT_COUNTER
    c.ack_server(seq)

"""Extract the official server's API traffic from the Wireshark captures (decrypted with the TLS key log).

For every HTTP/2 request to dbtb-prd.cosmos.channel.or.jp (and the login host cosmos.channel.or.jp) it pairs the request with its reply, decodes both
MessagePack bodies, and writes:
    <out>/all_calls.jsonl          one line per call: capture, time, path, request, reply
    <out>/by_endpoint/<name>.json  up to 3 examples per endpoint (readable JSON)
Only traffic to the official game host is kept; nothing else from the captures is read into the output.
Usage:  python extract_official_api.py <captures dir> <keylog file> <out dir>
"""
import json
import os
import subprocess
import sys
from collections import defaultdict

TSHARK = r"C:\Program Files\Wireshark\tshark.exe"
HOST = "dbtb-prd.cosmos.channel.or.jp"


# ---- minimal MessagePack decoder (no third-party package needed) ----
def unpack_all(data):
    out, i = [], 0
    while i < len(data):
        v, i = _unpack(data, i)
        out.append(v)
    return out


def _unpack(d, i):
    b = d[i]
    i += 1
    if b <= 0x7F: return b, i
    if 0x80 <= b <= 0x8F: return _map(d, i, b & 0x0F)
    if 0x90 <= b <= 0x9F: return _arr(d, i, b & 0x0F)
    if 0xA0 <= b <= 0xBF: n = b & 0x1F; return d[i:i + n].decode("utf-8", "replace"), i + n
    if b >= 0xE0: return b - 0x100, i
    def u(n): return int.from_bytes(d[i:i + n], "big")
    def s(n): return int.from_bytes(d[i:i + n], "big", signed=True)
    if b == 0xC0: return None, i
    if b == 0xC2: return False, i
    if b == 0xC3: return True, i
    if b in (0xC4, 0xC5, 0xC6):
        k = {0xC4: 1, 0xC5: 2, 0xC6: 4}[b]; n = u(k); i += k
        return {"bin": d[i:i + n].hex()}, i + n
    if b == 0xCA:
        import struct; return struct.unpack(">f", d[i:i + 4])[0], i + 4
    if b == 0xCB:
        import struct; return struct.unpack(">d", d[i:i + 8])[0], i + 8
    for code, n, signed in ((0xCC, 1, 0), (0xCD, 2, 0), (0xCE, 4, 0), (0xCF, 8, 0),
                            (0xD0, 1, 1), (0xD1, 2, 1), (0xD2, 4, 1), (0xD3, 8, 1)):
        if b == code:
            return (s(n) if signed else u(n)), i + n
    if b in (0xD9, 0xDA, 0xDB):
        k = {0xD9: 1, 0xDA: 2, 0xDB: 4}[b]; n = u(k); i += k
        return d[i:i + n].decode("utf-8", "replace"), i + n
    if b in (0xDC, 0xDD):
        k = 2 if b == 0xDC else 4; n = u(k); return _arr(d, i + k, n)
    if b in (0xDE, 0xDF):
        k = 2 if b == 0xDE else 4; n = u(k); return _map(d, i + k, n)
    if b in (0xD4, 0xD5, 0xD6, 0xD7, 0xD8):
        n = {0xD4: 1, 0xD5: 2, 0xD6: 4, 0xD7: 8, 0xD8: 16}[b]
        return {"ext": d[i], "data": d[i + 1:i + 1 + n].hex()}, i + 1 + n
    if b in (0xC7, 0xC8, 0xC9):
        k = {0xC7: 1, 0xC8: 2, 0xC9: 4}[b]; n = u(k); i += k
        return {"ext": d[i], "data": d[i + 1:i + 1 + n].hex()}, i + 1 + n
    raise ValueError("bad msgpack byte 0x%02x" % b)


def _arr(d, i, n):
    out = []
    for _ in range(n):
        v, i = _unpack(d, i); out.append(v)
    return out, i


def _map(d, i, n):
    out = {}
    for _ in range(n):
        k, i = _unpack(d, i); v, i = _unpack(d, i); out[str(k)] = v
    return out, i


def decode(hexdata):
    if not hexdata:
        return None
    raw = bytes.fromhex(hexdata)
    try:
        vals = unpack_all(raw)
        return vals if len(vals) != 1 else vals[0]
    except Exception:
        try:
            return {"text": raw.decode("utf-8")}
        except Exception:
            return {"raw": raw.hex()}


def calls_in(path, keylog):
    # Streams that went to the official host (TLS SNI)
    # The game API is on dbtb-prd.cosmos.channel.or.jp; login / startup (/000000/api/user/auth, sys/get_env_v3, ...)
    # go to cosmos.channel.or.jp itself. Both are kept; nothing else.
    snis = subprocess.run([TSHARK, "-r", path, "-n", "-Y",
                           'tls.handshake.extensions_server_name == "%s" || tls.handshake.extensions_server_name == "%s"'
                           % (HOST, HOST.split(".", 1)[1]),
                           "-T", "fields", "-e", "tcp.stream"], capture_output=True, text=True).stdout.split()
    streams = sorted(set(snis))
    if not streams:
        return []
    # Structured JSON: every HTTP/2 frame keeps its own stream id, type, headers and data (2026-11-12: the first
    # version lined up flat field lists and attached replies to the wrong calls when frames shared a packet).
    flt = "(%s) && http2" % " || ".join("tcp.stream==%s" % s for s in streams)
    # --no-duplicate-keys: a packet holding two TLS records (e.g. a PING plus the last DATA piece of a reply) has
    # two "http2" layers; plain JSON output repeats the key and Python kept only the last one, so the final piece
    # of some big replies was lost (2026-11-12, 25 of 1,362 replies cut short). With the switch they become a list.
    raw = subprocess.run([TSHARK, "-r", path, "-n", "-o", "tls.keylog_file:%s" % keylog, "-Y", flt,
                          "-T", "json", "--no-duplicate-keys", "-J", "frame tcp http2"], capture_output=True).stdout
    packets = json.loads(raw.decode("utf-8", "replace") or "[]")
    state = {}   # (tcp stream, http2 stream) -> call
    for pkt in packets:
        layers = pkt["_source"]["layers"]
        t = float(layers["frame"]["frame.time_relative"])
        tcp = layers["tcp"]["tcp.stream"]
        to_server = layers["tcp"]["tcp.dstport"] == "443"
        h2 = layers.get("http2")
        frames = []
        for part in (h2 if isinstance(h2, list) else [h2]):
            if not part:
                continue
            st = part.get("http2.stream")
            frames += st if isinstance(st, list) else [st]
        for fr in frames:
            if not isinstance(fr, dict):
                continue
            sid, typ = fr.get("http2.streamid"), fr.get("http2.type")
            if sid in (None, "0") or typ not in ("0", "1"):
                continue
            key = (tcp, sid)
            call = state.setdefault(key, {"time": t, "path": None, "status": None, "req": "", "resp": ""})
            if typ == "1":
                hdrs = fr.get("http2.header", [])
                for h in (hdrs if isinstance(hdrs, list) else [hdrs]):
                    name, value = h.get("http2.header.name"), h.get("http2.header.value")
                    if name == ":path":
                        call["path"] = value
                    elif name == ":status":
                        call["status"] = value
            uri = fr.get("http2.request.full_uri")
            if uri and not call["path"]:
                call["path"] = "/" + uri.split("://", 1)[-1].split("/", 1)[-1]
            if typ == "0" and fr.get("http2.data.data"):
                chunk = fr["http2.data.data"].replace(":", "")
                side = "req" if to_server else "resp"
                # Wireshark puts the whole reassembled body on the last DATA frame of a multi-frame body: when
                # a frame carries more bytes than its own length, it is the complete body, not one more piece.
                if len(chunk) // 2 > int(fr.get("http2.length", "0")):
                    call[side] = chunk
                else:
                    call[side] += chunk
    done = []
    for (tcp, sid), c in sorted(state.items(), key=lambda kv: kv[1]["time"]):
        if c["path"]:
            done.append({"time": c["time"], "path": c["path"], "status": c["status"],
                         "request": decode(c["req"]), "reply": decode(c["resp"])})
    return done


def main():
    cap_dir, keylog, out_dir = sys.argv[1:4]
    os.makedirs(os.path.join(out_dir, "by_endpoint"), exist_ok=True)
    examples = defaultdict(list)
    total = 0
    with open(os.path.join(out_dir, "all_calls.jsonl"), "w", encoding="utf-8") as allf:
        for root, _, files in os.walk(cap_dir):
            for name in sorted(files):
                if not name.lower().endswith(".pcapng"):
                    continue
                path = os.path.join(root, name)
                rel = os.path.relpath(path, cap_dir)
                calls = calls_in(path, keylog)
                for c in calls:
                    c["capture"] = rel
                    allf.write(json.dumps(c, ensure_ascii=False) + "\n")
                    ep = c["path"].split("?")[0]
                    ep = "/".join(ep.split("/")[3:]) if ep.count("/") >= 3 else ep
                    if len(examples[ep]) < 3:
                        examples[ep].append(c)
                total += len(calls)
                print("%-60s %d calls" % (rel, len(calls)), flush=True)
    for ep, ex in examples.items():
        with open(os.path.join(out_dir, "by_endpoint", ep.replace("/", "__") + ".json"), "w", encoding="utf-8") as f:
            json.dump(ex, f, ensure_ascii=False, indent=1)
    print("total calls:", total, "endpoints:", len(examples))


if __name__ == "__main__":
    main()

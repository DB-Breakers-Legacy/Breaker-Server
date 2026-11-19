"""Replay the official requests against OUR server and compare reply layouts with the official replies.

For every endpoint in all_calls.jsonl (from extract_official_api.py) the first decoded official call is taken; its
arguments (not its header: no official user id or session is sent) go to our server under a throw-away test
account, and our reply's layout is compared with the official reply's layout (list lengths and value types,
recursively; values are not compared). Friend / block / report calls are skipped (they carry other players' ids).

Run our server first on a spare port with a throw-away database, e.g. (from BreakerServer\\BreakerServer):
    dotnet ..\\..\\Bin\\Release\\BreakerServer.dll --urls http://127.0.0.1:5077
        --ConnectionStrings:Breaker="Data Source=<scratch>\\replay.db" --Breaker:UdpPort=7177 ...
Usage:  python replay_official_calls.py <official api reference dir> <server base url> <out .md>
Python standard library only (urllib); MessagePack encode/decode below.
"""
import json
import os
import struct
import sys
import urllib.error
import urllib.request
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_official_api import unpack_all  # noqa: E402  (same decoder the extractor uses)

SKIP_PREFIXES = ("friend/", "block/", "report/")
TEST_USER = 76561198000000077          # throw-away test account (local ticket, like ProtocolTests)


# ---- minimal MessagePack encoder ----
def pack(v):
    if v is None:
        return b"\xc0"
    if v is True:
        return b"\xc3"
    if v is False:
        return b"\xc2"
    if isinstance(v, int):
        if 0 <= v <= 0x7F:
            return bytes([v])
        if -32 <= v < 0:
            return struct.pack("b", v)
        if 0 <= v <= 0xFFFFFFFF:
            return b"\xce" + struct.pack(">I", v)
        if 0 <= v:
            return b"\xcf" + struct.pack(">Q", v)
        return b"\xd3" + struct.pack(">q", v)
    if isinstance(v, float):
        return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        if len(b) < 32:
            return bytes([0xA0 | len(b)]) + b
        if len(b) < 0x10000:
            return b"\xda" + struct.pack(">H", len(b)) + b
        return b"\xdb" + struct.pack(">I", len(b)) + b
    if isinstance(v, (list, tuple)):
        head = bytes([0x90 | len(v)]) if len(v) < 16 else b"\xdc" + struct.pack(">H", len(v))
        return head + b"".join(pack(x) for x in v)
    if isinstance(v, dict):
        if "bin" in v and len(v) == 1:
            raw = bytes.fromhex(v["bin"])
            return b"\xc6" + struct.pack(">I", len(raw)) + raw
        head = bytes([0x80 | len(v)]) if len(v) < 16 else b"\xde" + struct.pack(">H", len(v))
        return head + b"".join(pack(k) + pack(x) for k, x in v.items())
    raise TypeError(type(v))


def local_ticket(user_id):
    t = bytearray(32)
    t[0:8] = b"BRVAUTH1"
    t[8:12] = struct.pack("<I", 1)
    t[12:20] = struct.pack("<Q", user_id)
    t[20:24] = struct.pack("<I", 1)
    h = 14695981039346656037
    for b in t[0:24]:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    t[24:32] = struct.pack("<Q", h)
    return bytes(t).hex().upper()


def post(base, path, body):
    req = urllib.request.Request(base + path, data=pack(body), method="POST",
                                 headers={"Content-Type": "application/x-messagepack"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            vals = unpack_all(r.read())
            return r.status, (vals[0] if len(vals) == 1 else vals)
    except urllib.error.HTTPError as e:
        return e.code, None


def shape(v, depth=0):
    if depth > 6:
        return "..."
    if isinstance(v, list):
        if not v:
            return "[]"
        if len(v) > 4 and all(type(x) == type(v[0]) for x in v):
            return "[%s x%d]" % (shape(v[0], depth + 1), len(v))
        return "[" + ", ".join(shape(x, depth + 1) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ", ".join("%s:%s" % (k, shape(x, depth + 1)) for k, x in sorted(v.items())) + "}"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    if isinstance(v, str):
        return "str"
    return "null" if v is None else type(v).__name__


def first_diff(a, b, path="body"):
    if type(a) != type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        return "%s: official %s, ours %s" % (path, shape(a)[:80], shape(b)[:80])
    if isinstance(a, list):
        if a and b and all(isinstance(x, list) for x in a + b):     # collection of records: compare one entry
            return first_diff(a[0], b[0], path + "[entry]")
        if len(a) != len(b) and not (len(a) > 4 or len(b) > 4):
            return "%s: official has %d values, ours %d" % (path, len(a), len(b))
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, "%s[%d]" % (path, i))
            if d:
                return d
    if isinstance(a, dict) and set(a) != set(b):
        return "%s: keys differ (official only: %s; ours only: %s)" % (
            path, sorted(set(a) - set(b))[:5], sorted(set(b) - set(a))[:5])
    return None


def main():
    ref_dir, base, out_md = sys.argv[1:4]
    base = base.rstrip("/")
    examples = OrderedDict()
    with open(os.path.join(ref_dir, "all_calls.jsonl"), encoding="utf-8") as f:
        for line in f:
            c = json.loads(line)
            ep = "/".join(c["path"].split("?")[0].split("/")[3:])
            r, q = c.get("reply"), c.get("request")
            if ep in examples or ep.startswith(SKIP_PREFIXES):
                continue
            if isinstance(r, list) and r and isinstance(r[0], dict) and isinstance(q, list) and q and isinstance(q[0], dict):
                examples[ep] = (c["path"].split("?")[0], q[1:], r[1:])

    status, auth = post(base, "/025348/api/user/auth", [{}, [TEST_USER, local_ticket(TEST_USER), 0]])
    if status != 200:
        sys.exit("login to our server failed: HTTP %s" % status)
    token = auth[1][1][0]
    meta = {"titleCd": "025348", "userId": token, "session": "", "platform": 3, "version": "09.01"}

    rows = []
    for ep, (path, args, official) in examples.items():
        if ep == "user/auth":
            rows.append((ep, "skipped", "login itself (used above)"))
            continue
        code, ours = post(base, "/dbtb-prd" + path, [meta] + args)
        if code == 404:
            code, ours = post(base, path, [meta] + args)
        if code != 200:
            rows.append((ep, "HTTP %s" % code, "our server refused the official request"))
            continue
        ours_body = ours[1:] if isinstance(ours, list) and ours and isinstance(ours[0], dict) else ours
        off = official[0] if len(official) == 1 else official
        mine = ours_body[0] if isinstance(ours_body, list) and len(ours_body) == 1 else ours_body
        d = first_diff(off, mine)
        rows.append((ep, "same shape" if d is None else "differs", d or ""))

    with open(out_md, "w", encoding="utf-8") as f:
        f.write("# Official requests replayed against our server\n\n")
        f.write("Each endpoint's first official request (arguments only, under a throw-away test account) sent to our "
                "server; reply layout compared with the official reply. Friend/block/report calls skipped.\n\n")
        counts = {}
        for _, st, _ in rows:
            counts[st] = counts.get(st, 0) + 1
        f.write("| Result | Endpoints |\n|---|---|\n" + "".join("| %s | %d |\n" % kv for kv in sorted(counts.items())) + "\n")
        f.write("| Endpoint | Result | First difference |\n|---|---|---|\n")
        for ep, st, note in sorted(rows, key=lambda r: (r[1] == "same shape", r[0])):
            f.write("| `%s` | %s | %s |\n" % (ep, st, note.replace("|", "/")))
    print(counts)


if __name__ == "__main__":
    main()

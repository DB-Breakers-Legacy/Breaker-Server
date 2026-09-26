"""Golden-transcript gate: reproduce every annotated envelope byte-for-byte.

Auto-skips unless DIARKIS_TRANSCRIPTS_DIR points at the private transcripts
(these never leave the private repos).

Artifacts per transcript:

  <name>.txt        annotated decode transcript. Each DAT/UDP envelope line
                    records direction, ver, cmd, status (s->c), plen, the
                    acceptance note, and the decrypted plaintext as hex
                    (inline `pt=` in compact files, `pt[N]=` follow-up lines
                    otherwise). SYN lines map stream tags to session SIDs.
  <name>.keys.json  list of {"sid","key","iv","mac","client"} (hex; client =
                    client endpoint as ip:port), matched to the transcript by
                    SID. Sessions that share a key set (redirect probe + full
                    session) appear once.

The wire ciphertext is deterministic per (key set, plaintext): the CBC IV is
fixed per session and padding is zeros. So per annotated envelope the test
rebuilds the exact wire body from (pt, plen, keys), then runs the codec
pipeline (envelope.parse + crypto.decrypt) and asserts the recorded
plaintext, plen and acceptance note come back out — including the M4 edge
cases (plen0-nonzero-tail, pad-ext). Keyless baseline transcripts
(bootstrap/directory/heartbeat/teardown, raw-annotated format) are skipped
with a reported count.

Compact-format transcripts (matchmaking-stream43, sessionhost-162-163)
truncate the recorded pt to 96 bytes when the plaintext is longer; those
envelopes are prefix-checked (note assertion skipped — the pad tail is not
recoverable from a truncated plaintext).
"""

import hashlib
import hmac
import json
import os
import re
from collections import defaultdict

import pytest
from Crypto.Cipher import AES

from diarkis import crypto, envelope

pytestmark = pytest.mark.golden

TRANSCRIPTS_DIR = os.environ.get("DIARKIS_TRANSCRIPTS_DIR")

if not TRANSCRIPTS_DIR:
    pytest.skip("DIARKIS_TRANSCRIPTS_DIR not set", allow_module_level=True)

# f 985087 t=    0.059 s621 c->s DAT ver=0 cmd=1(0x0001) sid=OK plen=53 hmac=OK [note]
# f  4696 t=    0.066 c->s DAT ver=0 cmd=1(0x0001) sid=OK plen=53 hmac=OK pt=<hex>  (compact)
ENV_RE = re.compile(
    r"^f\s+(\d+) t=\s*\S+ (?:s(\d+) )?(c->s|s->c) (?:DAT|UDP) "
    r"ver=(\d+) cmd=(\d+)\(0x[0-9a-f]+\)(?: status=0x([0-9a-f]+))? "
    r"(?:sid=\w+ )?plen=(\d+) hmac=(OK|FAIL)(?: pt=([0-9a-f]*))?(?: \[(.*)\])?$"
)
# f 985087 t=    0.059 s621     pt[53]=<hex>
PT_RE = re.compile(r"^f\s+(\d+) t=\s*\S+ (?:s(\d+) )?\s+pt\[(\d+)\]=([0-9a-f]*)$")
SYN_RE = re.compile(r"(?:s(\d+) )?c->s SYN seq=\d+ sid=([0-9a-f]{32})")


def _rebuild_body(pt, wire_plen, keys, from_client):
    """Rebuild the exact wire body the sender produced for (pt, wire_plen)."""
    pad = 16 - (len(pt) % 16)  # sender always pads to the next block boundary
    ct = AES.new(keys.key, AES.MODE_CBC, keys.iv).encrypt(pt + b"\x00" * pad)
    blob = wire_plen.to_bytes(4, "big") + hmac.new(keys.mac, ct, hashlib.sha256).digest() + ct
    return (keys.sid + blob) if from_client else blob


def _check_envelope(name, info, failures, stats, capture):
    keys = info["keys"]
    from_client = info["from_client"]
    pt = bytes.fromhex(info["pt"])
    note = info["note"]
    # compact files truncate the recorded pt at 96 bytes
    truncated = len(pt) < info["plen"]
    # for plen0-nonzero-tail the annotated plen is the recovered content
    # length; the wire plen field was 0
    wire_plen = 0 if note == "plen0-nonzero-tail" else info["plen"]
    body = _rebuild_body(pt, wire_plen, keys, from_client)
    payload = envelope.build(info["cmd"], body, ver=info["ver"], status=info["status"])

    env = envelope.parse(payload, from_client)
    res = crypto.decrypt(env.body, keys, from_client=from_client)

    ok = (
        env.ver == info["ver"]
        and env.cmd == info["cmd"]
        and env.status == info["status"]
        and res.hmac_ok
        and res.plen == info["plen"]
        and (res.pt or b"").startswith(pt)
    )
    if truncated:
        # pad tail unrecoverable from a truncated pt — skip the note check
        if ok:
            stats[f"{capture}:verified-truncated"] += 1
        else:
            failures.append(
                f"{name} f{info['frame']} cmd={info['cmd']:#06x}: truncated prefix mismatch"
            )
        return
    ok = ok and res.pt == pt
    if note is None:
        ok = ok and res.note == ""
    elif note == "plen0-nonzero-tail":
        ok = ok and res.note == "plen0-nonzero-tail"
    elif note.startswith("pad-ext"):
        ok = ok and res.note.startswith("pad-ext")
    if ok:
        stats[f"{capture}:verified"] += 1
    else:
        failures.append(
            f"{name} f{info['frame']} cmd={info['cmd']:#06x}: plen={info['plen']} note={note} "
            f"-> plen={res.plen} hmac_ok={res.hmac_ok} note={res.note!r} pt_match={res.pt == pt}"
        )


def test_golden_transcripts():
    txts = sorted(f for f in os.listdir(TRANSCRIPTS_DIR) if f.endswith(".txt"))
    assert txts, f"no transcripts in {TRANSCRIPTS_DIR}"

    stats = defaultdict(int)
    failures = []
    for name in txts:
        path = os.path.join(TRANSCRIPTS_DIR, name)
        keys_path = os.path.join(TRANSCRIPTS_DIR, name[:-4] + ".keys.json")
        capture = name.split("-")[0]  # m34 / more-matches / matchmaking / sessionhost / baseline

        if not os.path.exists(keys_path):
            stats["keyless-skipped"] += 1  # raw-format baseline transcripts, permanently keyless
            continue

        with open(keys_path, encoding="ascii") as f:
            keysets = {
                ks["sid"]: crypto.KeySet(
                    sid=bytes.fromhex(ks["sid"]),
                    key=bytes.fromhex(ks["key"]),
                    iv=bytes.fromhex(ks["iv"]),
                    mac=bytes.fromhex(ks["mac"]),
                )
                for ks in json.load(f)
            }
        with open(path, encoding="utf-8") as f:
            text = f.read()
        stream_sid = {m.group(1) or "": m.group(2) for m in SYN_RE.finditer(text)}

        pending = None  # envelope awaiting its pt[...] follow-up line
        for line in text.splitlines():
            pt_m = PT_RE.match(line)
            if pt_m is not None:
                if pending is not None and pt_m.group(1) == pending["frame"]:
                    pending["pt"] = pt_m.group(4)
                    _check_envelope(name, pending, failures, stats, capture)
                    pending = None
                continue

            if pending is not None:
                failures.append(f"{name} f{pending['frame']}: envelope without pt line")
                pending = None

            if "DECRYPT-FAIL" in line:
                stats[f"{capture}:decrypt-fail-lines"] += 1
                continue
            if "DAT FRAG" in line or "REASSEMBLED" in line:
                stats[f"{capture}:fragment-lines"] += 1
                continue
            m = ENV_RE.match(line)
            if not m:
                continue

            frame, stream, direction, ver, cmd, status, plen, hmac_ok, inline_pt, note = m.groups()
            if hmac_ok != "OK":
                stats[f"{capture}:hmac-fail-lines"] += 1
                continue
            sid = stream_sid.get(stream or "")
            keys = keysets.get(sid)
            if keys is None:
                failures.append(f"{name} f{frame}: no key set for stream {stream} sid {sid}")
                continue
            info = {
                "frame": frame,
                "from_client": direction == "c->s",
                "ver": int(ver),
                "cmd": int(cmd),
                "status": None if status is None else int(status, 16),
                "plen": int(plen),
                "note": note,
                "pt": inline_pt,
                "keys": keys,
            }
            if inline_pt is None:  # long format: pt arrives on the next line
                pending = info
            else:
                _check_envelope(name, info, failures, stats, capture)

        if pending is not None:
            failures.append(f"{name} f{pending['frame']}: envelope without pt line")

    for k in sorted(stats):
        print(f"  {k}: {stats[k]}")
    assert not failures, "golden mismatches:\n" + "\n".join(failures[:20])
    assert stats["keyless-skipped"] == 4, "expected the 4 keyless baseline transcripts"
    assert any(k.endswith(":verified") for k in stats), "no envelopes verified"

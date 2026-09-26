"""Golden-transcript gate: decrypt the private capture transcripts byte-for-byte.

Auto-skips unless DIARKIS_TRANSCRIPTS_DIR points at the private transcripts
(these never leave the private repos). Expected format per transcript:

  <name>.tsv        Wireshark TSV export; column 8 is the UDP payload as hex
                    (colon-separated ok), column 4 is the source address.
  <name>.keys.json  {"sid","key","iv","mac"} hex + "client" = client src address

Every DAT/UDP envelope body in every transcript must decrypt (pad-extension
notes are accepted per the M4 rules; outright failures are not).
"""

import json
import os

import pytest

from diarkis import crypto, envelope, fragment, header

pytestmark = pytest.mark.golden

TRANSCRIPTS_DIR = os.environ.get("DIARKIS_TRANSCRIPTS_DIR")

if not TRANSCRIPTS_DIR:
    pytest.skip("DIARKIS_TRANSCRIPTS_DIR not set", allow_module_level=True)


def _load_tsv(path):
    rows = []
    for line in open(path, encoding="ascii"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 8 or not parts[7]:
            continue
        rows.append({"src": parts[3], "data": bytes.fromhex(parts[7].replace(":", ""))})
    return rows


def test_golden_transcripts():
    tsvs = sorted(
        f
        for f in os.listdir(TRANSCRIPTS_DIR)
        if f.endswith(".tsv")
        and os.path.exists(os.path.join(TRANSCRIPTS_DIR, f[:-4] + ".keys.json"))
    )
    assert tsvs, f"no <name>.tsv + <name>.keys.json pairs in {TRANSCRIPTS_DIR}"

    total_decoded = 0
    failures = []
    for name in tsvs:
        with open(os.path.join(TRANSCRIPTS_DIR, name[:-4] + ".keys.json")) as f:
            ks = json.load(f)
        keys = crypto.KeySet(
            sid=bytes.fromhex(ks["sid"]),
            key=bytes.fromhex(ks["key"]),
            iv=bytes.fromhex(ks["iv"]),
            mac=bytes.fromhex(ks["mac"]),
        )
        client = ks["client"]
        reassemblers = {}
        for row in _load_tsv(os.path.join(TRANSCRIPTS_DIR, name)):
            if len(row["data"]) < 4:
                continue
            _, ptype, payload = header.parse(row["data"])
            if ptype not in (1, 3):
                continue
            from_client = row["src"] == client

            frag = fragment.parse(payload)
            if frag is None and from_client:
                # envelope-first c->s fragment wrap: env + SID + frag header + chunk
                hdr = envelope.HEADER_LEN_CS
                if (
                    payload[:4] == envelope.MAGIC
                    and payload[hdr : hdr + 16] == keys.sid
                    and payload[hdr + 16 : hdr + 20] == fragment.MAGIC
                ):
                    frag = fragment.parse(payload, hdr + 16)
            if frag is not None:
                blob = reassemblers.setdefault(from_client, fragment.Reassembler()).add(frag)
                if blob is None:
                    continue
                payload = blob

            for env in envelope.parse_all(payload, from_client):
                res = crypto.decrypt(env.body, keys, from_client=from_client)
                if res.pt is None:
                    failures.append(f"{name} cmd={env.cmd:#06x} {res.note}")
                else:
                    total_decoded += 1

    assert not failures, "decrypt failures:\n" + "\n".join(failures[:20])
    assert total_decoded > 0, "no envelopes decoded — check transcript format"

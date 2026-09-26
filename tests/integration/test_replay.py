"""Replay integration test: real harvested client request bytes vs our stubs.

Marked `integration`; auto-skips unless DIARKIS_HARVEST_DIR points at a pulled
harvest (`index.csv` + per-call `.bin` files from `matchmaking data.pcapng`).
The harvest is real captured traffic — it lives under the control plane's
exports/ and is never committed.

For every harvested call whose route we implement, the real request body is
POSTed verbatim to the real route via the Django test client; assert HTTP 200
and `result: 0` in the response meta.

What this proves: our endpoint stubs accept the real client request bytes
(msgpack layout, arg shapes, sizes) and answer with a well-formed `result:0`
response for every implemented call in the real matchmaking sequence.

What it does NOT prove: anything about response CONTENT the client acts on
(stubs are canned), session-token validation (the harvest's rolling tokens
don't match a fresh server — our server doesn't enforce them anyway), Steam
ticket validation (stubbed upstream), or the UDP layer (covered separately).

Decode gotcha (HANDOFF.md): some harvested bodies contain mid-stream duplicate
TCP-reassembly fragments — decode with a streaming Unpacker and take the last
complete top-level object (raw=False, raw=True fallback).

DB note: the harvested user/auth registers args[0] (a Steam id) while the
025348 calls address the user by meta userId — so after replaying auth, the
test registers the meta userId through the same endpoint (documented upstream
quirk: the real server derived one from the other; ours doesn't yet).
"""

import csv
import os

import msgpack
import pytest
from django.test import Client
from django.urls import resolve
from django.urls.exceptions import Resolver404

pytestmark = pytest.mark.integration

HARVEST_DIR = os.environ.get("DIARKIS_HARVEST_DIR")

if not HARVEST_DIR:
    pytest.skip("DIARKIS_HARVEST_DIR not set", allow_module_level=True)


def unpack_body(raw):
    """Last complete top-level object (duplicate-fragment tolerant)."""
    for kwargs in ({"raw": False}, {"raw": True}):
        try:
            unpacker = msgpack.Unpacker(strict_map_key=False, **kwargs)
            unpacker.feed(raw)
            objects = list(unpacker)
            if objects:
                return objects[-1]
        except Exception:
            continue
    raise ValueError("body not decodable")


def route_for(path):
    if path.startswith("/000000/"):
        return "/LALALALALA" + path
    if path.startswith("/025348/"):
        return "/dbtb-prd/LALALALALA" + path
    return None  # not game traffic (Discord etc.)


@pytest.mark.django_db
def test_replay_harvest():
    with open(os.path.join(HARVEST_DIR, "index.csv"), newline="", encoding="utf-8-sig") as f:
        rows = sorted(csv.DictReader(f), key=lambda r: int(r["seq"]))

    client = Client()
    stats = {"replayed": 0, "skipped-noise": 0, "skipped-unimplemented": 0}
    failures = []
    unimplemented = set()
    user_id = None

    for row in rows:
        path = row["path"]
        route = route_for(path)
        if route is None:
            stats["skipped-noise"] += 1
            continue
        try:
            resolve(route)
        except Resolver404:
            stats["skipped-unimplemented"] += 1
            unimplemented.add(path)
            continue

        raw = open(os.path.join(HARVEST_DIR, row["req_file"]), "rb").read()
        meta, _args = unpack_body(raw)  # must be the [meta, args] envelope

        resp = client.post(route, data=raw, content_type="application/x-www-form-urlencoded")
        if resp.status_code != 200:
            failures.append(f"seq {row['seq']} {path}: HTTP {resp.status_code}")
            continue
        result = unpack_body(resp.content)[0]["result"]
        if result != 0:
            failures.append(f"seq {row['seq']} {path}: result={result}")
            continue
        stats["replayed"] += 1

        if path == "/000000/api/user/auth":
            # the 025348 calls address the user by meta userId; register it too
            for later in rows:
                if later["path"].startswith("/025348/"):
                    later_raw = open(os.path.join(HARVEST_DIR, later["req_file"]), "rb").read()
                    user_id = unpack_body(later_raw)[0]["userId"]
                    break
            if user_id:
                body = msgpack.packb([meta, [user_id]])
                resp = client.post(
                    route, data=body, content_type="application/x-www-form-urlencoded"
                )
                assert resp.status_code == 200

    for k, v in stats.items():
        print(f"  {k}: {v}")
    if unimplemented:
        print("  unimplemented endpoints:", ", ".join(sorted(unimplemented)))
    assert not failures, "replay failures:\n" + "\n".join(failures[:20])
    assert stats["replayed"] > 0

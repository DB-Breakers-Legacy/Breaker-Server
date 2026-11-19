"""Give a player Raiders on OUR local server (2026-11-16). Private testing only.

    python -I unlock_raider.py <breaker.db> <rival-templates.json> --raider 600 --raider 1300 [--user <id>] [--write]

Why: the project lead asked "unlock both broly and Z broly for me" (Raider 600 = Broly, 1300 = Z Broly; the Raider
ids are the first cell of RivalCharaInfoList.rcil).
What it does = exactly what the test shop does when a Raider is bought (ShopEndpoints.GrantAsync, "RivalID"):
PatrollerUsers.RivalListJson is the rival/get_list reply [status, [entry, ...]]; a Raider the player does not own
yet gets the entry from the server's Assets/rival-templates.json ([id, level 1, 0, 0, skill list 1, skill list 2,
unlock list] - skill ids copied from an official reply) appended. Nothing is charged.

Without --write it only shows what would change. With --write: the database is copied next to it first
(breaker.db.before-raiders-<time>, through SQLite's backup so a running server is fine), only this user's
RivalListJson changes, and the result is read back. The server reads the account from the database on every
request, so it needs no restart; the game asks for its Raider list when it starts / returns to the lobby.
"""
import argparse
import copy
import datetime
import json
import os
import sqlite3
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db")
    ap.add_argument("templates")
    ap.add_argument("--user", type=int)
    ap.add_argument("--raider", type=int, action="append", default=[])
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()

    templates = json.load(open(a.templates, encoding="utf-8"))["rivals"]
    con = sqlite3.connect(a.db)
    users = con.execute("select UserId from PatrollerUsers").fetchall()
    uid = a.user if a.user is not None else (users[0][0] if len(users) == 1 else None)
    if uid is None:
        sys.exit(f"{len(users)} players in this database - say which with --user")
    cols = [d[1] for d in con.execute("pragma table_info(PatrollerUsers)")]
    before = con.execute("select * from PatrollerUsers where UserId=?", (uid,)).fetchone()
    if before is None:
        sys.exit(f"no player {uid}")
    raw = before[cols.index("RivalListJson")]
    data = json.loads(raw) if raw else None
    if not (isinstance(data, list) and len(data) == 2 and isinstance(data[1], list)
            and all(isinstance(e, list) and len(e) == 7 and isinstance(e[0], int) for e in data[1])):
        sys.exit("unexpected Raider list in the database - nothing changed")
    owned = [e[0] for e in data[1]]
    print(f"  player {uid} owns Raiders {owned}")
    if not a.raider:
        print("  templates exist for", sorted(int(k) for k in templates))
        return 0
    old = copy.deepcopy(data)
    added = []
    for rid in a.raider:
        if rid in owned or rid in added:
            print(f"  Raider {rid}: already owned")
            continue
        entry = templates.get(str(rid))
        if not (isinstance(entry, list) and len(entry) == 7 and entry[0] == rid):
            sys.exit(f"Raider {rid}: no usable template in {os.path.basename(a.templates)} - nothing changed")
        data[1].append(copy.deepcopy(entry))
        added.append(rid)
        print(f"  Raider {rid}: + {json.dumps(entry, separators=(',', ':'))[:150]}...")
    if not added:
        print("nothing to change")
        return 0
    assert data[1][:len(old[1])] == old[1] and data[0] == old[0]           # the owned entries stay as they are
    new_raw = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    if not a.write:
        print("dry run - nothing written (add --write)")
        return 0

    backup = f"{a.db}.before-raiders-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    dst = sqlite3.connect(backup)
    with dst:
        con.backup(dst)
    dst.close()
    with con:
        con.execute("update PatrollerUsers set RivalListJson=? where UserId=?", (new_raw, uid))
    after = con.execute("select * from PatrollerUsers where UserId=?", (uid,)).fetchone()
    changed = [cols[i] for i in range(len(cols)) if before[i] != after[i]]
    back = json.loads(after[cols.index("RivalListJson")])
    ok = changed == ["RivalListJson"] and back == data and [e[0] for e in back[1]] == owned + added
    print(f"written; backup {os.path.basename(backup)}; changed columns {changed}; Raiders now "
          f"{[e[0] for e in back[1]]}; read back ok: {ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

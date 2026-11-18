"""Unlock Transphere forms / supers for a player on OUR local server (2026-11-15). Private testing only.

    python -I unlock_transphere.py <breaker.db> [--write] [--user <id>]
        --form <char>:<form>    e.g. --form 1:7   (Goku Transphere, form 7 = SS4)
        --super <char>:<id>     e.g. --super 8:3200400
        --outfit <char>:<id>    e.g. --outfit 1:10  (Goku SS4's outfit; each form wears only the outfits listed in
                                TransBallDataList.dt cols 5-28 - 2026-11-15: SS4 forms wear outfit 10)
        --row <char>            add an empty Transphere row (form 1) if the player has none

Where it lives (checked against the game's reader sub_1407D0710 and the official character/get replies):
PatrollerUsers.UserCharactersJson = the character/get reply [0, rows, part2, skills, part4, part5]; each row =
[character id, date, forms owned [[form, date]...], OUTFITS owned [[n, date]...], supers owned [[id, date]...],
skills [[id, date]...]]. Character ids = TransBallInfoList.tbil col 0 (1 Goku, 8 Vegeta, ...); form numbers =
TransBallDataList.dt cols 0/1 (Goku 7 = GokuSS4, Vegeta 7 = VegetaSS4).

Without --write it only shows what would change. With --write: the database is copied next to it first
(breaker.db.before-unlock-<time>), only this user's UserCharactersJson changes, and the result is read back.
"""
import argparse
import datetime
import json
import os
import shutil
import sqlite3
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--user", type=int)
    ap.add_argument("--form", action="append", default=[])
    ap.add_argument("--super", action="append", default=[])
    ap.add_argument("--outfit", action="append", default=[])
    ap.add_argument("--row", action="append", default=[], type=int)
    a = ap.parse_args()
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    con = sqlite3.connect(a.db)
    users = con.execute("select UserId from PatrollerUsers").fetchall()
    uid = a.user if a.user is not None else (users[0][0] if len(users) == 1 else None)
    if uid is None:
        sys.exit(f"{len(users)} players in this database - say which with --user")
    before = con.execute("select * from PatrollerUsers where UserId=?", (uid,)).fetchone()
    raw = con.execute("select UserCharactersJson from PatrollerUsers where UserId=?", (uid,)).fetchone()[0]
    data = json.loads(raw)
    rows = data[1]
    by_char = {r[0]: r for r in rows}
    changes = []

    for char in a.row:
        if char not in by_char:
            r = [char, now, [[1, now]], [[1, now]], [], []]
            rows.append(r)
            rows.sort(key=lambda x: x[0])
            by_char[char] = r
            changes.append(f"new Transphere row for character {char} (form 1)")

    def add(lst, value, what):
        if any(e[0] == value for e in lst):
            changes.append(f"{what}: already owned")
            return
        lst.append([value, now])
        lst.sort(key=lambda e: e[0])
        changes.append(f"{what}: added")

    for spec in a.form:
        char, form = (int(x) for x in spec.split(":"))
        if char not in by_char:
            sys.exit(f"player has no Transphere row for character {char} (use --row {char})")
        add(by_char[char][2], form, f"character {char} form {form}")
    for spec in a.super:
        char, sid = (int(x) for x in spec.split(":"))
        if char not in by_char:
            sys.exit(f"player has no Transphere row for character {char} (use --row {char})")
        add(by_char[char][4], sid, f"character {char} super {sid}")
    for spec in a.outfit:
        char, oid = (int(x) for x in spec.split(":"))
        if char not in by_char:
            sys.exit(f"player has no Transphere row for character {char} (use --row {char})")
        add(by_char[char][3], oid, f"character {char} outfit {oid}")

    for c in changes:
        print("  " + c)
    for char in sorted({int(s.split(":")[0]) for s in a.form + a.super + a.outfit} | set(a.row)):
        r = by_char[char]
        print(f"  character {char}: forms {[e[0] for e in r[2]]}  outfits {[e[0] for e in r[3]]}  supers {[e[0] for e in r[4]]}")
    new_raw = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    if new_raw == raw:
        print("nothing to change")
        return 0
    if not a.write:
        print("dry run - nothing written (add --write)")
        return 0

    con.close()
    backup = f"{a.db}.before-unlock-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    shutil.copy2(a.db, backup)
    con = sqlite3.connect(a.db)
    with con:
        con.execute("update PatrollerUsers set UserCharactersJson=? where UserId=?", (new_raw, uid))
    after = con.execute("select * from PatrollerUsers where UserId=?", (uid,)).fetchone()
    cols = [d[1] for d in con.execute("pragma table_info(PatrollerUsers)")]
    changed_cols = [cols[i] for i in range(len(cols)) if before[i] != after[i]]
    ok = changed_cols == ["UserCharactersJson"] and json.loads(after[cols.index("UserCharactersJson")]) == data
    print(f"written; backup {os.path.basename(backup)}; changed columns {changed_cols}; read back ok: {ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

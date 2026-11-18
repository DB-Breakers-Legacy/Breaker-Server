"""Set a player's Super Warrior Spirit on OUR local server (2026-11-16). Private testing only.

    python -I set_spirit.py <breaker.db> [--user <id>] [--spirit N] [--write]

Why: Transphere supers are bought with Spirit. The server's transball/unlock_spattack does not check or charge
anything - the GAME refuses the purchase when its own Spirit number (from patroller/get_status) is below the price.
Where it lives: PatrollerUsers.StatusJson = the patroller/get_status reply [status, part 1, ...]; part 1 =
[Possible_ level, Possible_ exp, zeni, super warrior spirit, Possible_ TP medals] (zeni = [2] confirmed 2026-11-13;
spirit = [3]: 2026-11-16 the test account had 5,090 left after buying the 25,000 10x Kamehameha, and the game
refused the 25,000 Final Shine Attack).

Without --spirit it only shows the current values. Without --write it only shows what would change. With --write:
the database is copied next to it first (breaker.db.before-spirit-<time>, through SQLite's backup so a running
server is fine), only this user's StatusJson changes, and the result is read back. The server reads the account from
the database on every request, so it needs no restart; the game shows the new number after its next status request
(restart the game or go back to the lobby).
"""
import argparse
import datetime
import json
import os
import sqlite3
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db")
    ap.add_argument("--user", type=int)
    ap.add_argument("--spirit", type=int)
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()

    con = sqlite3.connect(a.db)
    users = con.execute("select UserId from PatrollerUsers").fetchall()
    uid = a.user if a.user is not None else (users[0][0] if len(users) == 1 else None)
    if uid is None:
        sys.exit(f"{len(users)} players in this database - say which with --user")
    cols = [d[1] for d in con.execute("pragma table_info(PatrollerUsers)")]
    before = con.execute("select * from PatrollerUsers where UserId=?", (uid,)).fetchone()
    raw = before[cols.index("StatusJson")]
    if not raw:
        sys.exit("this player has no own status yet (still the starter status) - log in once first")
    data = json.loads(raw)
    part = data[1]
    if not (isinstance(part, list) and len(part) == 5 and all(isinstance(x, int) for x in part)):
        sys.exit(f"unexpected status part 1: {part}")
    print(f"  now: level {part[0]}, exp {part[1]}, zeni {part[2]}, spirit {part[3]}, medals {part[4]}")
    if a.spirit is None:
        return 0
    if not 0 <= a.spirit <= 9_999_999:
        sys.exit("spirit must be 0 .. 9,999,999")
    if part[3] == a.spirit:
        print("nothing to change")
        return 0
    old = part[3]
    part[3] = a.spirit
    new_raw = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    print(f"  spirit {old} -> {a.spirit}")
    if not a.write:
        print("dry run - nothing written (add --write)")
        return 0

    backup = f"{a.db}.before-spirit-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    dst = sqlite3.connect(backup)
    with dst:
        con.backup(dst)
    dst.close()
    with con:
        con.execute("update PatrollerUsers set StatusJson=? where UserId=?", (new_raw, uid))
    after = con.execute("select * from PatrollerUsers where UserId=?", (uid,)).fetchone()
    changed = [cols[i] for i in range(len(cols)) if before[i] != after[i]]
    back = json.loads(after[cols.index("StatusJson")])
    ok = changed == ["StatusJson"] and back == data and back[1][3] == a.spirit
    print(f"written; backup {os.path.basename(backup)}; changed columns {changed}; read back ok: {ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

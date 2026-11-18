"""Make an official save file for the DB Breakers Legacy server from a player's OWN capture of the official game.

The player records their own game traffic while logging in to the official server (Wireshark + TLS key log, the
team's capture guide); getting to the lobby once is enough. This script reads only the official game host's
traffic from that capture and writes, for every Steam account that logged in:
    <out dir>/<steamId>.json   = { format, steamId, capturedAt, capture, replies: {endpoint: official reply body} }
with the account's latest official replies of:
    patroller/get_status (level, rank, season pass), character/get (survivors, skills),
    character/get_item (costumes, items), rival/get_list (Raiders and their skills).
Nothing else goes into the file: no login ticket, no session, no TP balance, no other traffic.
The server owner puts the file into the server's import folder (Breaker:ImportDir); it is copied into that Steam
account the next time it logs in to our server.

Usage:
    python export_official_save.py <capture .pcapng or folder of them> <TLS key log> <out dir>
    python export_official_save.py --from-calls <all_calls.jsonl from extract_official_api.py> <out dir>
Needs Wireshark (tshark) for the first form; Python standard library only.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_official_api import calls_in  # noqa: E402  (same capture reader as the official API extractor)

FORMAT = "breakers-official-save-1"
NEEDED = ("patroller/get_status", "character/get", "character/get_item", "rival/get_list")


def endpoint(path):
    return "/".join(path.split("?")[0].split("/")[3:])


def collect(calls):
    """calls: iterable of (capture name, call). Returns {steamId: save dict}."""
    owner = {}     # official user id -> Steam id (from each user/auth: request [steamId, ticket, 0], reply [0, [userId, 0]])
    latest = {}    # (official user id, endpoint) -> (date, capture, body)
    for capture, c in calls:
        q, r = c.get("request"), c.get("reply")
        if not (isinstance(q, list) and len(q) >= 2 and isinstance(q[0], dict)
                and isinstance(r, list) and len(r) >= 2 and isinstance(r[0], dict)):
            continue
        ep, date = endpoint(c["path"]), str(r[0].get("date", ""))
        if ep == "user/auth":
            try:
                steam_id, user_id = str(q[1][0]), str(r[1][1][0])
            except (IndexError, TypeError, KeyError):
                continue
            if user_id:
                owner[user_id] = steam_id
            continue
        if ep in NEEDED and isinstance(r[1], list) and r[1] and r[1][0] == 0:
            key = (str(q[0].get("userId", "")), ep)
            if key not in latest or date >= latest[key][0]:
                latest[key] = (date, capture, r[1])

    saves = {}
    for user_id, steam_id in owner.items():
        found = {ep: latest.get((user_id, ep)) for ep in NEEDED}
        missing = [ep for ep, v in found.items() if v is None]
        if missing:
            print("Steam account %s: not enough in the capture (missing %s); log in to the lobby while recording"
                  % (steam_id, ", ".join(missing)))
            continue
        when = max(v[0] for v in found.values())
        saves[steam_id] = {
            "format": FORMAT,
            "steamId": steam_id,
            "capturedAt": when,
            "capture": sorted({v[1] for v in found.values()}),
            "replies": {ep: v[2] for ep, v in found.items()},
        }
    return saves


def main():
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--from-calls":
        with open(args[1], encoding="utf-8") as f:
            calls = [(c.get("capture", "?"), c) for c in map(json.loads, f)]
        out_dir = args[2]
    elif len(args) == 3:
        source, keylog, out_dir = args
        files = [source] if os.path.isfile(source) else [
            os.path.join(root, n) for root, _, names in os.walk(source) for n in sorted(names)
            if n.lower().endswith(".pcapng")]
        calls = []
        for path in files:
            found = calls_in(path, keylog)
            print("%s: %d official calls" % (os.path.basename(path), len(found)))
            calls += [(os.path.basename(path), c) for c in found]
    else:
        sys.exit(__doc__)

    os.makedirs(out_dir, exist_ok=True)
    saves = collect(calls)
    for steam_id, save in saves.items():
        path = os.path.join(out_dir, "%s.json" % steam_id)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(save, f, ensure_ascii=False)
        status = save["replies"]["patroller/get_status"]
        print("saved %s: level %s, %d survivors, %d Raiders, %d costumes (recorded %s)" % (
            path, status[1][0], len(save["replies"]["character/get"][1]),
            len(save["replies"]["rival/get_list"][1]), len(save["replies"]["character/get_item"][1]),
            save["capturedAt"]))
    if not saves:
        print("No complete save found.")


if __name__ == "__main__":
    main()

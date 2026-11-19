# Bringing progress over from the official server

Players can move their official progress to the DB Breakers Legacy server. This has to happen **while the
official servers are still online**, because the save is read from the player's own recording of their game.

## What carries over

| Carried over | Not carried over |
|---|---|
| Level, experience, Zeni | TP (premium currency): our server has none |
| Rank | Official mail / gifts in the mailbox |
| Season pass level and claimed rewards | Friends and block lists |
| Survivors, their skills and skill levels | |
| Raiders and their skills | |
| Costumes and other owned items | |

New players who don't bring a save start like a brand-new official account (level 1, 3 Raiders, the 10
starter costumes, no TP) and earn everything by playing.

## Player: make the save (once)

1. Follow the team's capture guide (mitmproxy key log + Wireshark), start recording, then start the game
   normally and log in to the **official** server.
2. Wait until you're in the lobby. That's enough: the game loads your whole account at that point.
3. Close the game, stop recording, and save the capture (.pcapng) and the key log.
4. Put `export_official_save.py` and `extract_official_api.py` (from `breakers server\scripts`) in one
   folder (Python 3 and Wireshark must be installed), then run:
   `python export_official_save.py <capture.pcapng> <keylog file> <output folder>`
   It prints e.g. `saved ...\7656119xxxxxxxxxx.json: level 531, 27 survivors, 13 Raiders, 451 costumes`.
5. Send **only that .json file** to the server owner. Never share the capture or the key log: they unlock
   everything your PC sent while recording.

The .json holds only the four account replies listed above and your Steam ID: no login ticket, no session,
no TP, nothing else from the capture.

## Server owner: import it

1. Put the file into `breakers server\save imports\` named `<steamId>.json` (the exporter already names it that way).
2. The next time that Steam account logs in to our server, the save is copied into it and the file is
   renamed to `<steamId>.imported-<date>.json`. A file that isn't a usable save is renamed to
   `<steamId>.rejected-<date>.json` and the server log says why; the account stays as it was.
3. Importing replaces that account's progress on our server. Dropping in a newer save later imports again.

Note: the server can't prove a save file is genuine (anyone could edit one). On a free server with nothing
for sale this only matters for fairness, so only import files you received from the player themselves.

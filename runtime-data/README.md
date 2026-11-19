# runtime-data/

Files the server needs at run time that are **not committed**: captured game data and local images.
Only this README and `runtime-data.sha256` are tracked; everything else here is gitignored.

| File | Needed for | Where it came from |
|---|---|---|
| `adjustmentdata.txt` | matchmaking (without it: "game data contains old data") | captured `adjustment_data_manage/read` reply |
| `news/legacy-news-test-05-niko.png` | optional custom lobby news image (`-CustomNews`) | local image |
| `save-imports/` | official save imports (`<steamId>.json`) | players, see docs/migrating-official-saves.md |
| `captures/` | request captures (`-Capture`) | written by the server |

`runtime-data.sha256` pins the expected contents; the start scripts refuse to run on a mismatch.
Get the files from a teammate (or the breakers-sandbox `work\runtime-data` folder) and check:

    Get-FileHash runtime-data\adjustmentdata.txt -Algorithm SHA256

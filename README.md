# Breaker-Server

Community server-preservation reimplementation for **DRAGON BALL: THE BREAKERS**
(title id `025348`), so the game remains playable after the official servers
shut down. Ported from the reference custom server ("ProjectTemporal") and
sanitized for public release.

GPL-3.0. Not affiliated with Bandai Namco. Never point this at official
infrastructure, Steam, or real player accounts — it is for local/private play
and preservation research only.

Current state: **lobby/menu HTTP API works; the actual match flow is not
implemented yet** (Phase 0).

## Components

1. **HTTP API** — Django 5.2 + MessagePack (`src/breakerapi/`, project
   `src/breaker_server/`). The client POSTs MessagePack bodies despite a
   form-urlencoded content-type; responses are msgpack with a rolling session
   token. CDN-impersonation middleware (`server: nginx`, `via: 1.1 google`).
2. **`diarkis/` codec package** (`src/diarkis/`) — bidirectional Diarkis RUDP
   wire codec: datagram header, `FE BE DE EF` command envelope, secure envelope
   (AES-128-CBC + HMAC-SHA256), `FF FE FD FC` fragmentation. Built from the
   capture-verified decoder logic.
3. **`RUDPserver.py`** (repo root) — Twisted UDP matchmaking handshake on port
   7100. **Phase 2 replaces this** (per-session keys, per-client state, codec
   move to `diarkis/`). Keys come from the environment, not source.
4. **STUN** (`stun/`) — external prerequisite (`stund`/STUNTMAN), config
   template + docs only, no binaries. Ports 3478/3479.

## Setup

```
py -3.12 -m venv .venv        # or: python -m venv .venv
.venv/Scripts/activate        # Windows; source .venv/bin/activate elsewhere
make install                  # or: python -m pip install -r requirements.txt -r requirements-dev.txt
cp .env.example .env          # fill it in (all committed values are placeholders)
make migrate
make run                      # python manage.py runserver
```

Requires Python 3.12. `make` targets are all `python -m ...` invocations; if
you don't have `make`, run the commands from the `Makefile` directly.

## Development

- `make lint` — ruff check + format check (`make format` to apply)
- `make test` — unit tests (synthetic codec fixtures + Django endpoint tests)
- `make test-golden` — codec gate against the private capture transcripts;
  auto-skips unless `DIARKIS_TRANSCRIPTS_DIR` points at them. Transcripts and
  other captured material live in the private documentation repo and are never
  committed here.
- `make test-integration` — placeholder (Phase 5)

## Known issues / deliberate omissions

- **Hypercorn `#BREADED` patch** (reference server patched
  `hypercorn/protocol/h2.py` to fix DATA_END on the last data packet) is not
  carried over — fragile across upgrades. HTTP/2 deployments may need an
  equivalent fix; run plain `runserver` for development.
- **channels/Redis** config from the reference server was dropped: websocket
  routes were commented out and unreachable (ASGI pointed at the plain Django
  application). Revisit if websockets ever become needed.
- `adjustmentdata.txt` (≈56 KB captured game blob) is not committed; serve your
  own copy via `ADJUSTMENT_DATA_PATH`.
- Canned leaderboard entries are synthetic placeholders.

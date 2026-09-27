# Breaker-Server

Community server-preservation reimplementation for **DRAGON BALL: THE BREAKERS**
(title id `025348`), so the game remains playable after the official servers
shut down. Ported from the reference custom server ("ProjectTemporal") and
rewritten in **C# / ASP.NET Core** for continued development and public release.

GPL-3.0. Not affiliated with Bandai Namco. Never point this at official
infrastructure, Steam, or real player accounts — it is for local/private play
and preservation research only.

Current state: **the C# server can progress the client through the menu and into
the tutorial flow, but entering the actual game is not implemented yet**.

## Components

1. **HTTP API** — ASP.NET Core backend (`BreakerServer/`) implementing the
   game's HTTP endpoints, request handling, MessagePack responses, session
   state, authentication, configuration, and supporting services.

2. **Diarkis protocol** (`BreakerServer/Protocol/` and `BreakerServer/Network/`)
   — C# implementation of the Diarkis RUDP protocol used by the game, including
   datagram handling, command envelopes, session packets, fragmentation, and
   protocol-specific message processing.

3. **Diarkis UDP services** (`BreakerServer/Network/`) — integrated UDP
   services used for matchmaking and session communication. Session state and
   keys are maintained by the server process so the HTTP and UDP systems can
   share the same runtime state.

4. **STUN** (`BreakerServer/Stun/`) — built-in C# STUN emulation used by the
   client for network discovery. The server listens on the configured STUN
   ports, including UDP 3478 and 3479.

## Setup

```powershell
dotnet restore BreakersRevived.sln
dotnet build BreakersRevived.sln
dotnet run --project BreakerServer
```

The project can also be opened directly in Visual Studio using:

```text
BreakersRevived.sln
```

Build and run the `BreakerServer` project normally from Visual Studio.

## Development

- `dotnet build BreakersRevived.sln` — build the complete C# solution.
- `dotnet run --project BreakerServer` — start the local server.
- `dotnet test` — run available automated tests.
- Server systems are separated into dedicated C# components under
  `BreakerServer/`, including authentication, configuration, endpoints,
  networking, protocol handling, services, STUN, and data handling.

## Known issues / deliberate omissions

- The C# rewrite is still under active development and does not currently
  complete the full game-entry flow.
- The client can currently progress through the server connection process and
  reach the tutorial flow.
- Match/session behavior is still being implemented.
- Some responses and game data are still placeholders while the remaining
  protocol behavior is researched and implemented.
- Canned leaderboard entries are synthetic placeholders.

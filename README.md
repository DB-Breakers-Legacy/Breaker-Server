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

## How to run it

Windows 10/11 PowerShell, run from the repo root.

### What you need

| What | For | Get it |
|---|---|---|
| .NET SDK 10 | the server | `winget install Microsoft.DotNet.SDK.10` |
| Visual Studio 2022 or Build Tools, "Desktop development with C++" + Windows 11 SDK | building `steam_api64.dll` | `winget install Microsoft.VisualStudio.2022.BuildTools`, then add the workload |
| `runtime-data\adjustmentdata.txt` | matchmaking | **not in git** (captured game data): get it from a teammate, see `runtime-data\README.md` |
| The game, installed from Steam | playing | |
| Tailscale | only for playing with someone on another PC | https://tailscale.com/download |

### 1. Build and test

```powershell
dotnet test BreakerServer/ProtocolTests/ProtocolTests.csproj        # 81 tests should pass
dotnet build BreakerServer/BreakerServer/BreakerServer.csproj

# The DLL: open "Developer PowerShell for VS 2022" in the repo root
msbuild steam_api64\steam_api64.vcxproj /p:Configuration=Release /p:Platform=x64 /m
#   -> Bin\Release\steam_api64.dll
```

### 2. Put the DLL in the game (once per PC)

1. Close the game. Open the game's `bin` folder
   (e.g. `...\steamapps\common\DRAGON BALL THE BREAKERS\bin`).
2. Rename the original `steam_api64.dll` to `steam_api64.dll.steam-original.bak` and copy
   `Bin\Release\steam_api64.dll` in its place. To play normally again, swap them back.
3. Copy `steam_config.ini.example` to `bin\BreakersRevived\steam_config.ini`.
   **Every player needs a different `steamid`** in it. The defaults keep everything on this PC
   (`127.0.0.1`), with LAN broadcast off.

### 3. Start the server

```powershell
powershell -ExecutionPolicy Bypass -File scripts\start-breaker-server.ps1        # -Capture, -StunDebug, -CustomNews
```

Wait for `Now listening on: http://127.0.0.1:5000`. Keep the window open; Ctrl+C stops the server.
On its first run the server creates `breaker.db` and `account-token-secret.key` (both gitignored, never
commit them). Don't delete the key: it keeps each account's userId, and without it the game throws away
local saves.

### 4. Play on this PC

Start the game from Steam. It logs in to your server and reaches the lobby; practice against the Raider
bot works. Logs: the server window, and `bin\BreakersRevived\Logs\BreakersRevived.log` in the game folder.

### 5. Play with someone over Tailscale

What works today: both players connect to one server, log in, reach the lobby and play practice. Matching
**two real players into one match is not done yet**: that is milestone M2 in
`docs/tailscale-matchmaking-plan.md`, which also has the full checklist.

1. Both PCs join the same tailnet. Each player runs `tailscale ip -4` and shares the `100.x` address.
2. Firewall, recommended, in an elevated (Run as Administrator) PowerShell. Host:
   `powershell -ExecutionPolicy Bypass -File scripts\set-tailscale-firewall.ps1 -Server -GameExe "<game folder>\bin\<game exe>" -BlockGameOutbound`.
   The other player runs the same without `-Server`. It allows the server and game ports only from Tailscale
   addresses, and blocks the game's traffic to anything outside the tailnet. `-Remove` deletes the rules.
3. Host starts the server on its Tailscale address. It still listens on `127.0.0.1`, and never on `0.0.0.0`:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\start-breaker-server.ps1 -TailscaleIP <host 100.x>
   ```
4. Each player edits `bin\BreakersRevived\steam_config.ini` (keep your own, different `steamid`):
   ```ini
   backend_host=<host 100.x>                ; the host uses its own 100.x too
   bind_address=<my 100.x>
   discovery_targets=<the other player's 100.x>
   lan_broadcast=0
   allowed_networks=127.0.0.0/8,100.64.0.0/10
   ```
5. Check before playing: `Get-NetUDPEndpoint -LocalPort 47584,7099,7100,3478,3479` lists only `100.x` and
   `127.0.0.1` addresses, never `0.0.0.0`.

### Diarkis session keys (optional)

Diarkis session keys need no setup: the server issues a fresh set of four
16-byte keys for every session (battle/get_diarkis_matching_server_info and
battle/get_connection_server_info), as the official server did. For debugging you
can pin one fixed set by setting all four as 32-character hex strings (environment
variables or .NET user-secrets, never `appsettings.json`):

```powershell
# optional, debugging only
$env:Breaker__DiarkisIvKey = "<32 hexadecimal characters>"
$env:Breaker__DiarkisAesKey = "<32 hexadecimal characters>"
$env:Breaker__DiarkisSidKey = "<32 hexadecimal characters>"
$env:Breaker__DiarkisHashKey = "<32 hexadecimal characters>"
```

```powershell
dotnet restore BreakerServer/ProtocolTests/ProtocolTests.csproj
dotnet build BreakerServer/BreakerServer/BreakerServer.csproj
dotnet run --project BreakerServer/BreakerServer/BreakerServer.csproj
```

Open `BreakersRevived.sln` in Visual Studio to work on the full solution. The
native `steam_api64` project requires the Visual Studio C++ workload.

Use this authentication and protocol implementation only for local/private
emulation. It does not validate tickets with Steam.

## Development

- `dotnet build BreakerServer/BreakerServer/BreakerServer.csproj` — build the managed server.
- `dotnet run --project BreakerServer/BreakerServer/BreakerServer.csproj` — start the local server.
- `dotnet test BreakerServer/ProtocolTests/ProtocolTests.csproj` — run protocol, registry, and HTTP integration tests.
- Server systems are separated into dedicated C# components under
  `BreakerServer/`, including authentication, configuration, endpoints,
  networking, protocol handling, services, STUN, and data handling.

## Known issues / deliberate omissions

- The C# rewrite is still under active development and does not currently
  complete the full game-entry flow.
- The client can currently progress through the server connection process and
  reach the tutorial flow.
- Match/session behavior is still being implemented. Each player currently gets their own match room,
  so two real players are never put in the same match (milestone M2 in `docs/tailscale-matchmaking-plan.md`).
- `docs/running-the-server.txt` describes the original dev PC (its `D:\` paths); on other PCs use the
  steps above.
- Some responses and game data are still placeholders while the remaining
  protocol behavior is researched and implemented.
- Canned leaderboard entries are synthetic placeholders.

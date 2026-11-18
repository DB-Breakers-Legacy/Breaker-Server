# Plan: two-player matchmaking over Tailscale, safely

Written 2026-11-17. Goal: a demo match between two players on a private Tailscale network, with nothing
listening on or broadcasting to the LAN or the internet, and tests that are repeatable.

## Where things stand

| Piece | State |
|---|---|
| Solo play (menu, lobby, practice, casual queue) | Works against the local server. Casual matchmaking fills the room with 7 bots after `BREAKER_AUTOFILL_SECONDS` (default 1 s). |
| **Two real players in one match** | **Never run.** `DiarkisUdpService` gives every session its own room (`PracticeRoomId` per session). Role list and connection-room member list hold only that one player. |
| Multi-player server logic | Exists on `origin/matchmaking/restoration-main` (`MatchmakingCoordinator.cs`, `ConnectionRoomCoordinator.cs`: shared rooms, member/mesh commands 103/127, relay of cmds 19/24/104 with membership checks). Synthetic tests only, never run with the real game. **That branch shares no history with `fix/diarkis-udp-transport`** (no merge base), so it has to be ported by hand, not merged. |
| In-match transport | From the captures (research doc on that branch): Diarkis connection room, P2P mesh via cmd 127, relay through the server as fallback. The DLL's Steam P2P emulation (`SteamOfflineServer`, UDP 47584) carries Steam-lobby traffic. `ISteamNetworkingSockets` / `ISteamNetworkingMessages` are stubs; if the game turns out to need them, that is extra work (M3 will show it). |
| Network scope | Fixed in this change (M0, below). Before it, the DLL listened on every interface, broadcast to the LAN and to 26.255.255.255 (public Radmin range), and sent **every P2P game packet** to all of those. |

## Topology for the demo

```
 Player A (host PC)                                Player B
 ┌──────────────────────────────┐   Tailscale     ┌──────────────────────────────┐
 │ BreakerServer  (-Mode Host,  │◄── 100.x ──────►│ game + steam_api64.dll       │
 │   -TailscaleIP 100.A)        │   (WireGuard)   │   backend_host = 100.A       │
 │   HTTP 5000, UDP 7099/7100,  │                 │   bind_address = 100.B       │
 │   STUN 3478/3479 on 100.A    │                 │   discovery_targets = 100.A  │
 │ game + steam_api64.dll       │◄───────────────►│   UDP 47584 on 100.B         │
 │   backend_host = 100.A       │   lobby / P2P   └──────────────────────────────┘
 │   bind_address = 100.A       │
 │   discovery_targets = 100.B  │
 └──────────────────────────────┘
```

Nothing binds to `0.0.0.0`. Both games use the host's Tailscale IP for the backend, the host included,
because the server advertises one address for its UDP services.

## M0: network scope fixes (done in this change, built and tested in the offline pipeline)

DLL (`steam_api64`):

| Before | Now | Setting (`BreakersRevived\steam_config.ini`) |
|---|---|---|
| UDP 47584 and the P2P socket bound `INADDR_ANY` (whole LAN) | Bound to one address; if it isn't up, the socket stays closed (fails closed) | `bind_address=127.0.0.1` (Tailscale: your `tailscale ip -4`) |
| Lobby adverts every 2 s to 255.255.255.255, custom_server, **26.255.255.255**, 127.0.0.1 (F-01) | Only to `custom_server` + `discovery_targets`; LAN broadcast opt-in; no hard-coded destination | `discovery_targets=127.0.0.1` (Tailscale: the other players' IPs), `lan_broadcast=0` |
| Every P2P game packet also broadcast to all of the above | Unicast to the peer once its endpoint is known; discovery targets only for first contact or a peer silent for 10 s | n/a |
| Outbound allow-list hard-coded, incl. public 25/8 and 26/8 and LAN broadcast (F-04) | From config; loopback, `custom_server`, `backend_host` always allowed; broadcast only with `lan_broadcast=1` | `allowed_networks=` (Tailscale only: `127.0.0.0/8,100.64.0.0/10`) |
| Any allowed-looking name (`foo`, `*.local`, `*.lan`) made its resolved IPs allowed, public ones too | Only `backend_host` / `custom_server` names (e.g. a MagicDNS name) do | n/a |
| Inbound lobby/P2P packets accepted from anyone (F-06) | Dropped unless the sender is in `allowed_networks` | same |
| Peer, lobby and P2P receive queues unbounded (F-06) | Capped (256 peers, 256 lobbies, 4096 queued packets; oldest dropped) | n/a |
| Lobby adverts carried the first LAN IP from `gethostname` | `advertise_host`, else the bound address | `advertise_host=` |

Older ini files get the new keys added with the safe defaults the first time the new DLL starts.

Server (`BreakerServer`):

* `Breaker:BindAddress` (default `127.0.0.1`): Diarkis matching/session and STUN listen only there
  (before: `IPAddress.Any`). Validated at start-up. The container image sets `0.0.0.0` *inside* the
  container; `docker run -p <ip>:...` decides what the host exposes.
* Test: `NetworkScopeTests` (default is loopback, bad values fall back to loopback, listener binds only the
  configured address).

Tooling (`breakers-sandbox`):

* `run\Start-BreakerServer.ps1 -TailscaleIP 100.x.y.z` checks that the address is in 100.64.0.0/10 and
  assigned to this PC. It then sets bind and advertised hosts, and publishes container ports on that IP
  instead of 127.0.0.1.
* `run\Set-TailscaleFirewall.ps1` (admin) adds firewall rules as a second boundary that doesn't depend on
  the mod code:
  - server ports allowed only from the tailnet;
  - the game's inbound UDP allowed only from the tailnet;
  - optionally, the game's outbound IPv4 blocked except to the tailnet (`-BlockGameOutbound`, the external
    check F-03 asks for).

## M1: two game instances on one PC, loopback only (no Tailscale yet)

Shows that two clients can see each other with nothing leaving the PC.

1. Build from the reviewed commit (`Build-Breakers.ps1 -AcceptMsvcLicense -Verify`) and deploy the DLL.
2. ini: default network scope (`bind_address=127.0.0.1`), `auto_instance_steamid=1` (instance 2 gets
   steamid + slot).
3. `Start-BreakerServer.ps1 -Mode Host -Capture`, then start the game twice.
4. Wireshark on the Wi-Fi/Ethernet and Tailscale adapters: expect **zero** packets to 47584/7099/7100/3478.
   Npcap's loopback adapter shows the actual traffic.
5. Check: the Steam-lobby list shows the other instance, and a room-code join works.

Exit: lobby discovery and join work on loopback, and nothing appears on other adapters.

## M2: real-player matchmaking in the server (main work item)

Port the multi-player logic from `origin/matchmaking/restoration-main` into the live-tested
`DiarkisUdpService` on this branch. Keep this branch's message IDs and the fixes made against the real
client (12xxx casual, 22xxx/42xxx connection rooms, paged member lists, `able_rematch`, autofill).

1. **Shared casual queue.** Sessions that send the casual role request join one queue. With
   `Breaker:RequiredPlayers` (test-only, default 8; set to 2 for the demo) real players are put in one
   room. Free slots are filled with bots as today, after `BREAKER_AUTOFILL_SECONDS`.
2. **Room state shared across sessions:** one room id; member list with every real player's own
   `userData`/`sdpData` plus bots. One player per role (one Raider, rest Survivors, honouring
   `desiredRole` where possible). 12105 match-found and 12119 connection room sent to every member.
3. **Connection room:** both players attach to the same room. 42100/22100 all-joined lists both.
   Port `ConnectionRoomCoordinator`'s member-ID (103), mesh (127), P2P-complete (301) and relay
   (19/24/104), with its rule that only room members can send to or read the room.
4. **Bot harness first** (`ProtocolTests`): two synthetic UDP clients, each with its own SID, keys and
   socket, go through SYN, init, queue, match found, connection room, relay and leave on loopback.
   Then add duplicate, slow and leaving-client cases. This runs in the offline Docker build with no game
   needed.
5. Bounded state: sessions, rooms, fragments and unacked messages expire. The existing 30-minute idle
   timeout stays.

Exit: the harness passes in `Build-Breakers.ps1 -Verify` (reproducible, offline).

## M3: two game instances on one PC against the M2 server (loopback)

Same setup as M1, with both instances queued for casual. Capture the server log (`-Capture`) and loopback
traffic. One evidenced incompatibility at a time: fix it, add a regression test, note it in the research
log. This step answers whether the match itself needs Diarkis mesh, relay, or Steam networking sockets.

Exit: both instances reach the same match.

## M4: the Tailscale demo

Do this only after M3. Each player follows the steps on their own PC.

1. **Tailscale.** Both PCs are on one tailnet. Either the second player joins as a user, or each of you
   shares your machine with the other: both directions are needed for P2P. Restrict the tailnet with an
   ACL so only these two devices reach the game ports:
   ```jsonc
   // Tailscale admin console -> Access controls (example; replace the users)
   {
     "groups": { "group:breakers": ["playerA@example.com", "playerB@example.com"] },
     "tagOwners": { "tag:breakers": ["playerA@example.com"] },
     "acls": [
       // server ports on the host
       { "action": "accept", "src": ["group:breakers"],
         "dst": ["tag:breakers:5000", "tag:breakers:7099-7100", "tag:breakers:3478-3479"] },
       // game-to-game UDP (lobby 47584 + the game's own P2P ports)
       { "action": "accept", "src": ["group:breakers"], "dst": ["group:breakers:*"] }
     ]
   }
   ```
   Tag the host PC `tag:breakers`. Tailscale ACLs can't restrict by protocol per port in this short form;
   the firewall script adds the UDP/TCP split.
2. **Firewall (admin).** Host: `Set-TailscaleFirewall.ps1 -Server -GameExe <exe> -BlockGameOutbound`.
   Player B: `-GameExe <exe> -BlockGameOutbound`.
3. **Server (host).** `Start-BreakerServer.ps1 -Mode Host -TailscaleIP <host 100.x> -Capture`. Host mode is
   used because in a container the server sees remote players as the Docker gateway. `RequiredPlayers=2`
   via `$env:Breaker__RequiredPlayers` once M2 exists.
4. **ini per player:** *different `steamid` for each player*, then:
   ```ini
   backend_host=<host 100.x>
   bind_address=<my 100.x>
   discovery_targets=<the other player's 100.x>
   lan_broadcast=0
   allowed_networks=127.0.0.0/8,100.64.0.0/10
   ```
5. **Checks before queueing:**
   - the DLL log shows `network scope: bind_address=100.x ... lan_broadcast=0`;
   - the server log shows `listening on 100.x:7100`;
   - `Get-NetUDPEndpoint -LocalPort 47584,7099,7100,3478,3479` lists only 100.x / 127.0.0.1 addresses,
     never `0.0.0.0`.
6. **Play and capture.** Run Wireshark on the physical adapter during the match. Only Tailscale's
   encrypted WireGuard UDP should appear there: no 47584, 7099/7100, 3478/3479, and no game-server
   hostnames.

Stop and investigate if any port shows `0.0.0.0`, any `BLOCK public` line names an unexpected host, or
the physical adapter shows game ports.

## M5: hardening before inviting anyone beyond the two of you

* **F-07** Server "auth" accepts unkeyed FNV tickets, so anyone who can reach port 5000 can act as any
  SteamID. Fix: a per-player key issued by the host (or an allow-list of SteamIDs) checked with HMAC; also
  set `AllowedHosts`. Note the DLL sends the original public host name in the Host header, so list those.
* **Lobby/P2P packets are unauthenticated** (sender SteamID is taken from the packet). Add an HMAC with a
  per-session secret handed out by the server.
* **F-02** Network hooks are installed last in start-up. Move `NetworkHooks::Init()` first.
* **F-05** `OnlineCheckBypass` patches by RVA with no check of the game build. Verify the exe hash first.
* `SO_REUSEADDR` on 47584 lets another local process share the port; use `SO_EXCLUSIVEADDRUSE` once
  multi-instance behaviour (M1) is confirmed.
* Unclassified UDP packets are logged at Information with hex previews: move to Debug and rate-limit.

## Future milestone: fetch dependencies at build time instead of committing them

The pinned Docker build (`breakers-sandbox\build\*\Dockerfile`, the equivalent of a Makefile here) already
fetches and hash-checks its toolchain. Every fetch happens in a network-enabled stage pinned by hash or
commit; compiling and testing then run with `--network=none`. Still open:

| Dependency | Now | Target |
|---|---|---|
| MinHook | `steam_api64/libMinHook.x64.lib` (binary blob) + `MinHook.h` | Fetch TsudaKageyu/minhook **v1.3.4 @ c3fcafdc10146beb5919319d0683e44e3c30d537** and build from source (the Docker build already does). For Visual Studio, use a vcpkg manifest (`minhook`) or a pinned fetch script. Then delete the `.lib`. |
| Dear ImGui | `steam_api64/imgui*` vendored, **not compiled** | Delete with the dead overlay code, or pin a release tag and fetch it if the overlay comes back. |
| Steamworks SDK headers | `steam_api64/sdk_includes/*.h` (many interface versions) | Valve's SDK needs a Steamworks login and its licence limits redistribution, so a build can't fetch it anonymously. **Needs a decision:** keep only the headers actually used and record the SDK version, or read them from a local `STEAMWORKS_SDK_DIR` checked against a hash. |
| `sdk_includes/sdk_includes.rar` | unused binary | delete |
| MSVC CRT + Windows SDK | Visual Studio install (VS build); xwin download pinned by hash (Docker build) | keep; document the VS version used for the MSVC build |
| LLVM/clang 20, Ubuntu packages | apt snapshot `20261115T000000Z` (Docker build) | keep |
| NuGet packages | lock files live in the tooling repo (`build/server/locks`) | move `packages.lock.json` into this repo and set `RestorePackagesWithLockFile` in `Directory.Build.props` |
| .NET SDK / ASP.NET runtime images | pinned by digest (Docker build) | keep; add `global.json` with SDK 10.0.404 for local builds |
| Wireshark `tshark` (scripts) | hard-coded `C:\Program Files\Wireshark\tshark.exe` | document the version; take the path from a parameter |
| Build output in git history | `Bin/`, `steam_api64/Build/` (F-20) | remove from the index now (`.gitignore` proposal); rewrite history before any public push |

# New matchmaking research and restoration plan

Last updated: 2026-11-08

## Purpose

Track the remaining reverse engineering, server implementation, and verification
needed to make Dragon Ball: The Breakers matchmaking work end to end. This is the
active findings log; older reviews remain background evidence rather than proof
of the current implementation state.

The immediate goal is an eight-player match on private infrastructure: queue,
room assembly, role assignment, connection-room handoff, P2P setup, match
reporting, and requeue. Transport compatibility and CSMS framing must be restored
before queue logic.

## Implementation baseline

All code work must be based on `main` containing
[PR #5, cleanup for match making](https://github.com/DB-Breakers-Legacy/Breaker-Server/pull/5):

- Source branch: `cleanup/matchmaking-prep`
- Reviewed PR head: `22f49e8e35d2774457266d23fa8b7d542aaafb21`
- Merged: 2026-11-09 01:26:08 UTC
- Squash-merge commit on `main`: `f6ee3068bb1930325cfad5655b5ece8425bc46aa`
- Implementation branch: `matchmaking/restoration-main`

The reviewed PR head and squash-merge commit have identical tree SHA
`d2d3e1b203de44d4c57776d06c3b18b6617c0793`, so the baseline verification applies
to the merged code without drift. Recheck and record `origin/main` before later
code milestones. Do not recreate the deleted `DiarkisSessionUdpService`; PR #5
combines both listener roles in `DiarkisUdpService` and registers matching and
session listeners separately in `Program.cs`.

PR #5 establishes only the cleanup baseline:

- one UDP service with matching and session listener roles;
- per-client session-key resolution on the session listener;
- magic-based cleartext directory replies and focused directory/session tests;
- test logging configuration that works without event-log privileges.

It does not restore captured packet types, crypto, reliability, matching-handout
keys, CSMS, fragmentation in the live command path, or matchmaking state.

Related documents:

- [Header research](header-research.md)
- [Earlier matchmaking reconstruction](matchmaking-reconstruction.md)
- [Earlier restoration milestones](matchmaking-restoration-plan.md)
- [Managed rewrite review](csharp-rewrite-review.md)
- [Native networking review](new-review-netcode.md)
- [Captured Diarkis structure](../../docs/DBTB-API-Documentation/md/Structure/Diarkis.md)
- [Captured matchmaking flow](../../docs/DBTB-API-Documentation/md/API/Calls/Matchmaking_Flow.md)

## Evidence and safety rules

- Use decrypted captures first, executable RTTI/code second, public Diarkis
  documentation third, and exploratory notes last.
- Record the executable hash and build for every native address. Addresses here
  are preferred-image virtual addresses with image base `0x140000000`.
- The generated headers under `../../headers-dbreakers/generated` are RTTI
  metadata, not C++ declarations or recovered object layouts.
- A vtable address is not a function or hook target. Resolve and review each
  entry before assigning it a behavioral name.
- Label findings as confirmed, candidate/inferred, or unknown. Do not silently
  combine schemas or addresses from different builds.
- Start Ghidra work read-only. Define missing functions or data narrowly when
  required, and review decompilation before renaming or changing prototypes.
- Keep executables, Ghidra projects, packet captures, TLS keys, credentials, and
  private decoder output outside Git.
- Use synthetic loopback clients first. A native-client test requires the
  isolated guest, an external firewall destination policy, and packet capture;
  the native hooks alone are not a sufficient boundary.
- Do not substitute public Diarkis ticket commands 218-229 for the game's custom
  CSMS protocol. Command 219 is separately observed as the queue keepalive.

## Executable baseline

The active Ghidra program used for the initial pass is:

```text
Name: DRAGON BALL THE BREAKERS.exe
SHA-256: 048796521daae21f12bbeb39c02fb4148049139aaf6141854921255064f7370c
Format: Portable Executable (PE)
Language: x86:LE:64:default
Compiler: windows
Image base: 0x140000000
Ghidra: 12.1.4 with GhidraMCP 2.0
```

The RTTI descriptors for `ProtocolRequestCasualRoomCreate`,
`ProtocolResponseCasualRoomCreate`, and
`ProtocolNotifyCasualRoomOnMatchingComplete` exactly match the generated
headers. Their vtable addresses and boundaries also match, so the shared header
map is applicable to this executable. Continue validating each additional
address as it is used.

## Confirmed native findings

### Vtable resolution

Request vtables reviewed so far contain eight function entries followed by RTTI
data for the next vtable. Response/notification vtables reviewed so far contain
four function entries. Values such as `0x14143B690`, `0x14140A030`, and
`0x14140B630` are RTTI data, not virtual-function entries.

| Class | Vtable | Entry | Function | Status |
| --- | ---: | ---: | ---: | --- |
| `ProtocolRequestCasualRoomCreate` | `0x1411FF448` | 1 | `0x14078D4F0` | Confirmed JSON serializer |
| `ProtocolRequestCasualRoomJoin` | `0x1411FF490` | 1 | `0x14078E020` | Confirmed JSON serializer |
| `ProtocolRequestCasualRoomOpen` | `0x1411FF640` | 1 | `0x14078CE70` | Serializer candidate; decompile next |
| `ProtocolRequestCasualRoomMemberDataSet` | `0x1411FF568` | 1 | `0x14078F1E0` | Serializer candidate; decompile next |
| `ProtocolRequestCasualUpdateMatchedSdp` | `0x1411FF7F0` | 1 | `0x140790CE0` | Serializer candidate; decompile next |
| `ProtocolRequestCasualCreateConnectionRoom` | `0x1411FF6D0` | 1 | `0x14078B6C0` | Shared candidate; decompile next |
| `ProtocolRequestCasualJoinConnectionRoom` | `0x1411FF718` | 1 | `0x14078B6C0` | Shared candidate; decompile next |
| `ProtocolRequestCasualReportBattleFinish` | `0x1411FF760` | 1 | `0x14078C5B0` | Serializer candidate; decompile next |
| `ProtocolResponseCasualRoomCreate` | `0x1411D4A30` | 3 | `0x14055AD80` | Deleting destructor, not a parser |
| `ProtocolNotifyCasualRoomOnMatchingComplete` | `0x1411D5110` | 3 | `0x14055AC40` | Deleting destructor, not a parser |

A candidate's position in a vtable is not sufficient evidence for its behavior.
Confirm field operations, callers, and constants in the function itself. A
throwaway Ghidra project subsequently resolved the response metadata without
changing the active analysis project: entries 1/2 of the room-create response
vtable return `13000`, and entries 1/2 of the matching-complete notification
vtable return `12105`. The entry-3 parser-candidate labels were therefore wrong;
constructor/callback analysis remains necessary for field-level parsing.

### Room-create request, sub-ID 12000

The serializer at `0x14078D4F0` emits these JSON keys in this order:

```text
gameRule
userName
userRank
userRivalRank
appVersion
blockList
sdpData
natType
userIp
platformUserId
diarkisNatType
```

This independently supports the captured 12000 key set. Captures establish
`blockList` as an array and provide the nested `sdpData` shape. JSON member order
is recorded as static evidence but is not assumed to be semantically required
without a corresponding wire requirement.

### Room-join request, sub-ID 12001

The serializer at `0x14078E020` emits:

```text
roomId
roomKey
userName
userRank
userRivalRank
blockList
sdpData
natType
appVersion
userIp
resultList
platformUserId
isInvited
diarkisNatType
```

The key set otherwise agrees with the captured join request. One disagreement
must remain open: the executable string is **`isInvited`**, while the sanitized
capture table currently says **`isInvite`**. Check the original capture and build
before implementing this field; do not silently choose either spelling.

### CSMS request serialization

The client CSMS writer at `0x1405297B0` emits a five-entry MessagePack map in
this order: `title_cd`, `platform`, `user_id`, `id`, `data`. The title, user ID,
and data values are strings, while platform is numeric. In particular, `user_id`
is a string matching the 18-digit header UID, not an integer.

The managed codec also preserves the captured integer encodings: request IDs use
`CD` for the observed 16-bit values, and response/push IDs use `D2`. MessagePack-C#
would encode an unsigned 32-bit value as `CE`, so using `uint` directly is not
wire-compatible with the captured response format.

### Still unknown from native code

- response and notification field reads, validation, and callback targets;
- constructors' command/sub-ID metadata and the remaining request serializers;
- matched-SDP semantics and connection-room state transitions;
- orchestration order enforced by `CasualMatchingModule`,
  `MatchingServerManager`, and `MatchingSubModule*`;
- whether the `isInvited`/`isInvite` disagreement is a note error or build skew;
- live-client interoperability of the managed server.

## PR #5 baseline server gaps

The PR #5 head and diff established the following required work. These are the
baseline findings, not claims that every item remains open; the milestone results
below record what has subsequently been completed:

1. Restore `/dbtb-prd/{prefix}/{title}/api` compatibility while retaining current
   development routes, and add the captured queue-time HTTP endpoints.
2. Issue fresh matching-session SID/AES/IV/HMAC sets through
   `SessionKeyRegistry`; the matching handout still uses global configured keys.
3. Replace the old packet-state values with captured SYN 2, reliable DAT 3,
   ACK 4, retransmission 5, and FIN 7 behavior.
4. Restore zero-padded AES-CBC envelopes, HMAC-verified `plen=0` and
   under-reported-length handling, cleartext SID prefixes, and correct command
   payload framing without the extra clear-length field.
5. Restore init/heartbeat replies, NAT exchange, public-address reporting,
   unacked-DAT tracking, type-5 retransmission, batched ACKs, and FIN ACKs.
6. Connect per-peer fragment reassembly and outbound fragmentation to live UDP
   command processing.
7. Implement CSMS and all matchmaking/connection-room state rather than adding
   more hardcoded command replies.
8. Replace self-consistent test vectors with sanitized golden wire fixtures and
   multi-client interoperability tests.

The merged service's command-send path still accepts only a byte sequence even
though the datagram header supports 24 bits; remove that truncation during the
transport milestone.

## Implementation plan

### Milestone 0 - Establish the PR #5 baseline

Code/research tasks:

- create the implementation branch from the current PR #5 head;
- run restore, Release build, tests, and formatting on the untouched baseline;
- record the results and the final PR head SHA here;
- confirm the executable hash above before further native analysis;
- back up the Ghidra project before defining missing functions or vtable data.

Done when the clean PR #5 baseline builds and passes its existing suite, and any
pre-existing failure is recorded rather than attributed to new work.

### Milestone 1 - Restore captured HTTP and transport behavior (complete for synthetic loopback transport)

Implement the HTTP routes, matching key issuance, packet types, crypto,
bootstrap/heartbeat, NAT behavior, reliability, FIN acknowledgement, redirect
key carryover, and live fragmentation listed above.

Verification:

- golden codec vectors cover zero padding, `plen=0`, under-reported lengths,
  SID prefixes, tampering, truncation, and conflicting fragments;
- two independent synthetic clients complete bootstrap and heartbeat with
  different keys and sequence spaces;
- retransmission stops after batched ACKs, FIN receives the correct ACK, and a
  redirect probe preserves the SID/key set;
- captured-route HTTP replay reaches the expected handlers;
- no production command handler relies on packet length as a substitute for
  magic/type parsing.

### Milestone 2 - Implement and verify CSMS framing (complete for framing and synthetic fragmentation)

Implement command 12000 and 22000 client requests, server responses, and server
pushes. Include MessagePack envelopes, double-encoded JSON `data`, nested JSON
strings, UID/platform headers, exact lengths, and request-plus-1000 response
sub-ID mapping.

Verification:

- sanitized golden CSMS vectors round-trip byte-exactly;
- the fragmented 1502-byte ticket create and multi-fragment final roster pass in
  both directions;
- malformed framing, invalid lengths, and conflicting fragments fail safely
  without dropping the listener;
- the join spelling disagreement is resolved before that field is encoded.

### Milestone 3 - Implement the matchmaking room coordinator (complete for synthetic match assembly)

Implement 12000 create, 12001 invite/rematch join, 12002 keepalive, 12005 member
update, 12012 search preferences, and 12017 completion. Maintain shared room,
battle-room, connection-room, member-profile, and queue state.

Emit 13000-series responses and 12101, 12102, 12105, 12118, 12119, and 12120
pushes in captured order. Assign one Raider and seven Survivors, handle queue
keepalive command 219, and implement cancellation, disconnect, timeout, and
requeue cleanup.

Verification:

- eight synthetic clients reach a complete battle room;
- every member receives consistent role and final-roster data, including
  fragmented pushes;
- duplicate joins and stale-member cleanup pass;
- `get_connection_server_info` returns the assembled battle's binding rather
  than unrelated global state.

### Milestone 4 - Implement connection handoff and P2P (complete for synthetic connection rooms and relay)

Support redirect and no-redirect variants, 22000-series attach/report flows,
23000-series responses, 22100-series pushes, room commands 11, 14, 101, 102,
103, and 115, cmd-127 mesh setup, cmd-301 completion reports, and the captured
reliable/unreliable relay commands.

Verification:

- all eight clients attach to one connection room;
- redirect and no-redirect socket tests preserve the correct SID/key set;
- member leave/dissolution and missing-room behavior match golden evidence;
- reliable relay retransmits, unreliable relay does not, and non-members cannot
  read or inject room traffic;
- a soak run does not leave unbounded session, fragment, or retransmission state.

### Milestone 5 - Correlate HTTP lifecycle and validate the real client

Add `adjustment_data_manage/get_version` and
`season/get_selectable_stage`, then correlate battle start, results, member
results, priority consumption, and requeue with shared battle state.

Include the remaining targeted native research instead of treating the synthetic
implementation as proof of client compatibility:

- resolve room-create response parsing at `0x14055AD80` and matching-complete
  notification parsing at `0x14055AC40`;
- confirm remaining request serializers and command metadata where lifecycle
  behavior is uncertain;
- resolve the `isInvited`/`isInvite` disagreement against the original capture
  before relying on that field;
- use endpoint-specific response evidence for the two queue HTTP calls rather
  than inventing response shapes.

Replay the harvested HTTP sequence before running the patched client in the
isolated guest. Compare live HTTP/UDP order with golden evidence and record every
unexplained difference. No official or unintended external destination is
allowed. Do not run the native client without an externally enforced firewall
policy and capture environment.

Done when login, queue, match setup, gameplay transport, results, and requeue
complete using only private infrastructure, the targeted native questions are
resolved or recorded with explicit uncertainty, and all synthetic and managed
suites pass. Synthetic replay alone does not complete the real-client milestone.

## Immediate native research queue

1. Define and decompile `0x14055AD80`; recover room-create response field reads,
   validation, stored offsets, and callbacks with address citations.
2. Define and decompile `0x14055AC40`; do the same for matching-complete
   notification parsing and dispatch.
3. Decompile the confirmed vtable-entry-1 candidates for room open, member-data
   update, matched SDP, connection-room create/join, and battle finish.
4. Inspect constructors and callers to recover command/sub-ID constants and
   object initialization; do not infer constants solely from class names.
5. Resolve the response/notification vtables for matched SDP, room data changes,
   connection-room creation, all-players-joined, and member updates.
6. Trace `CasualMatchingModule`, `MatchingServerManager`, and the relevant
   `MatchingSubModule*` classes to confirm state transitions and call order.
7. Resolve `isInvited` versus `isInvite` against the original capture and record
   the capture ID/frame and executable build.

Each completed item must update the confirmed findings or open-questions section
and identify any server fixture that can now be written.

## Verification commands

Run on the merged-main implementation branch for every code milestone:

```powershell
dotnet restore BreakerServer/ProtocolTests/ProtocolTests.csproj
dotnet build BreakerServer/BreakerServer/BreakerServer.csproj --configuration Release --no-restore
dotnet test BreakerServer/ProtocolTests/ProtocolTests.csproj --configuration Release --no-restore
dotnet format BreakerServer/BreakerServer/BreakerServer.csproj --verify-no-changes --no-restore
```

A passing suite proves only what its fixtures cover. Do not mark a wire milestone
done without the corresponding golden or multi-client check, and do not claim
live interoperability without the isolated real-client run.

### Milestone 0 result - merged PR #5 baseline

Verified on `matchmaking/restoration-main` at
`f6ee3068bb1930325cfad5655b5ece8425bc46aa` using the repository-local,
ignored `.sandbox-dotnet` SDK version 10.0.403:

- Restore: passed for `ProtocolTests` and its server dependency.
- Release build: passed with zero warnings and zero errors.
- Tests: all 39 passed; none failed or skipped.
- Format: reported pre-existing import-order findings in
  `Network/DiarkisUdpService.cs`, `Network/StunUdpService.cs`,
  `Services/ResponseCatalog.cs`, `Services/SessionKeyRegistry.cs`, and
  `Services/StarterDataStore.cs`. These files were left unchanged during the
  baseline task; formatting is not being reported as passing.

Milestone 0 is complete because the clean baseline builds and passes its tests
and the formatting result has been recorded explicitly.

### Milestone 1 result - captured transport restored for synthetic clients

Implemented on `matchmaking/restoration-main` using the captured protocol
structure and the historical Python peer's tested behavior:

- Added `/dbtb-prd/{prefix}/{title}/api` alongside the existing development
  route for game and news handlers.
- Matching handouts now issue fresh registered keys in wire order
  `SID, AES, IV, HMAC`; both UDP listeners resolve sessions through the shared
  registry.
- Replaced the legacy packet states with type-1 unreliable, type-2 SYN, type-3
  reliable DAT, type-4 ACK, type-5 retransmission, and type-7 FIN handling.
- Restored zero-padded AES-CBC, HMAC-verified `plen=0` and under-reported-length
  acceptance, client SID validation, and command bodies without an extra clear
  length field.
- Added per-SID 24-bit server sequence spaces, bounded deduplication, unacked
  tracking, 500 ms retransmission, batched ACK handling, and FIN ACKs carrying
  the server's next sequence.
- Restored init/heartbeat replies with the server-observed address, NAT and
  endpoint exchanges, directory responses, and the session-probe redirect.
- Connected client fragment reassembly and server fragmentation to live command
  processing.

Verification:

- Release build passed with zero warnings and zero errors.
- All 53 tests passed, including an independently generated AES/HMAC golden
  vector, crypto length edge cases, captured bootstrap/heartbeat behavior, two
  independent clients, retransmission and batched ACKs, FIN, redirect key
  carryover, and fragmented client init.
- `dotnet format` on `ProtocolTests.csproj` passed. The server-only format check
  reports only the four remaining baseline import-order findings in
  `StunUdpService.cs`, `ResponseCatalog.cs`, `SessionKeyRegistry.cs`, and
  `StarterDataStore.cs`; no new formatting finding remains in the changed files.

This completes the synthetic Milestone 1 exit criteria. CSMS, queue/room state,
and the queue-time HTTP response shapes remain later milestones. No native game
or live-client interoperability test was run.

### Milestone 2 result - CSMS framing and synthetic fragmentation

Implemented `Protocol/Diarkis/CsmsCodec.cs` with one strict codec for client
requests, server responses, and server pushes:

- Handles the 54-byte header, reserved bytes, 18-digit string UID, binary
  response platform, exact MessagePack lengths, and push-length prefix.
- Serializes request maps in captured order with string `user_id`, numeric
  platform, JSON-string `data`, `CD` request IDs, and `D2` response/push IDs.
- Validates header/map ID agreement, map shape, JSON objects, lengths, and
  request-plus-1000 response sub-ID mapping.
- Parses commands 12000 and 22000 on the live decrypted UDP path. A valid CSMS
  connection-room request is required before the session-probe redirect; invalid
  payloads are rejected without dropping the listener.

Verification:

- Release build passed with zero warnings and zero errors.
- All 59 tests passed, including exact request/response layouts, malformed IDs
  and lengths, a 1502-byte ticket create split into two fragments, a final
  roster push split into three fragments, and a fragmented CSMS connection-room
  request driving the real redirect path.
- `dotnet format` on `ProtocolTests.csproj` passed. The server-only check still
  reports only the four unrelated baseline import-order findings.

This completes Milestone 2 for framing and synthetic fragmentation. It does not
implement room semantics, encode the unresolved join-invitation field, or claim
native-client interoperability. No live game test was run.

### Milestone 3 result - synthetic eight-player match assembly

Implemented `Services/MatchmakingCoordinator.cs` and connected it to the live
CSMS/UDP path and HTTP connection handout:

- Handles 12000 create, 12001 invite/rematch join, 12002 keepalive, 12005 member
  update, 12012 preferences, and 12017 completion with shared player/room state.
- Emits ordered 13000-series responses and 12101, 12102, 12105, 12118, 12119,
  and 12120 pushes. Invite joins share and update the existing room roster.
- Assembles eight completed players, assigns one Raider and seven Survivors,
  emits role assignment followed by 64 targeted final-roster variants and one
  shared connection-room binding per player.
- Emits command-219 `Timeout` queue keepalives and removes duplicate, expired,
  disconnected, and requeued players from stale room/queue state.
- `get_connection_server_info` now requires the authenticated user's assembled
  battle and returns its binding with a fresh UDP key set.

Verification:

- Release build passed with zero warnings and zero errors.
- All 64 tests passed. Eight synthetic coordinator clients reach a complete
  battle, every member has consistent role/final-roster data, a generated final
  roster survives fragmentation, and the authenticated HTTP handout succeeds
  only after assembly.
- Invite/rematch join, duplicate create, expiry, disconnect, requeue, live UDP
  create/push/response order, and queue keepalive are covered.
- `dotnet format` on `ProtocolTests.csproj` passed. The server-only check still
  reports only the four unrelated baseline import-order findings.

This completes Milestone 3 for synthetic match assembly. Connection-room
commands, P2P, and native-client interoperability remain unverified. No live game
test was run.

### Milestone 4 result - synthetic connection rooms and P2P relay

Implemented `Services/ConnectionRoomCoordinator.cs` and integrated it with the
session listener:

- Handles 22000/22001/22002/22004 attach, practice, report, and leave flows,
  23000-series responses, and 22100/22101/22104/22105 pushes.
- Implements room commands 11, 14, 101, 102, 103, and 115, including the 52-char
  Diarkis room ID, member-list duplicate-entry quirk, member-left notifications,
  and missing-room rejection.
- Builds cmd-127 mesh records from each member's public/LAN endpoints and counts
  cmd-301 P2P completion reports per member.
- Relays cmds 19, 24, and 104 with validated room membership and recipient IDs.
  Game payloads remain opaque; cmd-104 preserves its reliable flag, while profile
  relays use unreliable datagrams without retransmission.
- Retains the redirect/no-redirect and SID/key-carryover behavior verified in
  Milestone 1.

Verification:

- Release build passed with zero warnings and zero errors.
- All 66 tests passed. Eight members attach to one connection room, exchange
  member IDs and mesh data, complete P2P reports, relay to valid recipients, and
  dissolve with shrinking rosters and final missing-room rejection.
- Non-members cannot inject room traffic. Live UDP connection attach emits the
  expected CSMS response/push sequence through the encrypted transport.
- `dotnet format` on `ProtocolTests.csproj` passed. The server-only check still
  reports only the four unrelated baseline import-order findings.

This completes Milestone 4 for synthetic connection rooms and relay. It does not
prove native-client interoperability or a long multi-peer soak. No live game test
was run.

### Milestone 5 progress - lifecycle replay and targeted research

- Added `adjustment_data_manage/get_version` and
  `season/get_selectable_stage` on both API route shapes. Their current response
  shapes (`[0, 2]` and `[0, [1000]]`) remain provisional until capture evidence
  confirms them; the native response vtables alone did not establish the shapes.
- Battle start now creates per-user state, result/member-result/priority calls
  require that state, and battle result clears it so the user can requeue.
- Added a synthetic HTTP replay covering queue calls, battle start, related
  result APIs, completion, and requeue. All 67 managed tests pass with zero build
  warnings; the test-project format check passes and only the four baseline
  server import-order findings remain.
- Corrected the earlier parser candidates: entries 1/2 return `13000` and
  `12105`, while entries 3 are deleting destructors. Constructor/callback
  analysis is still needed for field-level parsing, and the invitation spelling
  disagreement remains open.

Native-client validation is **not complete**. No externally enforced firewall
and capture environment has been established for this checkout, so no patched
client was run. Milestone 5 remains open for live validation and confirmation of
the provisional queue response shapes.

## Findings log

### 2026-11-08 - New baseline and initial executable/header correlation

- Selected PR #5 head `22f49e8e35d2774457266d23fa8b7d542aaafb21` as the required
  implementation baseline and reviewed its merged UDP service and tests.
- Recorded target SHA-256
  `048796521daae21f12bbeb39c02fb4148049139aaf6141854921255064f7370c`.
- Confirmed exact RTTI descriptor and vtable agreement between the executable
  and shared generated headers for the initial room-create and
  matching-complete classes.
- Confirmed room-create serializer `0x14078D4F0` and room-join serializer
  `0x14078E020`, including their JSON key order.
- Resolved the next request serializer candidates and identified response and
  notification parser candidates `0x14055AD80` and `0x14055AC40`.
- Found the unresolved `isInvited`/`isInvite` schema disagreement.
- Confirmed that PR #5 is a cleanup baseline, not a transport or matchmaking
  compatibility implementation.
- No server code was changed, no managed test was run on the PR head in this
  checkout, and no live-client interoperability test was performed.

### 2026-11-08 - PR #5 merged and baseline verified

- PR #5 was squash-merged as `f6ee3068bb1930325cfad5655b5ece8425bc46aa`; its
  tree is identical to the reviewed PR head.
- Created `matchmaking/restoration-main` from the merged `main` commit.
- Installed .NET SDK 10.0.403 only in the ignored `.sandbox-dotnet` directory
  because the machine had no SDK on PATH.
- Restore and Release build passed with zero warnings; all 39 tests passed.
- Recorded five pre-existing import-order formatting findings without changing
  server code.

### 2026-11-08 - Milestone 1 synthetic transport completed

- Restored captured HTTP route shapes and fresh matching-session key handouts.
- Ported captured crypto, packet, bootstrap, reliability, redirect, FIN, and
  fragmentation behavior to the merged C# UDP service.
- Expanded the managed suite from 39 to 53 tests; all pass.
- Confirmed redirect key carryover with a real second listener and independent
  bootstrap/heartbeat state with two synthetic clients.
- Preserved the four unrelated baseline formatting findings and documented that
  no native live-client test has been performed.

### 2026-11-08 - Milestone 2 CSMS framing completed

- Confirmed from the executable that CSMS request `user_id` is a string and
  preserved captured `CD`/`D2` ID encodings rather than MessagePack-C#'s default
  unsigned `CE` encoding.
- Added strict request, response, and push codecs plus live CSMS validation.
- Added golden two-fragment ticket-create and three-fragment roster-push tests.
- Expanded the managed suite to 59 passing tests without changing room behavior
  or claiming live-client interoperability.

### 2026-11-08 - Milestone 3 synthetic match assembly completed

- Added the room coordinator, invite/rematch membership, queue lifecycle, role
  assignment, final rosters, and battle-specific connection binding.
- Eight synthetic players complete the battle-room flow with consistent roster
  data and one shared connection-room ID.
- Live UDP create/push/response and queue keepalive paths are covered, along
  with the authenticated connection handout.
- Expanded the managed suite to 64 passing tests; no native game test was run.

### 2026-11-08 - Milestone 4 synthetic connection and P2P completed

- Added connection-room attach/leave, member and mesh commands, P2P completion
  bookkeeping, and membership-checked opaque relay.
- Eight synthetic members complete the connection lifecycle with shrinking
  rosters and missing-room rejection.
- Live UDP connection attach and non-member injection rejection are covered.
- Expanded the managed suite to 66 passing tests; no native game or long soak
  test was run.

### 2026-11-08 - Milestone 5 synthetic lifecycle progress

- Added queue HTTP routes, shared battle lifecycle state, and requeue behavior.
- Corrected the response/notification vtable metadata and rejected the earlier
  destructor/parser assumption.
- Expanded the managed suite to 67 passing tests. Native live validation and the
  two provisional queue response shapes remain open.

## Next update checklist

- [x] Record the build/test/format result on merged PR #5; build and tests pass,
      with five baseline import-order findings documented.
- [ ] Recover room-create response and matching-complete notification parsing.
- [ ] Confirm the remaining request serializer candidates and command metadata.
- [ ] Resolve the join-field spelling disagreement with capture evidence.
- [x] Complete Milestone 1 golden transport tests.
- [x] Complete Milestone 2 CSMS and fragmentation tests.
- [x] Record the first isolated eight-client result without claiming success
      before the captured exit criteria pass.
- [x] Complete Milestone 4 synthetic connection-room and P2P tests.
- [x] Complete the synthetic HTTP lifecycle replay.
- [ ] Confirm the queue-version and selectable-stage response shapes with capture
      evidence.
- [ ] Run native-client validation with an external firewall and packet capture.

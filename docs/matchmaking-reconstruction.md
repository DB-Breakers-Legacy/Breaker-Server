# Matchmaking reconstruction (2026-10-16)

Client examined: `DRAGON BALL THE BREAKERS.exe` in the supplied bin directory.
Addresses below are preferred-image virtual addresses (image base 0x140000000).

## Confirmed binary evidence

* SDK command-name dispatcher: 0x140DA1470. Two-level switch table at
  0x140DA1880 / 0x140DA177C maps room and MatchMaker command IDs.
* RoomCreate request serializer: 0x140D89A90. Command 100, version 1.
  12-byte request: maxMembers u16 BE, allowEmpty byte, join byte,
  ttl u16 BE at offset 4, reserved bytes 6-7, interval u32 BE at offset 8.
* Integer writers: 0x140DBA2C0 (u16 BE), 0x140DBA310 (u32 BE).
* MatchMaker response dispatcher: 0x140D79000; ignores versions other than 1.
  Status 1 is success. Host reply parser 0x140D7B510 consumes a raw string ID.
* Game has separate MatchingSubModuleMatching and MatchingSubModuleOpen classes;
  relevant name references near 0x140819372 and 0x14081DDDA0.

## Initial server gaps

Both UDP services only handled control traffic and hardcoded redirects, not room
or matchmaking requests. The session service did not consult SessionKeyRegistry,
although HTTP get_connection_server_info issued keys through that registry.
Command parsing accepted truncated bodies. Outgoing command sequence parameters
were limited to one byte despite a 24-bit datagram header. Fragment reassembly
was not wired into either service and accepted invalid/conflicting fragment sets.

## Verification

The standalone ProtocolTests console project runs without external test packages.
Run `dotnet run --project BreakerServer/ProtocolTests/ProtocolTests.csproj`.
Binary reconstruction is still in progress; this document is not a claim of
complete matchmaking or live-client interoperability.

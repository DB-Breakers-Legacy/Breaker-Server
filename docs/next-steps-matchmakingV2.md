# Matchmaking V2 next steps

Last updated: 2026-11-08

## Current baseline

Branch: `matchmaking/restoration-main`

The branch contains the merged PR #5 cleanup plus the transport, CSMS,
matchmaking, connection-room, P2P, and lifecycle work through commit `7367b7c`.
All 67 managed tests pass and the Release build has zero warnings. The test
project passes `dotnet format`; four unrelated baseline import-order findings
remain in the server project.

This is a synthetic implementation baseline, not proof of native-client
interoperability. No patched game client has been run and no long multi-peer soak
has been performed.

## Priority 1 - Validate the real client safely

1. Use a disposable isolated guest and dedicated test account.
2. Enforce an external firewall allowlist for the private backend and approved
   test peers. Do not rely on the native hooks as the network boundary.
3. Use loopback backend and news URLs; block official and unrelated external
   destinations.
4. Start mitmproxy and Wireshark before launching the patched client.
5. Capture login, queue, eight-player assembly, connection handoff, P2P setup,
   gameplay, results, and requeue.
6. Record each failing command, sub-ID, HTTP route, and state transition with
   capture/frame references. Fix one evidenced incompatibility at a time and
   commit it with a focused regression test and a short research-log update.

Done when the patched client completes that entire sequence using only private
infrastructure and the capture contains no unintended destinations.

## Priority 2 - Confirm provisional protocol behavior

- `adjustment_data_manage/get_version` currently returns `[0, 2]`.
- `season/get_selectable_stage` currently returns `[0, [1000]]`.

These are provisional development shapes, not confirmed captures. Replace them
if live or archived evidence disagrees, and add the corresponding replay test.

Also resolve the executable `isInvited` versus sanitized-note `isInvite`
disagreement before relying on that join field. Record the capture ID/frame and
executable build used to settle it.

## Priority 3 - Finish targeted native research

A throwaway Ghidra pass established that:

- room-create response vtable entries 1/2 return `13000`;
- matching-complete notification entries 1/2 return `12105`;
- entry-3 functions `0x14055AD80` and `0x14055AC40` are deleting destructors,
  not parsers.

Continue with constructor/callback analysis for field-level response and
notification parsing. Confirm the remaining request serializers and command
metadata only where they answer a concrete client or implementation question.
Do not start a broad whole-binary decompilation project.

## Priority 4 - Lifecycle and soak verification

- Run repeated eight-player matches and disconnect/requeue cycles.
- Exercise duplicate joins, queue timeout, member leave, room dissolution, and
  non-member injection rejection.
- Verify reliable relay retransmission stops after ACK and unreliable relay does
  not retransmit.
- Check that sessions, fragments, recent datagrams, and unacked messages remain
  bounded and are removed on FIN/expiry.
- Confirm the queue-version and selectable-stage responses during the same run.

## Ponytail review - optional small cleanup

These reductions are optional and should not delay live validation:

- `DiarkisPacketFactory.cs:23`: replace the manual `Combine` allocation/copy
  loop with `parts.SelectMany(x => x).ToArray()` (about 11 lines).
- `DiarkisUdpService.cs:38`: remove the unused `PacketCount` property and its
  increment (about 2 lines).
- `ConnectionRoomCoordinator.cs:90`: collapse the duplicate room-command
  overloads into the `CommandEnvelope` API used by production and tests (about
  5 lines).
- `CsmsCodecTests.cs:146`: replace the manual byte-pattern search with
  `Span<byte>.IndexOf` (about 9 lines).

Estimated reduction: about 27 lines. Apply these only with the existing suite
passing, and keep them separate from protocol behavior changes.

## Commit and PR checklist

- [x] Transport and crypto restored with synthetic tests.
- [x] CSMS framing and fragmentation implemented.
- [x] Synthetic eight-player matchmaking implemented.
- [x] Connection rooms and P2P relay implemented.
- [x] HTTP lifecycle replay implemented.
- [x] Targeted native parser metadata corrected.
- [ ] Native client completes the full flow in an isolated captured run.
- [ ] Provisional queue responses confirmed.
- [ ] Invitation spelling resolved.
- [ ] Long multi-peer soak completed.
- [ ] Optional ponytail reductions applied or deliberately deferred.

Continue committing each completed milestone or evidence-backed fix together
with its short summary in `new-matchmaking-research.md`.

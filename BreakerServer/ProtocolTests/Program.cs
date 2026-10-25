using BreakerServer.Protocol.Diarkis;

int passed = 0;
void Check(bool condition, string name) { if (!condition) throw new Exception(name); passed++; }
void Reject(Action action, string name) {
    try { action(); } catch (InvalidDataException) { passed++; return; }
    throw new Exception(name);
}
foreach (var sequence in new[] { 0, 255, 256, 65535, 65536, 0xffffff }) {
    var header = DatagramHeader.Parse(DatagramHeader.Pack(sequence, 7));
    Check(header.Sequence == sequence && header.PacketType == 7, "24-bit sequence");
}
foreach (var status in new byte?[] { null, 1, 0xff }) {
    var wire = CommandEnvelopeCodec.Build(100, new byte[] { 0, 1, 2, 3 }, 1, status);
    var parsed = CommandEnvelopeCodec.Parse(wire, !status.HasValue);
    Check(parsed.Command == 100 && parsed.Version == 1 && parsed.Status == status && parsed.Body.SequenceEqual(new byte[] {0,1,2,3}), "command round trip");
    Reject(() => CommandEnvelopeCodec.Parse(wire[..^1], !status.HasValue), "reject truncated command");
}
Reject(() => CommandEnvelopeCodec.Parse(new byte[10], false), "short server header");
var keys = new DiarkisKeySet(new byte[16], new byte[16], new byte[16], new byte[16]);
foreach (var length in new[] {0,1,15,16,17,255,1024}) {
    var data = Enumerable.Range(0,length).Select(x => (byte)x).ToArray();
    var encrypted = SecureEnvelope.Encrypt(data, keys);
    Check(SecureEnvelope.Decrypt(encrypted, keys).Plaintext!.SequenceEqual(data), "crypto round trip");
    encrypted[^1] ^= 1;
    Check(SecureEnvelope.Decrypt(encrypted, keys).Plaintext is null, "tampered packet");
}
var oversized = SecureEnvelope.Encrypt(new byte[1], keys);
oversized.AsSpan(0,4).Fill(255);
Check(SecureEnvelope.Decrypt(oversized, keys).Plaintext is null, "oversized declaration");
var fragments = Fragmentation.Split(17, new byte[] {1,2,3,4,5}, 2);
var reassembler = new FragmentReassembler();
Check(reassembler.Add(Fragmentation.Parse(fragments[2])!) is null, "out of order fragment");
Check(reassembler.Add(Fragmentation.Parse(fragments[0])!) is null, "incomplete fragments");
Check(reassembler.Add(Fragmentation.Parse(fragments[0])!) is null, "duplicate fragment");
Check(reassembler.Add(Fragmentation.Parse(fragments[1])!)!.SequenceEqual(new byte[]{1,2,3,4,5}), "reassemble ordered payload");
Reject(() => new FragmentReassembler().Add(new(1,2,2,[])), "invalid fragment index");
var conflicting = new FragmentReassembler();
conflicting.Add(new(1,0,2,[1]));
Reject(() => conflicting.Add(new(1,0,2,[2])), "conflicting duplicate");
Reject(() => conflicting.Add(new(1,1,3,[2])), "conflicting count");
var expiry = new FragmentReassembler();
var then = DateTimeOffset.UtcNow;
expiry.Add(new(1,0,2,[1]), then);
Check(expiry.Add(new(1,1,2,[2]), then.AddSeconds(31)) is null, "expired fragment set");
var rooms = new BreakerServer.Services.RoomRegistry();
var room = rooms.Create("host", 2, false, true, 60);
Check(room.Members.SequenceEqual(new[]{"host"}), "room host membership");
Check(rooms.Join(room.Id,"peer")?.Members.Count == 2, "room join");
Check(rooms.Join(room.Id,"peer")?.Members.Count == 2, "idempotent room join");
Check(rooms.Join(room.Id,"extra") is null, "room capacity");
Check(!rooms.SetOwner("peer", "peer"), "owner authorization");
Check(rooms.Leave("host")?.OwnerId == "peer", "owner migration");
rooms.Leave("peer");
Check(rooms.Get(room.Id) is null, "delete empty room");
var concurrentRoom = rooms.Create("leader", 8, false, true, 60);
Parallel.For(0, 50, i => rooms.Join(concurrentRoom.Id, "member"+i));
Check(rooms.Get(concurrentRoom.Id)!.Members.Count == 8, "concurrent room capacity");
var sessions = new BreakerServer.Services.SessionKeyRegistry();
var issued = sessions.Create();
Check(sessions.TryResolve(Convert.FromHexString(issued.Sid), out var issuedKeys), "issued SID lookup");
Check(issuedKeys!.Key.SequenceEqual(Convert.FromHexString(issued.Aes)), "session key material");
Check(!sessions.TryResolve(new byte[15], out _), "invalid SID length");
Check(!sessions.TryResolve(new byte[16], out _), "unknown SID");
Console.WriteLine($"Passed {passed} protocol checks.");

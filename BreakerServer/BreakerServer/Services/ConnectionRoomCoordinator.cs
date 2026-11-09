using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using BreakerServer.Protocol.Diarkis;

namespace BreakerServer.Services;

public sealed record OutgoingRoomCommand(
    string TargetUserId,
    ushort Command,
    byte Version,
    byte Status,
    byte[] Plaintext,
    bool Reliable = true);

public sealed class ConnectionRoomCoordinator(MatchmakingCoordinator matchmaking)
{
    private sealed class Member(string userId, ushort platform)
    {
        public string UserId { get; } = userId;
        public ushort Platform { get; } = platform;
        public string SdpData { get; set; } = "{}";
        public string? PublicAddress { get; set; }
        public string? LanAddress { get; set; }
        public byte[] MemberId { get; set; } = new byte[16];
        public int P2PCompleted { get; set; }
    }

    private sealed class Room(string connectionRoomId)
    {
        public string ConnectionRoomId { get; } = connectionRoomId;
        public string DiarkisRoomId { get; } = Guid.NewGuid().ToString("N") + Guid.NewGuid().ToString("N")[..20];
        public List<Member> Members { get; } = [];
    }

    private readonly object _sync = new();
    private readonly Dictionary<string, Room> _rooms = new(StringComparer.Ordinal);
    private int _serial;

    public IReadOnlyList<OutgoingCsms> Handle(CsmsPacket request)
    {
        lock (_sync)
        {
            var binding = matchmaking.GetConnectionBinding(request.UserId);
            var room = _rooms.GetValueOrDefault(binding.ConnectionRoomId);
            if (room is null)
            {
                room = new Room(binding.ConnectionRoomId);
                _rooms.Add(room.ConnectionRoomId, room);
            }

            var member = room.Members.FirstOrDefault(x => x.UserId == request.UserId);
            if (member is null)
            {
                member = new Member(request.UserId, request.Platform);
                room.Members.Add(member);
            }

            using var data = JsonDocument.Parse(request.Data);
            if (data.RootElement.TryGetProperty("sdpData", out _))
                member.SdpData = StringProperty(data.RootElement, "sdpData");

            return request.SubId switch
            {
                22000 => Attach(room, member, 23000, includeGuid: false),
                22001 => Attach(room, member, 23001, includeGuid: true),
                22004 => Attach(room, member, 23004, includeGuid: false),
                22002 => Leave(room, member),
                _ => throw new InvalidDataException($"Unsupported connection-room sub-ID {request.SubId}.")
            };
        }
    }

    public void SetEndpoints(string userId, string? publicAddress, string? lanAddress)
    {
        lock (_sync)
        {
            var member = FindMember(userId);
            member.PublicAddress = publicAddress;
            member.LanAddress = lanAddress;
        }
    }

    public int P2PCompleted(string userId)
    {
        lock (_sync)
            return FindMember(userId).P2PCompleted;
    }

    public IReadOnlyList<OutgoingRoomCommand> HandleRoomCommand(
        string userId,
        CommandEnvelope envelope)
    {
        lock (_sync)
        {
            var (room, member) = RequireRoomMember(userId);
            return envelope.Command switch
            {
                11 => MemberList(room),
                101 => [Reply(member, 101, 1, RoomGuidResponse(room))],
                102 => Detach(room, member, envelope.Body),
                103 => RegisterMemberId(room, member, envelope.Body),
                115 => [Reply(member, 115, 1, "OK"u8.ToArray())],
                127 => [Reply(member, 127, 0, [0])],
                19 => RelayProfile(room, member, envelope.Body, command: 19),
                24 => RelayProfile(room, member, envelope.Body, command: 24),
                104 => RelayMessage(room, member, envelope.Body),
                301 => P2PComplete(member),
                _ => throw new InvalidDataException($"Unsupported session-room command {envelope.Command}.")
            };
        }
    }

    private IReadOnlyList<OutgoingCsms> Attach(Room room, Member member, uint responseId, bool includeGuid)
    {
        var output = new List<OutgoingCsms>
        {
            Response(member, responseId, includeGuid
                ? (object)new { status = 0, diarkisRoomId = room.DiarkisRoomId }
                : new { status = 0 })
        };
        if (responseId == 23001)
            output.Add(Push(member, 22100, new { status = 0, roomMemberList = room.Members.Select(x => new { userId = x.UserId }).ToArray() }));
        output.Add(Push(member, 22104, new { userId = member.UserId, sdpData = member.SdpData }));
        output.Add(Push(member, 22101, new { connectionRoomId = room.ConnectionRoomId }));
        return output;
    }

    private IReadOnlyList<OutgoingCsms> Leave(Room room, Member member)
    {
        room.Members.Remove(member);
        var output = new List<OutgoingCsms> { Response(member, 23002, new { status = 0 }) };
        foreach (var target in room.Members)
        {
            output.Add(Push(target, 22105, new
            {
                connectionRoomId = room.ConnectionRoomId,
                roomOwnerUserId = room.Members[0].UserId,
                roomMemberList = room.Members.Select(x => new { userId = x.UserId }).ToArray(),
                total = room.Members.Count,
                serial = ++_serial
            }));
        }
        if (room.Members.Count == 0)
            _rooms.Remove(room.ConnectionRoomId);
        return output;
    }

    private IReadOnlyList<OutgoingRoomCommand> MemberList(Room room)
    {
        var payload = room.Members.SelectMany(x => UidRecord(x.UserId)).ToList();
        if (room.Members.Count > 1)
            payload.AddRange(UidRecord(room.Members[0].UserId));
        return room.Members.Select(x => Reply(x, 11, 1, payload.ToArray())).ToArray();
    }

    private IReadOnlyList<OutgoingRoomCommand> Detach(Room room, Member member, byte[] plaintext)
    {
        if (plaintext.Length != 70 || Encoding.ASCII.GetString(plaintext, 52, 18) != member.UserId)
            throw new InvalidDataException("Invalid room detach payload.");
        room.Members.Remove(member);
        var output = new List<OutgoingRoomCommand> { Reply(member, 102, 1, "OK"u8.ToArray()) };
        foreach (var target in room.Members)
        {
            output.Add(new OutgoingRoomCommand(target.UserId, 102, 0, 0xFF, UidRecord(member.UserId)));
            output.Add(new OutgoingRoomCommand(target.UserId, 14, 0, 0xFF, UidRecord(member.UserId)));
        }
        if (room.Members.Count == 0)
            _rooms.Remove(room.ConnectionRoomId);
        return output;
    }

    private IReadOnlyList<OutgoingRoomCommand> RegisterMemberId(Room room, Member member, byte[] plaintext)
    {
        if (plaintext.Length != 69 || plaintext[0] != 1)
            throw new InvalidDataException("Invalid member-ID registration.");
        member.MemberId = plaintext.AsSpan(53, 16).ToArray();
        var output = room.Members.Select(target =>
            new OutgoingRoomCommand(target.UserId, 103, 0, 0xFF,
                DiarkisPacketFactory.Combine(U32(17), member.MemberId, [0x70]))).ToList();
        output.Add(new OutgoingRoomCommand(member.UserId, 127, 0, 0xFF, BuildMesh(room)));
        return output;
    }

    private IReadOnlyList<OutgoingRoomCommand> RelayProfile(Room room, Member member, byte[] plaintext, ushort command)
    {
        if (command == 19)
        {
            if (plaintext.Length != 26 || plaintext[0] != 0 || plaintext[1] != 0x12)
                throw new InvalidDataException("Invalid profile relay.");
            var uid = Encoding.ASCII.GetString(plaintext, 2, 18);
            if (uid != member.UserId)
                throw new InvalidDataException("Profile sender does not match the session user.");
            var payload = DiarkisPacketFactory.Combine([0x12], plaintext.AsSpan(2).ToArray());
            return room.Members.Where(x => x.UserId != member.UserId)
                .Select(x => new OutgoingRoomCommand(x.UserId, 19, 0, 0xFF, payload, Reliable: false)).ToArray();
        }

        if (plaintext.Length < 104 || plaintext[0] != 0)
            throw new InvalidDataException("Invalid addressed profile relay.");
        var targetUid = Encoding.ASCII.GetString(plaintext, 61, 18);
        var senderUid = Encoding.ASCII.GetString(plaintext, 80, 18);
        var target = RequireMember(room, targetUid);
        if (senderUid != member.UserId)
            throw new InvalidDataException("Addressed profile sender does not match the session user.");
        return [new OutgoingRoomCommand(target.UserId, 24, 0, 0xFF,
            DiarkisPacketFactory.Combine([0x12], plaintext.AsSpan(80, 24).ToArray()), Reliable: false)];
    }

    private IReadOnlyList<OutgoingRoomCommand> RelayMessage(Room room, Member member, byte[] plaintext)
    {
        if (plaintext.Length < 73)
            throw new InvalidDataException("Invalid room message.");
        var reliable = plaintext[0] != 0;
        var blockLength = BinaryPrimitives.ReadUInt32BigEndian(plaintext.AsSpan(53, 4));
        var offset = 57;
        if (blockLength > int.MaxValue || offset + (int)blockLength > plaintext.Length)
            throw new InvalidDataException("Invalid room-message recipient block.");

        var recipients = new List<Member>();
        var blockEnd = offset + (int)blockLength;
        while (offset < blockEnd)
        {
            var length = BinaryPrimitives.ReadUInt32BigEndian(plaintext.AsSpan(offset, 4));
            offset += 4;
            if (length > int.MaxValue || offset + (int)length > blockEnd)
                throw new InvalidDataException("Invalid room-message recipient length.");
            recipients.Add(RequireMember(room, Encoding.ASCII.GetString(plaintext, offset, (int)length)));
            offset += (int)length;
        }

        var message = plaintext.AsSpan(blockEnd).ToArray();
        if (message.Length < 16)
            throw new InvalidDataException("Room message has no sender ID.");
        var payload = DiarkisPacketFactory.Combine(U32(message.Length), message);
        return recipients.Select(x => new OutgoingRoomCommand(
            x.UserId, 104, 0, 0xFF, payload, reliable)).ToArray();
    }

    private static IReadOnlyList<OutgoingRoomCommand> P2PComplete(Member member)
    {
        member.P2PCompleted++;
        return [];
    }

    private byte[] BuildMesh(Room room)
    {
        var records = room.Members.SelectMany(member =>
        {
            var publicEndpoint = Endpoint(member.PublicAddress ?? "127.0.0.1:0");
            var lanEndpoint = Endpoint(member.LanAddress ?? member.PublicAddress ?? "127.0.0.1:0");
            var blob = DiarkisPacketFactory.Combine(publicEndpoint, lanEndpoint);
            var base64 = Encoding.ASCII.GetBytes(Convert.ToBase64String(blob));
            return DiarkisPacketFactory.Combine(U32(18), Uid(member.UserId), U32(base64.Length), base64);
        }).ToArray();
        return DiarkisPacketFactory.Combine(U32(48 + records.Length), new byte[48], records);
    }

    private static byte[] Endpoint(string address)
    {
        var bytes = Encoding.ASCII.GetBytes(address);
        return DiarkisPacketFactory.Combine(U32(bytes.Length), bytes);
    }

    private static byte[] RoomGuidResponse(Room room) =>
        DiarkisPacketFactory.Combine(new byte[4], Uid(room.DiarkisRoomId));

    private static byte[] UidRecord(string userId) => DiarkisPacketFactory.Combine(U32(18), Uid(userId));
    private static byte[] Uid(string value) => Encoding.ASCII.GetBytes(value);
    private static byte[] U32(int value)
    {
        var bytes = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, (uint)value);
        return bytes;
    }

    private static OutgoingCsms Response(Member member, uint subId, object data) =>
        new(member.UserId, member.Platform, subId, JsonSerializer.Serialize(data), Push: false);

    private static OutgoingCsms Push(Member member, uint subId, object data) =>
        new(member.UserId, member.Platform, subId, JsonSerializer.Serialize(data), Push: true);

    private static OutgoingRoomCommand Reply(Member member, ushort command, byte status, byte[] payload) =>
        new(member.UserId, command, 0, status, payload);

    private (Room Room, Member Member) RequireRoomMember(string userId)
    {
        var room = _rooms.Values.FirstOrDefault(x => x.Members.Any(member => member.UserId == userId))
            ?? throw new InvalidDataException("Missing room ID");
        return (room, room.Members.First(x => x.UserId == userId));
    }

    private static Member RequireMember(Room room, string userId) =>
        room.Members.FirstOrDefault(x => x.UserId == userId)
        ?? throw new InvalidDataException("The target user is not a room member.");

    private Member FindMember(string userId) => RequireRoomMember(userId).Member;

    private static string StringProperty(JsonElement data, string name) =>
        data.TryGetProperty(name, out var value)
            ? value.ValueKind == JsonValueKind.String ? value.GetString() ?? string.Empty : value.GetRawText()
            : string.Empty;
}

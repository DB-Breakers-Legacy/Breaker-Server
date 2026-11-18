using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using BreakerServer.Services;
using Microsoft.Extensions.Options;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class ConnectionRoomCoordinatorTests
{
    [Fact]
    public void EightMembersAttachExchangeMeshRelayAndDissolve()
    {
        var matchmaking = CreateMatchmaking();
        var connectionId = Assemble(matchmaking);
        var rooms = new ConnectionRoomCoordinator(matchmaking);
        var users = Enumerable.Range(1, 8).Select(UserId).ToArray();
        string diarkisRoomId = string.Empty;

        foreach (var userId in users)
        {
            var attached = rooms.Handle(new CsmsPacket(userId, 3, 22001, "025348",
                JsonSerializer.Serialize(new { connectionRoomId = connectionId }), false));
            Assert.Equal(23001U, attached[0].SubId);
            diarkisRoomId = JsonDocument.Parse(attached[0].Data)
                .RootElement.GetProperty("diarkisRoomId").GetString()!;
            Assert.Equal([22100U, 22104U, 22101U], attached.Skip(1).Select(x => x.SubId).ToArray());
            rooms.SetEndpoints(userId, $"192.0.2.{Array.IndexOf(users, userId) + 1}:5000", $"192.168.1.{Array.IndexOf(users, userId) + 1}:5000");
        }
        Assert.Equal(52, diarkisRoomId.Length);

        var first = users[0];
        var create = rooms.HandleRoomCommand(first, Envelope(101, []));
        Assert.Equal(56, create.Single().Plaintext.Length);
        Assert.Equal(diarkisRoomId, Encoding.ASCII.GetString(create[0].Plaintext, 4, 52));

        for (var index = 0; index < users.Length; index++)
        {
            var registration = DiarkisPacketFactory.Combine(
                [1], Encoding.ASCII.GetBytes(diarkisRoomId), Enumerable.Repeat((byte)index, 16).ToArray());
            var messages = rooms.HandleRoomCommand(users[index], Envelope(103, registration));
            Assert.Equal(9, messages.Count);
            var mesh = messages.Last();
            Assert.Equal(127, mesh.Command);
            Assert.Equal((uint)(mesh.Plaintext.Length - 4), BinaryPrimitives.ReadUInt32BigEndian(mesh.Plaintext));
        }

        var poll = rooms.HandleRoomCommand(first, Envelope(11, []));
        Assert.Equal(9 * 22, poll[0].Plaintext.Length);
        for (var peer = 1; peer <= 7; peer++)
            rooms.HandleRoomCommand(first, Envelope(301, []));
        Assert.Equal(7, rooms.P2PCompleted(first));

        var profile = DiarkisPacketFactory.Combine([0, 0x12], Uid(first), [1, 0, 2, 0, 0, 0]);
        var profileRelay = rooms.HandleRoomCommand(first, Envelope(19, profile));
        Assert.Equal(7, profileRelay.Count);
        Assert.All(profileRelay, x => Assert.False(x.Reliable));
        Assert.All(profileRelay, x => Assert.Equal(25, x.Plaintext.Length));

        var addressed = new byte[104];
        addressed[0] = 0;
        Encoding.ASCII.GetBytes(diarkisRoomId).CopyTo(addressed, 1);
        BinaryPrimitives.WriteUInt32BigEndian(addressed.AsSpan(53, 4), 22);
        BinaryPrimitives.WriteUInt32BigEndian(addressed.AsSpan(57, 4), 18);
        Uid(users[1]).CopyTo(addressed, 61);
        addressed[79] = 0x12;
        Uid(first).CopyTo(addressed, 80);
        addressed[100] = 1;
        var addressedRelay = rooms.HandleRoomCommand(first, Envelope(24, addressed));
        Assert.Single(addressedRelay);
        Assert.Equal(users[1], addressedRelay[0].TargetUserId);
        Assert.Equal(25, addressedRelay[0].Plaintext.Length);

        var message = Enumerable.Range(0, 32).Select(x => (byte)x).ToArray();
        var relayPayload = BuildMessage(diarkisRoomId, [users[1], users[2]], message, reliable: false);
        var relayed = rooms.HandleRoomCommand(first, Envelope(104, relayPayload));
        Assert.Equal(2, relayed.Count);
        Assert.All(relayed, x => Assert.False(x.Reliable));
        Assert.All(relayed, x => Assert.Equal(message, x.Plaintext.AsSpan(4).ToArray()));
        Assert.Throws<InvalidDataException>(() =>
            rooms.HandleRoomCommand(UserId(9), Envelope(104, relayPayload)));

        Assert.Equal("OK"u8.ToArray(), rooms.HandleRoomCommand(first, Envelope(115, []))[0].Plaintext);
        for (var index = 7; index >= 0; index--)
        {
            var left = rooms.Handle(new CsmsPacket(users[index], 3, 22002, "025348",
                JsonSerializer.Serialize(new { roomId = connectionId }), false));
            Assert.Equal(23002U, left[0].SubId);
            if (index > 0)
                Assert.Equal(index, left.Count(x => x.SubId == 22105));
        }
        Assert.Throws<InvalidDataException>(() => rooms.HandleRoomCommand(first, Envelope(11, [])));
    }

    private static MatchmakingCoordinator CreateMatchmaking() => new(
        Options.Create(new BreakerOptions { UdpHost = "127.0.0.1", UdpSessionPort = 7102 }));

    private static string Assemble(MatchmakingCoordinator coordinator)
    {
        for (var index = 1; index <= 8; index++)
        {
            var userId = UserId(index);
            var created = coordinator.Handle(new CsmsPacket(userId, 3, 12000, "025348", "{\"gameRule\":0}", false));
            var roomId = JsonDocument.Parse(created[1].Data).RootElement.GetProperty("roomId").GetString()!;
            coordinator.Handle(new CsmsPacket(userId, 3, 12017, "025348",
                JsonSerializer.Serialize(new { roomId }), false));
        }
        return coordinator.GetConnectionBinding(UserId(1)).ConnectionRoomId;
    }

    private static CommandEnvelope Envelope(ushort command, byte[] body) =>
        new(0, command, null, body.Length, body, 10 + body.Length);

    private static byte[] BuildMessage(string roomId, string[] recipients, byte[] message, bool reliable)
    {
        var recipientBlock = recipients.SelectMany(x => DiarkisPacketFactory.Combine(U32(18), Uid(x))).ToArray();
        return DiarkisPacketFactory.Combine(
            [reliable ? (byte)1 : (byte)0],
            Uid(roomId), U32(recipientBlock.Length), recipientBlock, message);
    }

    private static byte[] Uid(string value) => Encoding.ASCII.GetBytes(value);
    private static string UserId(int index) => $"10000000000000000{index}";

    private static byte[] U32(int value)
    {
        var bytes = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(bytes, (uint)value);
        return bytes;
    }
}

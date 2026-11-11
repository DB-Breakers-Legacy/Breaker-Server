using System.Buffers.Binary;
using System.Text;
using BreakerServer.Protocol.Game;
using MessagePack;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class GameMessageTests
{
    // Same layout as the live 32000 request of 2026-11-10: "CSMS", "09.01", u16 0, user id, serial 24, id 32000.
    private static byte[] ClientRequest(string json)
    {
        var header = new byte[GameMessage.HeaderLength];
        "CSMS"u8.CopyTo(header);
        "09.01"u8.CopyTo(header.AsSpan(4));
        "AC9E3C6FD7A94D4476FF07C883B7F7E0"u8.CopyTo(header.AsSpan(14));
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(46), 24);
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(50), 32000);
        var map = MessagePackSerializer.Serialize(new Dictionary<string, object>
        {
            ["title_cd"] = "025348", ["platform"] = 3, ["user_id"] = "AC9E3C6FD7A94D4476FF07C883B7F7E0",
            ["id"] = 32000, ["data"] = json,
        });
        return [.. header, .. map];
    }

    [Fact]
    public void ParsesTheClientFrameAndBuildsAMatchingReply()
    {
        var request = GameMessage.Parse(ClientRequest("{\"gameRule\":3}"));
        Assert.Equal(32000, request.Id);
        Assert.Equal(24u, request.Serial);
        Assert.Equal("09.01", request.AppVersion);
        Assert.Equal("AC9E3C6FD7A94D4476FF07C883B7F7E0", request.UserId);
        Assert.Equal("{\"gameRule\":3}", request.Data);

        var reply = GameMessage.Parse(request.Reply(33000, "{\"status\":0}"));
        Assert.Equal(33000, reply.Id);
        Assert.Equal(33000u, reply.HeaderMessageId);
        Assert.Equal(24u, reply.Serial);
        Assert.Equal(request.UserId, reply.UserId);
        Assert.Equal("{\"status\":0}", reply.Data);
    }
}

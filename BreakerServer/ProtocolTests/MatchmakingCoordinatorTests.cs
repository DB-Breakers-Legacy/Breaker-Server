using System.Text.Json;
using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using BreakerServer.Services;
using Microsoft.Extensions.Options;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class MatchmakingCoordinatorTests
{
    [Fact]
    public void EightPlayersProduceRolesFinalRostersAndOneConnectionBinding()
    {
        var coordinator = CreateCoordinator();
        IReadOnlyList<OutgoingCsms> final = [];
        for (var index = 1; index <= 8; index++)
        {
            var userId = UserId(index);
            var created = coordinator.Handle(Request(userId, 12000, CreateData(index)));
            Assert.Equal([12120U, 13000U], created.Select(x => x.SubId).ToArray());
            Assert.True(created[0].Push);
            Assert.False(created[1].Push);
            var roomId = Property(created[1].Data, "roomId").GetString()!;

            coordinator.Handle(Request(userId, 12012, "{\"desiredRole\":20,\"desiredStageId\":1000}"));
            coordinator.Handle(Request(userId, 12005,
                JsonSerializer.Serialize(new { roomId, sdpData = SdpData(index) })));
            final = coordinator.Handle(Request(userId, 12017,
                JsonSerializer.Serialize(new { roomId })));
        }

        var roleMessages = final.Where(x => x.SubId == 12105).ToArray();
        var rosterMessages = final.Where(x => x.SubId == 12102).ToArray();
        var bindingMessages = final.Where(x => x.SubId == 12119).ToArray();
        Assert.Equal(8, roleMessages.Length);
        Assert.Equal(65, rosterMessages.Length);
        Assert.Equal(8, bindingMessages.Length);
        var finalList = final.ToList();
        Assert.True(finalList.FindIndex(x => x.SubId == 13017) <
                    finalList.FindIndex(x => x.SubId == 12105));

        var roles = Property(roleMessages[0].Data, "roleList").EnumerateArray()
            .Select(x => x.GetProperty("role").GetInt32()).ToArray();
        Assert.Equal(1, roles.Count(x => x == 0));
        Assert.Equal(7, roles.Count(x => x == 1));

        var finalRosters = finalList
            .Skip(finalList.FindIndex(x => x.SubId == 12105))
            .Where(x => x.SubId == 12102 && Property(x.Data, "total").GetInt32() == 8)
            .ToArray();
        Assert.Equal(64, finalRosters.Length);
        foreach (var roster in finalRosters)
        {
            Assert.Equal(8, Property(roster.Data, "roomMemberList").GetArrayLength());
            Assert.Equal(18, roster.TargetUserId.Length);
        }
        Assert.Equal(8, finalRosters.Select(x => x.TargetUserId).Distinct().Count());

        var sample = finalRosters[0];
        var push = CsmsCodec.BuildResponse(
            sample.TargetUserId, sample.TargetPlatform, sample.SubId, sample.Data, push: true);
        var fragments = Fragmentation.Split(9, push, 1300);
        Assert.True(fragments.Count > 1);
        var reassembler = new FragmentReassembler();
        byte[]? complete = null;
        foreach (var fragment in fragments)
            complete = reassembler.Add(Fragmentation.Parse(fragment)!) ?? complete;
        Assert.Equal(push, complete);

        var bindings = Enumerable.Range(1, 8)
            .Select(index => coordinator.GetConnectionBinding(UserId(index)))
            .ToArray();
        Assert.Single(bindings.Select(x => x.ConnectionRoomId).Distinct());
        Assert.All(bindings, x => Assert.Equal("127.0.0.1", x.Host));
        Assert.All(bindings, x => Assert.Equal(7102, x.Port));
        Assert.Equal(0, coordinator.QueueCount);
    }

    [Fact]
    public void InviteJoinSharesRoomAndDisconnectShrinksRoster()
    {
        var coordinator = CreateCoordinator();
        var ownerId = UserId(1);
        var guestId = UserId(2);
        var created = coordinator.Handle(Request(ownerId, 12000, CreateData(1)));
        var roomId = Property(created[1].Data, "roomId").GetString()!;
        var joinData = JsonSerializer.Serialize(new
        {
            roomId,
            userName = "Player2",
            userRank = 2,
            userRivalRank = 3,
            sdpData = SdpData(2)
        });

        var joined = coordinator.Handle(Request(guestId, 12001, joinData));
        Assert.Equal(2, joined.Count(x => x.SubId == 12101));
        Assert.Equal(13001U, joined.Last().SubId);
        Assert.Equal(2, Property(joined[0].Data, "total").GetInt32());
        Assert.Equal(ownerId, Property(joined[0].Data, "roomOwnerUserId").GetString());
        Assert.Equal(2, Property(joined[0].Data, "roomMemberList").GetArrayLength());

        coordinator.Handle(Request(guestId, 12001, joinData));
        Assert.Equal(2, coordinator.PlayerCount);
        coordinator.RemoveSession(guestId);
        var ownerUpdate = coordinator.Handle(Request(ownerId, 12005, "{\"sdpData\":\"{}\"}"));
        Assert.Equal(1, Property(ownerUpdate[0].Data, "total").GetInt32());
    }

    [Fact]
    public void DuplicateJoinTimeoutDisconnectAndRequeueDoNotLeaveStalePlayers()
    {
        var coordinator = CreateCoordinator();
        var userId = UserId(1);
        var first = coordinator.Handle(Request(userId, 12000, CreateData(1)));
        var firstRoom = Property(first[1].Data, "roomId").GetString();
        Assert.Equal(1, coordinator.QueueCount);

        var duplicate = coordinator.Handle(Request(userId, 12000, CreateData(1)));
        var duplicateRoom = Property(duplicate[1].Data, "roomId").GetString();
        Assert.NotEqual(firstRoom, duplicateRoom);
        Assert.Equal(1, coordinator.PlayerCount);
        Assert.Equal(1, coordinator.QueueCount);

        coordinator.Expire(DateTimeOffset.UtcNow.AddMinutes(3));
        Assert.Equal(0, coordinator.PlayerCount);
        Assert.Equal(0, coordinator.QueueCount);
        Assert.Throws<InvalidDataException>(() => coordinator.GetConnectionBinding(userId));

        coordinator.Handle(Request(userId, 12000, CreateData(1)));
        coordinator.RemoveSession(userId);
        Assert.Equal(0, coordinator.PlayerCount);

        coordinator.Handle(Request(userId, 12000, CreateData(1)));
        Assert.Equal(1, coordinator.PlayerCount);
        Assert.True(coordinator.IsQueued(userId));
    }

    private static MatchmakingCoordinator CreateCoordinator() => new(
        Options.Create(new BreakerOptions
        {
            UdpHost = "127.0.0.1",
            UdpSessionPort = 7102
        }));

    private static CsmsPacket Request(string userId, uint subId, string data) =>
        new(userId, 3, subId, "025348", data, IsPush: false);

    private static string CreateData(int index) => JsonSerializer.Serialize(new
    {
        gameRule = 0,
        userName = $"Player{index}",
        userRank = index,
        userRivalRank = index + 1,
        appVersion = "1.0",
        blockList = Array.Empty<int>(),
        sdpData = SdpData(index),
        natType = "0",
        userIp = "127.0.0.1",
        platformUserId = UserId(index),
        diarkisNatType = 0
    });

    private static string SdpData(int index) => JsonSerializer.Serialize(new
    {
        platform = 3,
        pf_uid = UserId(index),
        padding = new string('x', 400)
    });

    private static string UserId(int index) => $"10000000000000000{index}";

    private static JsonElement Property(string data, string name) =>
        JsonDocument.Parse(data).RootElement.GetProperty(name);
}

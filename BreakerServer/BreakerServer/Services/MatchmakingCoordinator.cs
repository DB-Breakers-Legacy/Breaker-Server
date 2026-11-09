using System.Security.Cryptography;
using System.Text.Json;
using BreakerServer.Configuration;
using BreakerServer.Protocol.Diarkis;
using Microsoft.Extensions.Options;

namespace BreakerServer.Services;

public sealed record OutgoingCsms(
    string TargetUserId,
    ushort TargetPlatform,
    uint SubId,
    string Data,
    bool Push);

public sealed record ConnectionBinding(string Host, int Port, string ConnectionRoomId);

public sealed class MatchmakingCoordinator(IOptions<BreakerOptions> options)
{
    private const int RequiredPlayers = 8;
    private static readonly TimeSpan QueueLifetime = TimeSpan.FromMinutes(2);

    private sealed class Player(string userId, ushort platform)
    {
        public string UserId { get; } = userId;
        public ushort Platform { get; } = platform;
        public string RoomId { get; set; } = NewId(16);
        public string RoomOwnerId { get; set; } = userId;
        public List<Player> RoomMembers { get; set; } = [];
        public string RoomKey { get; } = RandomNumberGenerator.GetInt32(0, 100000000).ToString("D8");
        public string BattleRoomId { get; } = NewId(16);
        public string SdpData { get; set; } = "{}";
        public string UserData { get; set; } = "{}";
        public int DesiredRole { get; set; }
        public int DesiredStageId { get; set; }
        public bool Completed { get; set; }
        public MatchGroup? Group { get; set; }
        public DateTimeOffset LastSeen { get; set; } = DateTimeOffset.UtcNow;
    }

    private sealed class MatchGroup
    {
        public string ConnectionRoomId { get; } = NewId(16);
        public List<Player> Members { get; } = [];
    }

    private readonly string _host = options.Value.UdpHost;
    private readonly int _port = options.Value.UdpSessionPort;
    private readonly object _sync = new();
    private readonly Dictionary<string, Player> _players = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _activeBattles = new(StringComparer.Ordinal);
    private int _serial;

    public int QueueCount
    {
        get
        {
            lock (_sync)
                return _players.Values.Count(x => x.Group is null);
        }
    }

    public int PlayerCount
    {
        get
        {
            lock (_sync)
                return _players.Count;
        }
    }

    public IReadOnlyList<OutgoingCsms> Handle(CsmsPacket request)
    {
        lock (_sync)
        {
            return request.SubId switch
            {
                12000 => Create(request),
                12001 => Join(request),
                12002 => StatusOnly(request, 13002),
                12005 => UpdateMember(request),
                12012 => UpdatePreferences(request),
                12017 => Complete(request),
                _ => throw new InvalidDataException($"Unsupported matchmaking sub-ID {request.SubId}.")
            };
        }
    }

    public bool IsQueued(string? userId)
    {
        if (userId is null)
            return false;
        lock (_sync)
            return _players.TryGetValue(userId, out var player) && player.Group is null;
    }

    public ConnectionBinding GetConnectionBinding(string userId)
    {
        lock (_sync)
        {
            if (!_players.TryGetValue(userId, out var player) || player.Group is null)
                throw new InvalidDataException("The user has no assembled battle connection room.");
            return new ConnectionBinding(_host, _port, player.Group.ConnectionRoomId);
        }
    }

    public string StartBattle(string userId, string leader)
    {
        lock (_sync)
        {
            GetConnectionBinding(userId);
            if (string.IsNullOrWhiteSpace(leader))
                throw new InvalidDataException("A battle leader is required.");
            if (!_activeBattles.TryGetValue(userId, out var battleId))
            {
                battleId = $"{leader}_{DateTimeOffset.Now:yyyyMMddHHmmss}";
                _activeBattles[userId] = battleId;
            }
            return battleId;
        }
    }

    public string RequireBattle(string userId)
    {
        lock (_sync)
        {
            return _activeBattles.TryGetValue(userId, out var battleId)
                ? battleId
                : throw new InvalidDataException("The user has no active battle.");
        }
    }

    public void CompleteBattle(string userId)
    {
        lock (_sync)
        {
            RequireBattle(userId);
            _activeBattles.Remove(userId);
            RemovePlayer(userId);
        }
    }

    public void RemoveSession(string? userId)
    {
        if (userId is null)
            return;
        lock (_sync)
        {
            _activeBattles.Remove(userId);
            RemovePlayer(userId);
        }
    }

    public void Expire(DateTimeOffset now)
    {
        lock (_sync)
        {
            var expired = _players.Values
                .Where(x => x.Group is null && now - x.LastSeen > QueueLifetime)
                .Select(x => x.UserId)
                .ToArray();
            foreach (var userId in expired)
                RemovePlayer(userId);
        }
    }

    private IReadOnlyList<OutgoingCsms> Create(CsmsPacket request)
    {
        RemovePlayer(request.UserId);
        var player = new Player(request.UserId, request.Platform);
        using (var data = JsonDocument.Parse(request.Data))
        {
            player.SdpData = StringProperty(data.RootElement, "sdpData");
            player.UserData = JsonSerializer.Serialize(new Dictionary<string, object?>
            {
                ["userName"] = StringProperty(data.RootElement, "userName"),
                ["userRank"] = IntProperty(data.RootElement, "userRank"),
                ["userRivalRank"] = IntProperty(data.RootElement, "userRivalRank"),
                ["desiredRole"] = 0,
                ["desiredStageId"] = 0,
                ["platform"] = request.Platform
            });
        }

        player.RoomMembers.Add(player);
        _players.Add(player.UserId, player);
        return
        [
            Push(player, 12120, new { roomId = player.RoomId }),
            Response(player, 13000, new
            {
                status = 0,
                roomId = player.RoomId,
                roomData = "{\"gameRule\":0}",
                roomKey = player.RoomKey
            })
        ];
    }

    private IReadOnlyList<OutgoingCsms> Join(CsmsPacket request)
    {
        using var data = JsonDocument.Parse(request.Data);
        var roomId = StringProperty(data.RootElement, "roomId");
        var owner = _players.Values.FirstOrDefault(x => x.RoomId == roomId)
            ?? throw new InvalidDataException("The requested matchmaking room does not exist.");
        if (owner.UserId == request.UserId)
            return [Push(owner, 12101, RoomRoster(owner)), Response(owner, 13001, new { status = 0 })];

        RemovePlayer(request.UserId);
        var player = new Player(request.UserId, request.Platform)
        {
            RoomId = owner.RoomId,
            RoomOwnerId = owner.RoomOwnerId,
            RoomMembers = owner.RoomMembers,
            SdpData = StringProperty(data.RootElement, "sdpData")
        };
        player.UserData = JsonSerializer.Serialize(new Dictionary<string, object?>
        {
            ["userName"] = StringProperty(data.RootElement, "userName"),
            ["userRank"] = IntProperty(data.RootElement, "userRank"),
            ["userRivalRank"] = IntProperty(data.RootElement, "userRivalRank"),
            ["desiredRole"] = 0,
            ["desiredStageId"] = 0,
            ["platform"] = request.Platform
        });
        player.RoomMembers.Add(player);
        _players.Add(player.UserId, player);

        var output = player.RoomMembers
            .Select(member => Push(member, 12101, RoomRoster(player)))
            .ToList();
        output.Add(Response(player, 13001, new { status = 0 }));
        return output;
    }

    private IReadOnlyList<OutgoingCsms> UpdateMember(CsmsPacket request)
    {
        var player = RequiredPlayer(request.UserId);
        player.LastSeen = DateTimeOffset.UtcNow;
        using var data = JsonDocument.Parse(request.Data);
        if (data.RootElement.TryGetProperty("sdpData", out _))
            player.SdpData = StringProperty(data.RootElement, "sdpData");
        if (data.RootElement.TryGetProperty("userData", out _))
            player.UserData = StringProperty(data.RootElement, "userData");

        return
        [
            Push(player, 12101, RoomRoster(player)),
            Response(player, 13005, new { status = 0 })
        ];
    }

    private IReadOnlyList<OutgoingCsms> UpdatePreferences(CsmsPacket request)
    {
        var player = RequiredPlayer(request.UserId);
        player.LastSeen = DateTimeOffset.UtcNow;
        using var data = JsonDocument.Parse(request.Data);
        player.DesiredRole = IntProperty(data.RootElement, "desiredRole");
        player.DesiredStageId = IntProperty(data.RootElement, "desiredStageId");
        using var existing = JsonDocument.Parse(player.UserData);
        player.UserData = JsonSerializer.Serialize(new Dictionary<string, object?>
        {
            ["userName"] = StringProperty(existing.RootElement, "userName"),
            ["userRank"] = IntProperty(existing.RootElement, "userRank"),
            ["userRivalRank"] = IntProperty(existing.RootElement, "userRivalRank"),
            ["desiredRole"] = player.DesiredRole,
            ["desiredStageId"] = player.DesiredStageId,
            ["platform"] = player.Platform
        });

        return
        [
            Push(player, 12101, RoomRoster(player)),
            Response(player, 13012, new { status = 0 })
        ];
    }

    private IReadOnlyList<OutgoingCsms> Complete(CsmsPacket request)
    {
        var player = RequiredPlayer(request.UserId);
        player.LastSeen = DateTimeOffset.UtcNow;
        player.Completed = true;
        var output = new List<OutgoingCsms>
        {
            Push(player, 12101, RoomRoster(player)),
            Push(player, 12102, BattleRoster(player)),
            Response(player, 13017, new { status = 0, battleRoomId = player.BattleRoomId }),
            Push(player, 12118, new { roomId = player.RoomId })
        };

        var completed = _players.Values.Where(x => x.Group is null && x.Completed).ToList();
        if (completed.Count == RequiredPlayers)
        {
            var group = new MatchGroup();
            foreach (var member in completed)
                member.Group = group;
            group.Members.AddRange(completed);
            output.AddRange(MatchFound(group));
        }

        return output;
    }

    private IReadOnlyList<OutgoingCsms> MatchFound(MatchGroup group)
    {
        var output = new List<OutgoingCsms>();
        var roles = group.Members.Select((member, index) => new
        {
            userId = member.UserId,
            role = index == 0 ? 0 : 1
        }).ToArray();

        foreach (var target in group.Members)
        {
            output.Add(Push(target, 12105, new
            {
                roleList = roles,
                battleRoomId = target.BattleRoomId,
                stageId = "1000",
                patrollerPriorityPoint = 0,
                rivalPriorityPoint = 0
            }));
        }

        foreach (var keyedMember in group.Members)
        {
            var roster = BattleRoster(keyedMember, group.Members);
            foreach (var target in group.Members)
                output.Add(Push(target, 12102, roster));
        }

        foreach (var target in group.Members)
        {
            output.Add(Push(target, 12119, new
            {
                connectionRoomId = group.ConnectionRoomId,
                serverIp = _host,
                serverPort = _port
            }));
        }

        return output;
    }

    private IReadOnlyList<OutgoingCsms> StatusOnly(CsmsPacket request, uint responseSubId)
    {
        var player = RequiredPlayer(request.UserId);
        player.LastSeen = DateTimeOffset.UtcNow;
        return [Response(player, responseSubId, new { status = 0 })];
    }

    private object RoomRoster(Player player) => new
    {
        roomId = player.RoomId,
        roomOwnerUserId = player.RoomOwnerId,
        roomMemberList = player.RoomMembers.Select(Member).ToArray(),
        total = player.RoomMembers.Count,
        serial = ++_serial
    };

    private object BattleRoster(Player player) => BattleRoster(player,
        _players.Values.Where(x => x.Group is null && x.Completed).ToList());

    private object BattleRoster(Player player, IReadOnlyCollection<Player> members) => new
    {
        battleRoomId = player.BattleRoomId,
        roomOwnerUserId = player.UserId,
        roomMemberList = members.Select(Member).ToArray(),
        total = members.Count,
        serial = ++_serial
    };

    private static object Member(Player player) => new
    {
        userId = player.UserId,
        userData = player.UserData,
        sdpData = player.SdpData
    };

    private static OutgoingCsms Response(Player player, uint subId, object data) =>
        new(player.UserId, player.Platform, subId, JsonSerializer.Serialize(data), Push: false);

    private static OutgoingCsms Push(Player player, uint subId, object data) =>
        new(player.UserId, player.Platform, subId, JsonSerializer.Serialize(data), Push: true);

    private Player RequiredPlayer(string userId) =>
        _players.TryGetValue(userId, out var player)
            ? player
            : throw new InvalidDataException("Create a matchmaking room before using this sub-ID.");

    private void RemovePlayer(string userId)
    {
        if (!_players.Remove(userId, out var player))
            return;
        player.RoomMembers.Remove(player);
        player.Group?.Members.Remove(player);
    }

    private static string StringProperty(JsonElement data, string name) =>
        data.TryGetProperty(name, out var value)
            ? value.ValueKind == JsonValueKind.String ? value.GetString() ?? string.Empty : value.GetRawText()
            : string.Empty;

    private static int IntProperty(JsonElement data, string name) =>
        data.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number
            ? value.GetInt32()
            : 0;

    private static string NewId(int length) => Guid.NewGuid().ToString("N")[..length];
}

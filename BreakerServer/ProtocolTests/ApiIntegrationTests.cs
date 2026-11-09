using System.Buffers.Binary;
using System.Net;
using BreakerServer.Infrastructure;
using MessagePack;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Options;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class ApiIntegrationTests : IClassFixture<BreakerApiFactory>
{
    private const long UserId = 76561198000000001;
    private readonly BreakerApiFactory _factory;
    private readonly HttpClient _client;

    public ApiIntegrationTests(BreakerApiFactory factory)
    {
        _factory = factory;
        _client = factory.CreateClient();
    }

    [Fact]
    public async Task NumericIdentityCannotReadAnotherAccount()
    {
        using var response = await Post(
            "/dbtb-prd/025348/api/gamecurrency/get_owned",
            AuthenticatedBody(UserId.ToString()));

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
    }

    [Fact]
    public async Task LocalTicketAuthenticatesOnlyItsOwnUserAndOpensAccount()
    {
        var token = await Authenticate(UserId, UserId);
        using var response = await Post(
            "/dbtb-prd/025348/api/gamecurrency/get_owned",
            AuthenticatedBody(token));

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
    }

    [Fact]
    public async Task MismatchedLocalTicketIsRejected()
    {
        using var response = await Post("/025348/api/user/auth", AuthBody(UserId, UserId + 1));
        var body = await ReadResponse(response);
        var result = ObjectTree.List(body);

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Equal(1, ObjectTree.Int32(ObjectTree.List(result[1])[0]));
    }

    [Fact]
    public async Task OpaqueHexIsNotAcceptedAsATicket()
    {
        using var response = await Post(
            "/025348/api/user/auth",
            new object?[] { new Dictionary<string, object?>(), new object?[] { UserId, "AA", 0 } });
        var body = ObjectTree.List(await ReadResponse(response));

        Assert.Equal(1, ObjectTree.Int32(ObjectTree.List(body[1])[0]));
    }

    [Theory]
    [InlineData("/025348/api/user/auth")]
    [InlineData("/dbtb-prd/025348/api/battle/start")]
    [InlineData("/dbtb-prd/025348/api/transball/unlock_spattack")]
    public async Task MalformedMessagePackReturnsBadRequest(string path)
    {
        using var response = await PostBytes(path, new byte[] { 0, 1, 2, 3 });
        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
    }

    [Fact]
    public async Task EmptyRivalTrainingRowReturnsBadRequest()
    {
        var token = await Authenticate(UserId, UserId);
        using var response = await Post(
            "/dbtb-prd/025348/api/rival/training",
            new object?[]
            {
                new Dictionary<string, object?> { ["userId"] = token },
                new object?[] { new object?[] { Array.Empty<object?>() } }
            });

        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
    }

    [Fact]
    public void RequestBodyLimitIsEnforcedByKestrel() =>
        Assert.Equal(
            MessagePackHttp.MaxBodyBytes,
            _factory.Services.GetRequiredService<IOptions<KestrelServerOptions>>()
                .Value.Limits.MaxRequestBodySize);

    [Fact]
    public async Task CapturedPrefixRouteMapsToExistingHandlers()
    {
        using var response = await Post(
            "/dbtb-prd/LALALALALA/025348/api/news/get_2d_ui_news_list",
            new object?[] { new Dictionary<string, object?>(), Array.Empty<object?>() });

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.NotNull(await ReadResponse(response));
    }

    [Fact]
    public async Task MatchingHandoutsIssueFreshRegisteredKeysInWireOrder()
    {
        var first = await ReadMatchingHandout();
        var second = await ReadMatchingHandout();

        Assert.NotEqual(first, second);
        var registry = _factory.Services.GetRequiredService<BreakerServer.Services.SessionKeyRegistry>();
        var sid = Convert.FromHexString(first[2]);
        Assert.True(registry.TryResolve(sid, out var keys));
        Assert.Equal(first[2], Convert.ToHexString(keys!.Sid).ToLowerInvariant());
        Assert.Equal(first[3], Convert.ToHexString(keys.Key).ToLowerInvariant());
        Assert.Equal(first[4], Convert.ToHexString(keys.Iv).ToLowerInvariant());
        Assert.Equal(first[5], Convert.ToHexString(keys.Mac).ToLowerInvariant());
    }

    private async Task<string[]> ReadMatchingHandout()
    {
        using var response = await Post(
            "/dbtb-prd/LALALALALA/025348/api/battle/get_diarkis_matching_server_info",
            new object?[] { new Dictionary<string, object?>(), Array.Empty<object?>() });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);

        var body = ObjectTree.List(await ReadResponse(response));
        var handout = ObjectTree.List(ObjectTree.List(body[1])[1], 6);
        return handout.Select(ObjectTree.Text).ToArray();
    }

    [Fact]
    public async Task ConnectionHandoutRequiresAssembledBattleBinding()
    {
        var coordinator = _factory.Services.GetRequiredService<BreakerServer.Services.MatchmakingCoordinator>();
        for (var index = 1; index <= 8; index++)
        {
            var userId = index == 1 ? UserId.ToString() : $"10000000000000000{index}";
            var created = coordinator.Handle(new BreakerServer.Protocol.Diarkis.CsmsPacket(
                userId, 3, 12000, "025348", "{\"gameRule\":0}", false));
            var roomId = System.Text.Json.JsonDocument.Parse(created[1].Data)
                .RootElement.GetProperty("roomId").GetString()!;
            coordinator.Handle(new BreakerServer.Protocol.Diarkis.CsmsPacket(
                userId, 3, 12017, "025348",
                System.Text.Json.JsonSerializer.Serialize(new { roomId }), false));
        }

        var token = await Authenticate(UserId, UserId);
        using var response = await Post(
            "/dbtb-prd/LALALALALA/025348/api/battle/get_connection_server_info",
            AuthenticatedBody(token));
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var body = ObjectTree.List(await ReadResponse(response));
        var result = ObjectTree.List(body[1]);
        Assert.Equal(0, ObjectTree.Int32(result[0]));
        var handout = ObjectTree.List(result[1], 6);
        Assert.True(ObjectTree.Int32(handout[1]) > 0);
        Assert.Equal(32, ObjectTree.Text(handout[2]).Length);
    }

    [Fact]
    public async Task MessageInfoRequiresAuthenticationAndKnownMessage()
    {
        using var anonymous = await Post(
            "/dbtb-prd/025348/api/message/get_message_info",
            new object?[] { new Dictionary<string, object?>(), new object?[] { 0, 0, "missing" } });
        Assert.Equal(HttpStatusCode.BadRequest, anonymous.StatusCode);

        var token = await Authenticate(UserId, UserId);
        using var missing = await Post(
            "/dbtb-prd/025348/api/message/get_message_info",
            new object?[]
            {
                new Dictionary<string, object?> { ["userId"] = token },
                new object?[] { 0, 0, "missing" }
            });
        Assert.Equal(HttpStatusCode.BadRequest, missing.StatusCode);
    }

    private async Task<string> Authenticate(long numericId, long ticketUserId)
    {
        using var response = await Post("/025348/api/user/auth", AuthBody(numericId, ticketUserId));
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var body = ObjectTree.List(await ReadResponse(response));
        var result = ObjectTree.List(body[1]);
        Assert.Equal(0, ObjectTree.Int32(result[0]));
        return ObjectTree.Text(ObjectTree.List(result[1], 1)[0]);
    }

    private Task<HttpResponseMessage> Post(string path, object? body) =>
        PostBytes(path, MessagePackSerializer.Serialize(body, MessagePackHttp.Options));

    private async Task<HttpResponseMessage> PostBytes(string path, byte[] bytes)
    {
        using var content = new ByteArrayContent(bytes);
        content.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/x-messagepack");
        return await _client.PostAsync(path, content);
    }

    private static async Task<object?> ReadResponse(HttpResponseMessage response) =>
        ObjectTree.Normalize(MessagePackSerializer.Deserialize<object>(
            await response.Content.ReadAsByteArrayAsync(),
            MessagePackHttp.Options));

    private static object[] AuthenticatedBody(string userId) =>
        [new Dictionary<string, object?> { ["userId"] = userId }, new List<object?>()];

    private static object[] AuthBody(long numericId, long ticketUserId) =>
        [new Dictionary<string, object?>(), new object?[]
        {
            numericId,
            Convert.ToHexString(LocalTicket((ulong)ticketUserId)),
            0
        }];

    private static byte[] LocalTicket(ulong userId)
    {
        var ticket = new byte[32];
        "BRVAUTH1"u8.CopyTo(ticket);
        BinaryPrimitives.WriteUInt32LittleEndian(ticket.AsSpan(8, 4), 1);
        BinaryPrimitives.WriteUInt64LittleEndian(ticket.AsSpan(12, 8), userId);
        BinaryPrimitives.WriteUInt32LittleEndian(ticket.AsSpan(20, 4), 1);

        var hash = 14695981039346656037UL;
        foreach (var value in ticket.AsSpan(0, 24))
        {
            hash ^= value;
            hash *= 1099511628211UL;
        }
        BinaryPrimitives.WriteUInt64LittleEndian(ticket.AsSpan(24, 8), hash);
        return ticket;
    }
}

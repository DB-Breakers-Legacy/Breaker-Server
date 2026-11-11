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
    public async Task BattleDataVersionMatchesTheVersionReadReturns()
    {
        var token = await Authenticate(UserId, UserId);
        using var version = await Post("/dbtb-prd/025348/api/adjustment_data_manage/get_version", AuthenticatedBody(token));
        using var read = await Post("/dbtb-prd/025348/api/adjustment_data_manage/read", AuthenticatedBody(token));
        var v = ObjectTree.List(ObjectTree.List(await ReadResponse(version))[1]);
        var r = ObjectTree.List(ObjectTree.List(await ReadResponse(read))[1]);

        Assert.Equal(HttpStatusCode.OK, version.StatusCode);
        Assert.Equal(0, ObjectTree.Int32(v[0]));
        Assert.Equal(ObjectTree.Int32(r[1]), ObjectTree.Int32(v[1]));
    }

    [Fact]
    public async Task SelectableStageReturnsStatusAndList()
    {
        var token = await Authenticate(UserId, UserId);
        using var response = await Post("/dbtb-prd/025348/api/season/get_selectable_stage", AuthenticatedBody(token));
        var body = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Equal(0, ObjectTree.Int32(body[0]));
        Assert.Empty(ObjectTree.List(body[1]));
    }

    [Fact]
    public async Task SameAccountGetsTheSameUserIdAtEveryLogin()
    {
        var first = await Authenticate(UserId, UserId);
        var second = await Authenticate(UserId, UserId);

        Assert.Equal(first, second);
        Assert.NotEqual(UserId.ToString(), first);
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

    [Fact]
    public async Task MatchingServerInfoSendsKeysInClientOrderAndRegistersThem()
    {
        // The client reads [host, port, SID, AES key, IV, HMAC key]; the factory configures
        // IV=01.., AES=02.., SID=03.., HMAC=04.. as a fixed development key set.
        using var response = await Post(
            "/dbtb-prd/025348/api/battle/get_diarkis_matching_server_info",
            AuthenticatedBody(UserId.ToString()));
        var payload = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);
        var server = ObjectTree.List(payload[1]);

        Assert.Equal(string.Concat(Enumerable.Repeat("03", 16)), ObjectTree.Text(server[2]));
        Assert.Equal(string.Concat(Enumerable.Repeat("02", 16)), ObjectTree.Text(server[3]));
        Assert.Equal(string.Concat(Enumerable.Repeat("01", 16)), ObjectTree.Text(server[4]));
        Assert.Equal(string.Concat(Enumerable.Repeat("04", 16)), ObjectTree.Text(server[5]));

        var registry = _factory.Services.GetRequiredService<BreakerServer.Services.SessionKeyRegistry>();
        Assert.True(registry.TryGet(ObjectTree.Text(server[2]), out _));
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

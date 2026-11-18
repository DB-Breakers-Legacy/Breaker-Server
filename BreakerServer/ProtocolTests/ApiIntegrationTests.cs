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
    public async Task PracticeStartReturnsStatusAndBattleId()
    {
        var token = await Authenticate(UserId, UserId);
        object[] body = [new Dictionary<string, object?> { ["userId"] = token }, new List<object?> { new List<object?> { token } }];
        using var response = await Post("/dbtb-prd/025348/api/battle/practice_start", body);
        var reply = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Equal(0, ObjectTree.Int32(reply[0]));
        Assert.StartsWith(token, ObjectTree.Text(reply[1]));
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

    // Official reply for args [[[30810, 10]]] was [0, [[30810, 10, 1]], 0]; the level is then kept in
    // character/get part 3 as [skillId, level, flag, date] (flag 1 = trained).
    [Fact]
    public async Task SkillTrainingRepliesLikeOfficialAndSavesTheLevel()
    {
        const long trainer = 76561198000000002;
        var token = await Authenticate(trainer, trainer);
        using var response = await Post(
            "/dbtb-prd/025348/api/skill/training",
            new object?[]
            {
                new Dictionary<string, object?> { ["userId"] = token },
                new object?[] { new object?[] { new object?[] { 30810, 10 } } }
            });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var reply = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);
        Assert.Equal(3, reply.Count);
        Assert.Equal(0, ObjectTree.Int32(reply[0]));
        var row = ObjectTree.List(ObjectTree.List(reply[1]).Single());
        Assert.Equal(new[] { 30810, 10, 1 }, row.Select(ObjectTree.Int32).ToArray());
        Assert.Equal(0, ObjectTree.Int32(reply[2]));

        using var characters = await Post("/dbtb-prd/025348/api/character/get", AuthenticatedBody(token));
        var skills = ObjectTree.List(ObjectTree.List(ObjectTree.List(await ReadResponse(characters))[1])[3]);
        var saved = skills.Select(v => ObjectTree.List(v)).Single(x => ObjectTree.Int32(x[0]) == 30810);
        Assert.Equal(10, ObjectTree.Int32(saved[1]));
        Assert.Equal(1, ObjectTree.Int32(saved[2]));
    }

    // Raider 300 starts with 6 skills. `rows.Select(ObjectTree.List)` passed each item's position as the
    // minimum size, so the 4th two-value skill entry threw and skill reset answered 400 for every Raider.
    [Fact]
    public async Task RaiderSkillResetWorksForRaiderWithManySkills()
    {
        const long resetter = 76561198000000003;
        var token = await Authenticate(resetter, resetter);
        using var response = await Post(
            "/dbtb-prd/025348/api/rival/skill_reset",
            new object?[]
            {
                new Dictionary<string, object?> { ["userId"] = token },
                new object?[] { 300 }
            });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var reply = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);
        Assert.Equal(0, ObjectTree.Int32(reply[0]));
        Assert.Equal(300, ObjectTree.Int32(ObjectTree.List(reply[1])[0]));
    }

    // Routes added from the official checklist (2026-11-12): each answers OK with the official layout.
    [Theory]
    [InlineData("regularly_run/regularly_maintenance_check", 1)]
    [InlineData("user/update_orientation_step", 1)]
    [InlineData("report/report", 1)]
    [InlineData("block/add_block_user", 1)]
    [InlineData("friend/send_friend_request", 1)]
    [InlineData("friend/get_friend_list", 4)]
    [InlineData("commonpurchase/get_catalog", 2)]
    [InlineData("commonpurchase/get_fund_settlement_text", 2)]
    [InlineData("costume/get_list", 2)]
    [InlineData("transball/get_list", 2)]
    [InlineData("battle/orientation_start", 2)]
    [InlineData("battle/orientation_result", 4)]
    public async Task OfficialChecklistRoutesAnswerWithOfficialLayout(string path, int values)
    {
        const long player = 76561198000000004;
        var token = await Authenticate(player, player);
        using var response = await Post("/dbtb-prd/025348/api/" + path, AuthenticatedBody(token));
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var reply = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);
        Assert.Equal(values, reply.Count);
        Assert.Equal(0, ObjectTree.Int32(reply[0]));
    }

    [Fact]
    public async Task TutorialStageTwoCarriesTheRaiderEntry()
    {
        const long player = 76561198000000005;
        var token = await Authenticate(player, player);
        using var response = await Post(
            "/dbtb-prd/025348/api/battle/orientation_result",
            new object?[]
            {
                new Dictionary<string, object?> { ["userId"] = token },
                new object?[] { "x_20261016014327", 2, "proof" }
            });
        var reply = ObjectTree.List(ObjectTree.List(await ReadResponse(response))[1]);
        var raider = ObjectTree.List(ObjectTree.List(reply[3]).Single());
        Assert.Equal(100, ObjectTree.Int32(raider[0]));
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

    // Shop purchase (2026-11-13): the price (last number of the item entry) is taken from Zeni = status part 1 [2]
    // and the Raider is added to rival/get_list; without enough Zeni nothing changes. Uses the first Raider in
    // Assets/shop.json, whatever it is.
    [Fact]
    public async Task ShopPaymentTakesZeniAndAddsTheRaider()
    {
        const long buyer = 76561198000000006;
        var token = await Authenticate(buyer, buyer);

        using var info = await Post("/dbtb-prd/025348/api/shop/get_info_category",
            new object?[] { new Dictionary<string, object?> { ["userId"] = token }, new object?[] { "en", 1, 500, 0 } });
        var lineup = ObjectTree.List(ObjectTree.List(ObjectTree.List(await ReadResponse(info))[1])[5]);
        var product = ObjectTree.List(lineup.First(p =>
            ObjectTree.Text(ObjectTree.List(ObjectTree.List(ObjectTree.List(p)[13])[0])[1]) == "RivalID"));
        var item = ObjectTree.List(ObjectTree.List(product[13])[0]);
        var productId = ObjectTree.Int32(item[0]);
        var raider = ObjectTree.Int32(item[2]);
        var price = ObjectTree.Int64(item[3]);
        Assert.Equal(2, ObjectTree.Int32(product[4])); // Zeni

        object?[] Buy() => new object?[]
        {
            new Dictionary<string, object?> { ["userId"] = token }, new object?[] { 1, productId, 1, "" }
        };

        // A new player has 0 Zeni: refused, nothing added.
        using (var refused = await Post("/dbtb-prd/025348/api/shop/payment", Buy()))
            Assert.NotEqual(0, ObjectTree.Int32(ObjectTree.List(ObjectTree.List(await ReadResponse(refused))[1])[0]));

        using (var scope = _factory.Services.CreateScope())
        {
            var accounts = scope.ServiceProvider.GetRequiredService<BreakerServer.Services.AccountService>();
            var user = await accounts.GetAsync(buyer, CancellationToken.None);
            var status = accounts.OwnStatus(user);
            ObjectTree.List(status[1])[2] = price + 50;
            await accounts.SaveStatusAsync(user, status, CancellationToken.None);
        }

        using (var bought = await Post("/dbtb-prd/025348/api/shop/payment", Buy()))
            Assert.Equal(0, ObjectTree.Int32(ObjectTree.List(ObjectTree.List(await ReadResponse(bought))[1])[0]));

        using var rivals = await Post("/dbtb-prd/025348/api/rival/get_list", AuthenticatedBody(token));
        var owned = ObjectTree.List(ObjectTree.List(ObjectTree.List(await ReadResponse(rivals))[1])[1]);
        Assert.Contains(owned, r => ObjectTree.Int32(ObjectTree.List(r)[0]) == raider);

        using var statusReply = await Post("/dbtb-prd/025348/api/patroller/get_status", AuthenticatedBody(token));
        var part1 = ObjectTree.List(ObjectTree.List(ObjectTree.List(await ReadResponse(statusReply))[1])[1]);
        Assert.Equal(50, ObjectTree.Int64(part1[2]));
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

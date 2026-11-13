using System.Buffers.Binary;
using System.Net;
using System.Text.Json;
using BreakerServer.Data;
using BreakerServer.Infrastructure;
using MessagePack;
using Microsoft.Data.Sqlite;
using Microsoft.EntityFrameworkCore;
using Xunit;

namespace BreakerServer.ProtocolTests;

// Fixture with an official-save import folder of its own (deleted afterwards).
public sealed class ImportApiFactory : BreakerApiFactory
{
    public string ImportDir { get; } = Directory.CreateTempSubdirectory("breaker-import-test-").FullName;

    protected override IEnumerable<KeyValuePair<string, string?>> ExtraSettings() =>
        [new("Breaker:ImportDir", ImportDir)];

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
        if (disposing && Directory.Exists(ImportDir)) Directory.Delete(ImportDir, true);
    }
}

// Each player's own progress (2026-11-12, user: brand-new players start with no progress and no premium
// currency; players can bring their progress over from the official server).
public sealed class PlayerSaveTests(ImportApiFactory factory) : IClassFixture<ImportApiFactory>
{
    private readonly HttpClient _client = factory.CreateClient();

    [Fact]
    public async Task BrandNewPlayerStartsLikeANewOfficialAccountWithNoTp()
    {
        const long player = 76561198000000101;
        var token = await Authenticate(player);

        var status = await Body("patroller/get_status", token);
        Assert.Equal(12, status.Count);
        Assert.Equal(new[] { 1, 0, 0, 5090, 0 }, Ints(status[1]));          // official new account
        var pass = ObjectTree.List(status[4]);
        Assert.Equal(0, ObjectTree.Int32(pass[3]));
        Assert.All(ObjectTree.List(pass[5]), e => Assert.Equal(0, ObjectTree.Int32(ObjectTree.List(e)[1])));

        var tp = await Body("gamecurrency/get_owned", token);
        Assert.Equal(new[] { 0, 0, 0, 0 }, new[] { 0, 1, 2, 4 }.Select(i => ObjectTree.Int32(tp[i])).ToArray());

        var items = await Body("character/get_item", token);
        Assert.Equal(7, items.Count);
        Assert.Equal(10, ObjectTree.List(items[1]).Count);                   // official new account after its welcome gifts
        var costumes = await Body("costume/get_list", token);
        Assert.Equal(ObjectTree.ToJson(items[1]), ObjectTree.ToJson(costumes[1]));
    }

    [Fact]
    public async Task OfficialSaveIsImportedAtLoginWithoutTp()
    {
        const long player = 76561198000000102;
        var file = Path.Combine(factory.ImportDir, $"{player}.json");
        await File.WriteAllTextAsync(file, JsonSerializer.Serialize(new Dictionary<string, object?>
        {
            ["format"] = "breakers-official-save-1",
            ["replies"] = new Dictionary<string, object?>
            {
                ["patroller/get_status"] = new object?[]
                {
                    0, new[] { 531, 13290, 7665579, 330898, 0 }, new[] { 19, 5000, 0, 16, 1025, 1 }, new[] { 19, 5000, 16, 1025 },
                    new object?[] { 9, "2025-07-30 02:00:00", "2050-12-31 14:59:59", 24, 179,
                        new object?[] { new object?[] { 1, 1, 1, 150, Array.Empty<object>() }, new object?[] { 2, 1, 5, 150, Array.Empty<object>() } },
                        35, Array.Empty<object>(), new[] { "", "" } },
                    new object?[] { 5, 0, 2, 2, "2026-08-28 05:59:59" }, new object?[] { "", 0 }, new[] { 1, 1, 1 }, 0, 0,
                    "2026-08-14 06:00:00", "2026-08-28 05:59:59"
                },
                ["character/get"] = new object?[] { 0, new object?[] { new object?[] { 1, "2022-10-14 03:46:41", new object[0], new object[0], new object[0], new object[0] } }, new object[0], new object[0], new object[0], new object[0] },
                ["character/get_item"] = new object?[] { 0, new object?[] { new object?[] { 1100, "2025-08-09 11:02:29" }, new object?[] { 1103, "2023-07-22 06:19:08" } }, new object[0], new object[0], new object[0], new object[0], new object[0] },
                ["rival/get_list"] = new object?[] { 0, Enumerable.Range(1, 13).Select(n => (object?)new object?[] { n * 100, 1, 0, 0, new object[0], new object[0], new object[0] }).ToArray() },
                ["gamecurrency/get_owned"] = new object?[] { 0, 0, 946, "", 0, "0" }
            }
        }));

        var token = await Authenticate(player);

        Assert.False(File.Exists(file));
        Assert.Single(Directory.GetFiles(factory.ImportDir, $"{player}.imported-*.json"));
        var status = await Body("patroller/get_status", token);
        Assert.Equal(new[] { 531, 13290, 7665579, 330898, 0 }, Ints(status[1]));
        Assert.Equal(new[] { 19, 5000, 0, 16, 1025, 1 }, Ints(status[2]));
        var pass = ObjectTree.List(status[4]);
        Assert.Equal(24, ObjectTree.Int32(pass[3]));
        Assert.Equal(179, ObjectTree.Int32(pass[4]));
        var flags = ObjectTree.List(pass[5]).Select(e => ObjectTree.List(e)).ToDictionary(e => ObjectTree.Int32(e[0]), e => ObjectTree.Int32(e[1]));
        Assert.Equal(1, flags[1]);
        Assert.Equal(1, flags[2]);
        Assert.Equal(0, flags[3]);                                           // not in the save: unclaimed
        Assert.True(flags.Count > 2);                                        // reward table is the server's

        Assert.Equal(13, ObjectTree.List((await Body("rival/get_list", token))[1]).Count);
        Assert.Equal(2, ObjectTree.List((await Body("character/get_item", token))[1]).Count);
        Assert.Single(ObjectTree.List((await Body("character/get", token))[1]));
        Assert.Equal(0, ObjectTree.Int32((await Body("gamecurrency/get_owned", token))[2]));   // official 946 TP not kept
    }

    [Fact]
    public async Task UnusableSaveIsRefusedAndTheAccountStaysNew()
    {
        const long player = 76561198000000103;
        var file = Path.Combine(factory.ImportDir, $"{player}.json");
        await File.WriteAllTextAsync(file, """{ "format": "breakers-official-save-1", "replies": { "rival/get_list": [0, []] } }""");

        var token = await Authenticate(player);

        Assert.False(File.Exists(file));
        Assert.Single(Directory.GetFiles(factory.ImportDir, $"{player}.rejected-*.json"));
        Assert.Equal(new[] { 1, 0, 0, 5090, 0 }, Ints((await Body("patroller/get_status", token))[1]));
        Assert.Equal(3, ObjectTree.List((await Body("rival/get_list", token))[1]).Count);
    }

    // A database made before 2026-11-12 has no StatusJson / ItemsJson / ImportedFrom columns.
    [Fact]
    public void DatabaseUpgradeAddsTheNewColumnsAndKeepsAccounts()
    {
        using var connection = new SqliteConnection("Data Source=:memory:");
        connection.Open();
        using (var create = connection.CreateCommand())
        {
            create.CommandText =
                "CREATE TABLE \"PatrollerUsers\" (\"UserId\" INTEGER NOT NULL CONSTRAINT \"PK_PatrollerUsers\" PRIMARY KEY, " +
                "\"GameCurrencyJson\" TEXT NOT NULL, \"MessageListJson\" TEXT NOT NULL, \"RivalListJson\" TEXT NOT NULL, " +
                "\"UserCharactersJson\" TEXT NOT NULL);" +
                "INSERT INTO \"PatrollerUsers\" VALUES (7, '[]', '[]', '[0,[]]', '[]');";
            create.ExecuteNonQuery();
        }

        using var db = new BreakerDbContext(new DbContextOptionsBuilder<BreakerDbContext>().UseSqlite(connection).Options);
        DatabaseUpgrade.Run(db);
        DatabaseUpgrade.Run(db);                                             // running twice is harmless

        var user = db.PatrollerUsers.Single();
        Assert.Equal("[0,[]]", user.RivalListJson);
        Assert.Equal(string.Empty, user.StatusJson);
        Assert.Equal(string.Empty, user.ItemsJson);
        Assert.Equal(string.Empty, user.ImportedFrom);
    }

    private static int[] Ints(object? value) => ObjectTree.List(value).Select(v => ObjectTree.Int32(v)).ToArray();

    private async Task<List<object?>> Body(string path, string token)
    {
        using var response = await Post("/dbtb-prd/025348/api/" + path,
            new object?[] { new Dictionary<string, object?> { ["userId"] = token }, new List<object?>() });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        return ObjectTree.List(ObjectTree.List(await Read(response))[1]);
    }

    private async Task<string> Authenticate(long userId)
    {
        using var response = await Post("/025348/api/user/auth", new object?[]
        {
            new Dictionary<string, object?>(),
            new object?[] { userId, Convert.ToHexString(LocalTicket((ulong)userId)), 0 }
        });
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        var result = ObjectTree.List(ObjectTree.List(await Read(response))[1]);
        Assert.Equal(0, ObjectTree.Int32(result[0]));
        return ObjectTree.Text(ObjectTree.List(result[1], 1)[0]);
    }

    private async Task<HttpResponseMessage> Post(string path, object? body)
    {
        using var content = new ByteArrayContent(MessagePackSerializer.Serialize(body, MessagePackHttp.Options));
        content.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/x-messagepack");
        return await _client.PostAsync(path, content);
    }

    private static async Task<object?> Read(HttpResponseMessage response) =>
        ObjectTree.Normalize(MessagePackSerializer.Deserialize<object>(
            await response.Content.ReadAsByteArrayAsync(), MessagePackHttp.Options));

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

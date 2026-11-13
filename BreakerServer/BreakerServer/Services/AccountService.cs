using BreakerServer.Data;
using BreakerServer.Infrastructure;
using Microsoft.EntityFrameworkCore;

namespace BreakerServer.Services;

public sealed class AccountService(BreakerDbContext db, StarterDataStore starter, ResponseCatalog catalog)
{
    public async Task<PatrollerUser> GetOrCreateAsync(long userId, CancellationToken ct)
    {
        var user = await db.PatrollerUsers.FindAsync([userId], ct);
        if (user is not null) return user;
        user = new PatrollerUser
        {
            UserId = userId,
            GameCurrencyJson = starter.CurrencyJson,
            MessageListJson = starter.MessagesJson,
            RivalListJson = starter.RivalsJson,
            UserCharactersJson = starter.CharactersJson
        };
        db.PatrollerUsers.Add(user);
        try
        {
            await db.SaveChangesAsync(ct);
            return user;
        }
        catch (DbUpdateException)
        {
            db.Entry(user).State = EntityState.Detached;
            var existing = await db.PatrollerUsers.FindAsync([userId], ct);
            if (existing is not null) return existing;
            throw;
        }
    }

    public async Task<PatrollerUser> GetAsync(long userId, CancellationToken ct) =>
        await db.PatrollerUsers.SingleOrDefaultAsync(x => x.UserId == userId, ct)
        ?? throw new InvalidDataException("The authenticated account does not exist.");

    // gamecurrency/get_owned is the game's ServerApiRequestGetTpToken: the whole reply is TP (premium currency),
    // read by sub_1407F7FD0 as [status, int, int, string, int, string]. Free community server: no premium currency
    // for anyone (also not carried over from imported official saves).
    public object? Currency(PatrollerUser user) => new object?[] { 0, 0, 0, string.Empty, 0, "0" };

    public object? Messages(PatrollerUser user) => ObjectTree.FromJson(user.MessageListJson);
    public object? Rivals(PatrollerUser user) => ObjectTree.FromJson(user.RivalListJson);
    public object? Characters(PatrollerUser user) => ObjectTree.FromJson(user.UserCharactersJson);

    // character/get_item: this player's own costumes and items (a brand-new player gets the official starter set).
    public object? Items(PatrollerUser user) =>
        ObjectTree.FromJson(string.IsNullOrEmpty(user.ItemsJson) ? starter.ItemsJson : user.ItemsJson);

    // patroller/get_status: the server-wide reply (season-pass reward table, banners, ranked period dates) with
    // this player's own parts put in: 1 = Possible_ [level, exp, total exp, zeni, ?], 2 and 3 = Possible_ rank,
    // season pass [3] level and [4] points, and each pass reward's claimed flag ([1] of the entry with the same
    // reward number [0]; rewards the player has no entry for are unclaimed).
    public object? Status(PatrollerUser user)
    {
        var reply = ObjectTree.List(ObjectTree.FromJson(ObjectTree.ToJson(catalog.Get("getPatrollerStatus"))), 5);
        var mine = ObjectTree.List(
            ObjectTree.FromJson(string.IsNullOrEmpty(user.StatusJson) ? starter.StatusJson : user.StatusJson), 5);
        reply[1] = mine[1];
        reply[2] = mine[2];
        reply[3] = mine[3];

        var pass = ObjectTree.List(reply[4], 6);
        var myPass = ObjectTree.List(mine[4], 6);
        pass[3] = myPass[3];
        pass[4] = myPass[4];
        var claimed = new Dictionary<int, object?>();
        foreach (var entry in ObjectTree.List(myPass[5]).Select(v => ObjectTree.List(v, 2)))
            claimed[ObjectTree.Int32(entry[0])] = entry[1];
        foreach (var entry in ObjectTree.List(pass[5]).Select(v => ObjectTree.List(v, 2)))
            entry[1] = claimed.TryGetValue(ObjectTree.Int32(entry[0]), out var flag) ? flag : 0;

        return reply;
    }

    public async Task SaveRivalsAsync(PatrollerUser user, object? data, CancellationToken ct)
    {
        user.RivalListJson = ObjectTree.ToJson(data);
        await db.SaveChangesAsync(ct);
    }
    public async Task SaveCharactersAsync(PatrollerUser user, object? data, CancellationToken ct)
    {
        user.UserCharactersJson = ObjectTree.ToJson(data);
        await db.SaveChangesAsync(ct);
    }

    public async Task SaveAsync(CancellationToken ct) => await db.SaveChangesAsync(ct);
}

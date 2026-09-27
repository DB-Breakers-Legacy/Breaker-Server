using BreakerServer.Data;
using BreakerServer.Infrastructure;
using Microsoft.EntityFrameworkCore;

namespace BreakerServer.Services;

public sealed class AccountService(BreakerDbContext db, StarterDataStore starter)
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
        await db.SaveChangesAsync(ct);
        return user;
    }

    public async Task<PatrollerUser> GetAsync(long userId, CancellationToken ct) =>
        await db.PatrollerUsers.SingleAsync(x => x.UserId == userId, ct);

    public object? Currency(PatrollerUser user) => ObjectTree.FromJson(user.GameCurrencyJson);
    public object? Messages(PatrollerUser user) => ObjectTree.FromJson(user.MessageListJson);
    public object? Rivals(PatrollerUser user) => ObjectTree.FromJson(user.RivalListJson);
    public object? Characters(PatrollerUser user) => ObjectTree.FromJson(user.UserCharactersJson);

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
}

using BreakerServer.Configuration;
using BreakerServer.Data;
using BreakerServer.Infrastructure;
using Microsoft.Extensions.Options;

namespace BreakerServer.Services;

// Moves a player's progress from the official server to this one. The server owner puts the player's official
// save (made by scripts\export_official_save.py from the player's own capture) into the import folder, named
// <steamId>.json. The next time that Steam account logs in, the save is copied into its account and the file is
// renamed to <steamId>.imported-<time>.json (or .rejected-<time>.json if it is not a usable save).
//
// Save file: { "format": "breakers-official-save-1", "replies": { "<endpoint>": <official reply body>, ... } }
// with the official reply bodies (without the header map) of patroller/get_status, character/get,
// character/get_item and rival/get_list. Not imported: TP (no premium currency here) and official mail.
public sealed class OfficialSaveImporter(
    AccountService accounts,
    IOptions<BreakerOptions> options,
    ILogger<OfficialSaveImporter> logger)
{
    public const string Format = "breakers-official-save-1";

    public async Task TryImportAsync(PatrollerUser user, CancellationToken ct)
    {
        var folder = options.Value.ImportDir;
        if (string.IsNullOrWhiteSpace(folder)) return;
        var path = Path.Combine(folder, $"{user.UserId}.json");
        if (!File.Exists(path)) return;

        var stamp = DateTimeOffset.Now.ToString("yyyyMMdd-HHmmss");
        string? problem;
        try
        {
            problem = Apply(user, ObjectTree.FromJson(await File.ReadAllTextAsync(path, ct)));
        }
        catch (Exception ex) when (ex is System.Text.Json.JsonException or InvalidDataException
                                       or InvalidCastException or FormatException or OverflowException)
        {
            problem = ex.Message;
        }

        if (problem is not null)
        {
            logger.LogWarning("Official save {Path} not imported: {Problem}", path, problem);
            File.Move(path, Path.Combine(folder, $"{user.UserId}.rejected-{stamp}.json"));
            return;
        }

        user.ImportedFrom = $"{Path.GetFileName(path)} at {DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss}";
        await accounts.SaveAsync(ct);
        File.Move(path, Path.Combine(folder, $"{user.UserId}.imported-{stamp}.json"));
        logger.LogInformation("Official save imported into account {UserId}", user.UserId);
    }

    // Checks the whole save first, then copies it; returns why it was refused, or null when imported.
    private static string? Apply(PatrollerUser user, object? save)
    {
        if (save is not Dictionary<string, object?> root
            || !root.TryGetValue("format", out var format) || format as string != Format)
            return $"not a save file (format must be \"{Format}\")";
        if (!root.TryGetValue("replies", out var repliesValue) || repliesValue is not Dictionary<string, object?> replies)
            return "no replies in the save";

        List<object?>? Reply(string endpoint, int minimumCount)
        {
            if (!replies.TryGetValue(endpoint, out var body) || body is not List<object?> list
                || list.Count < minimumCount || ObjectTree.Int32(list[0]) != 0)
                return null;
            return list;
        }

        // Minimum sizes from the game's reply readers / official replies: status 12 parts (pass = part 4, 9
        // values), character/get 6, character/get_item 7, rival/get_list 2.
        var status = Reply("patroller/get_status", 5);
        var characters = Reply("character/get", 6);
        var items = Reply("character/get_item", 7);
        var rivals = Reply("rival/get_list", 2);
        if (status is null || characters is null || items is null || rivals is null)
            return "patroller/get_status, character/get, character/get_item and rival/get_list must all be there, " +
                   "each an official reply starting with 0";

        var pass = ObjectTree.List(status[4], 6);
        var rewards = ObjectTree.List(pass[5])
            .Select(v => ObjectTree.List(v, 2))
            .Select(e => (object?)new object?[] { ObjectTree.Int32(e[0]), ObjectTree.Int32(e[1]) })
            .ToList();
        ObjectTree.List(status[1], 5);
        ObjectTree.List(status[2]);
        ObjectTree.List(status[3]);
        ObjectTree.List(characters[1]);
        ObjectTree.List(items[1]);
        ObjectTree.List(rivals[1]);

        // Only the player's own parts of the status are kept (see AccountService.Status).
        user.StatusJson = ObjectTree.ToJson(new object?[]
        {
            0, status[1], status[2], status[3],
            new object?[] { pass[0], string.Empty, string.Empty, pass[3], pass[4], rewards }
        });
        user.UserCharactersJson = ObjectTree.ToJson(characters);
        user.ItemsJson = ObjectTree.ToJson(items);
        user.RivalListJson = ObjectTree.ToJson(rivals);
        return null;
    }
}

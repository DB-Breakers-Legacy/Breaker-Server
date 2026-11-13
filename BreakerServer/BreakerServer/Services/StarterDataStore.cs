using BreakerServer.Infrastructure;
using System.Text.Json;

namespace BreakerServer.Services;

public sealed class StarterDataStore
{
    private readonly Dictionary<string, string> _json;
    public StarterDataStore(IWebHostEnvironment env)
    {
        var path = Path.Combine(env.ContentRootPath, "Assets", "starter-data.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        _json = doc.RootElement.EnumerateObject().ToDictionary(x => x.Name, x => x.Value.GetRawText());
    }
    public string CurrencyJson => _json["starting_currency"];
    public string MessagesJson => _json["starting_messages"];
    public string RivalsJson => _json["starting_rivals"];
    public string CharactersJson => _json["starting_characters"];

    // Brand-new official account (account creation capture 2026-08-27): profile/rank/season-pass parts of
    // patroller/get_status, and the character/get_item reply (starter costumes and items) as it is after the
    // welcome gifts the official server hands out at the first lobby visit (item/item_possession: 4 costumes),
    // the same point the starter survivors (starting_characters) were taken at.
    public string StatusJson => _json["starting_status"];
    public string ItemsJson => _json["starting_items"];
}

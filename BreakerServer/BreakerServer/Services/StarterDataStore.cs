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
}

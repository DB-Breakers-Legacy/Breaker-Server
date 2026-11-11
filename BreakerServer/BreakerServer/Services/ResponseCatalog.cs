using BreakerServer.Infrastructure;
using System.Text.Json;

namespace BreakerServer.Services;

public sealed class ResponseCatalog
{
    private readonly Dictionary<string, object?> _responses;
    public ResponseCatalog(IWebHostEnvironment env)
    {
        var path = Path.Combine(env.ContentRootPath, "Assets", "api-responses.json");
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        _responses = doc.RootElement.EnumerateObject()
            .ToDictionary(x => x.Name, x => ObjectTree.FromElement(x.Value));
    }
    public object? Get(string name) => _responses.TryGetValue(name, out var value) ? value : throw new KeyNotFoundException(name);
}

using MessagePack;
using MessagePack.Resolvers;

namespace BreakerServer.Infrastructure;

public static class MessagePackHttp
{
    public static readonly MessagePackSerializerOptions Options =
        MessagePackSerializerOptions.Standard.WithResolver(ContractlessStandardResolver.Instance);

    public static async Task<object?> ReadAsync(HttpRequest request, CancellationToken ct = default)
    {
        using var ms = new MemoryStream();
        await request.Body.CopyToAsync(ms, ct);
        if (ms.Length == 0) return new List<object?> { new Dictionary<string, object?>(), new List<object?>() };
        return ObjectTree.Normalize(MessagePackSerializer.Deserialize<object>(ms.ToArray(), Options));
    }

    public static IResult Result(object? value)
    {
        var bytes = MessagePackSerializer.Serialize(value, Options);
        return Results.Bytes(bytes, "application/x-messagepack; charset=utf-8");
    }
}

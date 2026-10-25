using System.Collections;
using System.Text.Json;

namespace BreakerServer.Infrastructure;

public static class ObjectTree
{
    public static object? FromJson(string json)
    {
        using var doc = JsonDocument.Parse(json);
        return FromElement(doc.RootElement);
    }

    public static object? FromElement(JsonElement element) => element.ValueKind switch
    {
        JsonValueKind.Array => element.EnumerateArray().Select(FromElement).ToList(),
        JsonValueKind.Object => element.EnumerateObject().ToDictionary(p => p.Name, p => FromElement(p.Value)),
        JsonValueKind.String => element.GetString(),
        JsonValueKind.Number when element.TryGetInt32(out var i) => i,
        JsonValueKind.Number when element.TryGetInt64(out var l) => l,
        JsonValueKind.Number => element.GetDouble(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        _ => null
    };

    public static string ToJson(object? value) => JsonSerializer.Serialize(Normalize(value));

    public static object? Normalize(object? value)
    {
        if (value is null) return null;
        if (value is JsonElement element) return FromElement(element);
        if (value is IDictionary dictionary)
        {
            var result = new Dictionary<string, object?>();
            foreach (DictionaryEntry item in dictionary)
                result[Convert.ToString(item.Key) ?? string.Empty] = Normalize(item.Value);
            return result;
        }
        if (value is IEnumerable enumerable && value is not string && value is not byte[])
            return enumerable.Cast<object?>().Select(Normalize).ToList();
        return value;
    }

    public static List<object?> List(object? value) => value switch
    {
        List<object?> list => list,
        object?[] array => array.ToList(),
        IEnumerable enumerable when value is not string => enumerable.Cast<object?>().ToList(),
        _ => throw new InvalidDataException("Expected MessagePack array.")
    };

    public static Dictionary<object, object?> Map(object? value)
    {
        if (value is Dictionary<object, object?> exact) return exact;
        if (value is IDictionary dictionary)
        {
            var map = new Dictionary<object, object?>();
            foreach (DictionaryEntry item in dictionary)
                if (item.Key is not null) map[item.Key] = item.Value;
            return map;
        }
        throw new InvalidDataException("Expected MessagePack map.");
    }

    public static long Int64(object? value) => Convert.ToInt64(value);
    public static int Int32(object? value) => Convert.ToInt32(value);
    public static string Text(object? value) => value switch
    {
        byte[] bytes => System.Text.Encoding.UTF8.GetString(bytes),
        _ => Convert.ToString(value) ?? string.Empty
    };
}

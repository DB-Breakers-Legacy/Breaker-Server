using System.Buffers.Binary;
using System.Globalization;
using System.Text;
using System.Text.Json;
using BreakerServer.Infrastructure;
using MessagePack;

namespace BreakerServer.Protocol.Diarkis;

public sealed record CsmsPacket(
    string UserId,
    ushort Platform,
    uint SubId,
    string? TitleCode,
    string Data,
    bool IsPush);

public static class CsmsCodec
{
    public const int HeaderLength = 54;
    public const string DefaultTitleCode = "025348";

    private static readonly byte[] Magic = "CSMS"u8.ToArray();
    private static readonly byte[] ClientVersion = "09"u8.ToArray();
    private static readonly byte[] ProtocolSuffix = ".01"u8.ToArray();

    public static uint ResponseSubId(uint requestSubId) => checked(requestSubId + 1000);

    public static byte[] BuildRequest(
        string userId,
        ushort platform,
        uint subId,
        string data,
        string titleCode = DefaultTitleCode)
    {
        ValidateUserId(userId);
        ValidateData(data);
        if (string.IsNullOrEmpty(titleCode))
            throw new ArgumentException("A title code is required.", nameof(titleCode));

        var map = new Dictionary<string, object?>
        {
            ["title_cd"] = titleCode,
            ["platform"] = platform,
            ["user_id"] = userId,
            ["id"] = subId <= ushort.MaxValue ? (object)(ushort)subId : checked((int)subId),
            ["data"] = data
        };
        var body = MessagePackSerializer.Serialize(map, MessagePackHttp.Options);
        var output = new byte[HeaderLength + body.Length];
        WriteHeader(output, userId, subId);
        ClientVersion.CopyTo(output, 4);
        BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(46, 4), 24);
        body.CopyTo(output, HeaderLength);
        return output;
    }

    public static byte[] BuildResponse(
        string userId,
        ushort platform,
        uint subId,
        string data,
        bool push = false)
    {
        ValidateUserId(userId);
        ValidateData(data);

        var map = new Dictionary<string, object?>
        {
            ["id"] = checked((int)subId),
            ["data"] = data
        };
        var body = MessagePackSerializer.Serialize(map, MessagePackHttp.Options);
        var response = new byte[HeaderLength + body.Length];
        WriteHeader(response, userId, subId);
        BinaryPrimitives.WriteUInt16BigEndian(response.AsSpan(4, 2), platform);
        BinaryPrimitives.WriteUInt32BigEndian(response.AsSpan(46, 4), checked((uint)body.Length));
        body.CopyTo(response, HeaderLength);

        if (!push)
            return response;

        var length = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(length, checked((uint)response.Length));
        return DiarkisPacketFactory.Combine(length, response);
    }

    public static CsmsPacket Parse(ReadOnlySpan<byte> input, bool fromClient)
    {
        try
        {
            return ParseCore(input, fromClient);
        }
        catch (InvalidDataException)
        {
            throw;
        }
        catch (Exception ex) when (ex is MessagePackSerializationException or JsonException or
            FormatException or OverflowException or ArgumentException)
        {
            throw new InvalidDataException("Invalid CSMS payload.", ex);
        }
    }

    private static CsmsPacket ParseCore(ReadOnlySpan<byte> input, bool fromClient)
    {
        var isPush = false;
        if (!input[..Math.Min(input.Length, Magic.Length)].SequenceEqual(Magic))
        {
            if (fromClient || input.Length < 4)
                throw new InvalidDataException("Invalid CSMS magic or push direction.");

            var declaredLength = BinaryPrimitives.ReadUInt32BigEndian(input[..4]);
            if (declaredLength != input.Length - 4)
                throw new InvalidDataException("Invalid CSMS push length.");
            isPush = true;
            input = input[4..];
        }

        if (input.Length < HeaderLength || !input[..4].SequenceEqual(Magic) ||
            !input.Slice(6, 3).SequenceEqual(ProtocolSuffix))
            throw new InvalidDataException("Invalid CSMS header.");

        if (fromClient && !input.Slice(4, 2).SequenceEqual(ClientVersion))
            throw new InvalidDataException("Invalid CSMS client version.");
        if (input.Slice(9, 5).IndexOfAnyExcept((byte)0) >= 0 ||
            input.Slice(32, 14).IndexOfAnyExcept((byte)0) >= 0)
            throw new InvalidDataException("Invalid CSMS reserved bytes.");

        var userId = Encoding.ASCII.GetString(input.Slice(14, 18));
        ValidateUserId(userId);
        var platform = fromClient ? (ushort)0 : BinaryPrimitives.ReadUInt16BigEndian(input.Slice(4, 2));
        var declaredBodyLength = BinaryPrimitives.ReadUInt32BigEndian(input.Slice(46, 4));
        var subId = BinaryPrimitives.ReadUInt32BigEndian(input.Slice(50, 4));
        var body = input[HeaderLength..];
        if (fromClient)
        {
            if (declaredBodyLength != 24)
                throw new InvalidDataException("Invalid CSMS client length stub.");
        }
        else if (declaredBodyLength != body.Length)
        {
            throw new InvalidDataException("Invalid CSMS MessagePack length.");
        }

        var map = MessagePackSerializer.Deserialize<Dictionary<string, object?>>(
            body.ToArray(), MessagePackHttp.Options);
        var expectedKeys = fromClient ? 5 : 2;
        if (map.Count != expectedKeys)
            throw new InvalidDataException("Unexpected CSMS MessagePack map shape.");

        var id = ReadUInt32(map, "id");
        if (id != subId)
            throw new InvalidDataException("CSMS header and MessagePack sub-IDs differ.");
        var data = ReadString(map, "data");
        ValidateData(data);

        string? titleCode = null;
        if (fromClient)
        {
            titleCode = ReadString(map, "title_cd");
            platform = checked((ushort)ReadUInt32(map, "platform"));
            if (!string.Equals(ReadString(map, "user_id"), userId, StringComparison.Ordinal))
                throw new InvalidDataException("CSMS header and MessagePack user IDs differ.");
        }

        return new CsmsPacket(userId, platform, subId, titleCode, data, isPush);
    }

    private static void WriteHeader(byte[] output, string userId, uint subId)
    {
        Magic.CopyTo(output, 0);
        ProtocolSuffix.CopyTo(output, 6);
        Encoding.ASCII.GetBytes(userId).CopyTo(output, 14);
        BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(50, 4), subId);
    }

    private static string ReadString(IReadOnlyDictionary<string, object?> map, string key)
    {
        if (!map.TryGetValue(key, out var value) || value is not string text)
            throw new InvalidDataException($"CSMS field {key} must be a string.");
        return text;
    }

    private static uint ReadUInt32(IReadOnlyDictionary<string, object?> map, string key)
    {
        if (!map.TryGetValue(key, out var value) || value is null || value is bool ||
            value is not IConvertible convertible)
            throw new InvalidDataException($"CSMS field {key} must be an unsigned integer.");

        try
        {
            return convertible.ToUInt32(CultureInfo.InvariantCulture);
        }
        catch (Exception ex) when (ex is FormatException or OverflowException or InvalidCastException)
        {
            throw new InvalidDataException($"CSMS field {key} must be an unsigned integer.", ex);
        }
    }

    private static void ValidateUserId(string userId)
    {
        if (userId.Length != 18 || userId.Any(x => x is < '0' or > '9'))
            throw new InvalidDataException("CSMS user ID must contain exactly 18 ASCII digits.");
    }

    private static void ValidateData(string data)
    {
        using var document = JsonDocument.Parse(data);
        if (document.RootElement.ValueKind != JsonValueKind.Object)
            throw new InvalidDataException("CSMS data must be a JSON object encoded as a string.");
    }
}

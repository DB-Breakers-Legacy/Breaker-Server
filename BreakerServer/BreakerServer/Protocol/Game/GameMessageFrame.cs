using System.Buffers.Binary;
using System.Text;
using MessagePack;

namespace BreakerServer.Protocol.Game;

/// <summary>
/// The game's own message frame inside the Diarkis custom commands (12000 / 22000 / 32000 / 42000).
///
/// Read from the client (2026-11-10):
/// - writer sub_14081F120 / packer sub_1405297B0, reader sub_140528F80 (called by DiarkisUdpBase
///   vfunc_7 with no prefix and vfunc_8 with 4 leading bytes skipped);
/// - a 54-byte binary header, then a MessagePack map. The client writes the keys title_cd, platform,
///   user_id, id, data; the reader only looks at "id" (message id) and "data" (the JSON text).
///
/// Header (big-endian numbers), as captured from a live 32000 request:
///   0  "CSMS"
///   4  app version, 8 bytes, zero-padded ("09.01")
///   12 u16 (0 in the capture)
///   14 user id, 32 bytes (the login userId token)
///   46 u32 request serial (24 in the capture; echoed back so the client can match its pending request)
///   50 u32 message id (32000 for PracticeRoomCreate)
/// </summary>
public sealed record GameMessage(
    byte[] Magic,
    string AppVersion,
    ushort Flags,
    string UserId,
    uint Serial,
    uint HeaderMessageId,
    int Id,
    string Data)
{
    public const int HeaderLength = 54;
    private static readonly byte[] CsmsMagic = "CSMS"u8.ToArray();

    public static GameMessage Parse(ReadOnlySpan<byte> frame)
    {
        if (frame.Length < HeaderLength || !frame[..4].SequenceEqual(CsmsMagic))
            throw new InvalidDataException("Not a CSMS game message.");

        var appVersion = Encoding.ASCII.GetString(frame.Slice(4, 8)).TrimEnd('\0');
        var flags = BinaryPrimitives.ReadUInt16BigEndian(frame.Slice(12, 2));
        var userId = Encoding.ASCII.GetString(frame.Slice(14, 32)).TrimEnd('\0');
        var serial = BinaryPrimitives.ReadUInt32BigEndian(frame.Slice(46, 4));
        var headerId = BinaryPrimitives.ReadUInt32BigEndian(frame.Slice(50, 4));

        var map = MessagePackSerializer.Deserialize<object>(frame[HeaderLength..].ToArray()) as IDictionary<object, object>
            ?? throw new InvalidDataException("Game message body is not a map.");
        var id = map.TryGetValue("id", out var idValue) ? Convert.ToInt32(idValue) : (int)headerId;
        var data = map.TryGetValue("data", out var dataValue) ? dataValue as string ?? string.Empty : string.Empty;

        return new GameMessage(CsmsMagic, appVersion, flags, userId, serial, headerId, id, data);
    }

    /// <summary>A reply to this request: same header (version, flags, user, serial), new id and JSON.</summary>
    public byte[] Reply(int replyId, string json)
    {
        var header = new byte[HeaderLength];
        CsmsMagic.CopyTo(header, 0);
        Encoding.ASCII.GetBytes(AppVersion.Length > 8 ? AppVersion[..8] : AppVersion).CopyTo(header, 4);
        BinaryPrimitives.WriteUInt16BigEndian(header.AsSpan(12, 2), Flags);
        var user = Encoding.ASCII.GetBytes(UserId);
        user.AsSpan(0, Math.Min(32, user.Length)).CopyTo(header.AsSpan(14, 32));
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(46, 4), Serial);
        BinaryPrimitives.WriteUInt32BigEndian(header.AsSpan(50, 4), (uint)replyId);

        var body = MessagePackSerializer.Serialize(new Dictionary<string, object>
        {
            ["id"] = replyId,
            ["data"] = json,
        });
        return [.. header, .. body];
    }
}

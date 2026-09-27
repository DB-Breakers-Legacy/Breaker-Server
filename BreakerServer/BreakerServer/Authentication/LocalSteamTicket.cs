using System.Buffers.Binary;

namespace BreakerServer.Authentication;

public sealed record LocalSteamTicket(uint Version, ulong SteamId, uint Serial)
{
    private static ReadOnlySpan<byte> Magic => "BRVAUTH1"u8;
    private const ulong FnvOffset = 14695981039346656037UL;
    private const ulong FnvPrime = 1099511628211UL;
    public const int Size = 32;

    public static bool TryParse(ReadOnlySpan<byte> bytes, out LocalSteamTicket? ticket)
    {
        ticket = null;
        if (bytes.Length != Size || !bytes[..8].SequenceEqual(Magic))
            return false;

        var version = BinaryPrimitives.ReadUInt32LittleEndian(bytes[8..12]);
        var steamId = BinaryPrimitives.ReadUInt64LittleEndian(bytes[12..20]);
        var serial = BinaryPrimitives.ReadUInt32LittleEndian(bytes[20..24]);
        var expected = BinaryPrimitives.ReadUInt64LittleEndian(bytes[24..32]);

        if (version != 1 || expected != Checksum(bytes[..24]))
            return false;

        ticket = new LocalSteamTicket(version, steamId, serial);
        return true;
    }

    private static ulong Checksum(ReadOnlySpan<byte> bytes)
    {
        var hash = FnvOffset;
        foreach (var value in bytes)
        {
            hash ^= value;
            hash *= FnvPrime;
        }
        return hash;
    }
}

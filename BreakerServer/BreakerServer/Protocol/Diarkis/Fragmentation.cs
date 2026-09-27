namespace BreakerServer.Protocol.Diarkis;

public sealed record DiarkisFragment(ushort MessageSequence, ushort Index, ushort Count, byte[] Chunk);

public static class Fragmentation
{
    public static readonly byte[] Magic = [0xFF, 0xFE, 0xFD, 0xFC];
    public const int HeaderLength = 10;

    public static byte[] Pack(ushort messageSequence, ushort index, ushort count, ReadOnlySpan<byte> chunk)
    {
        var output = new byte[HeaderLength + chunk.Length];
        Magic.CopyTo(output, 0);
        output[4] = (byte)(messageSequence >> 8);
        output[5] = (byte)messageSequence;
        output[6] = (byte)(index >> 8);
        output[7] = (byte)index;
        output[8] = (byte)(count >> 8);
        output[9] = (byte)count;
        chunk.CopyTo(output.AsSpan(HeaderLength));
        return output;
    }

    public static IReadOnlyList<byte[]> Split(ushort messageSequence, ReadOnlySpan<byte> blob, int chunkSize)
    {
        if (chunkSize <= 0) throw new ArgumentOutOfRangeException(nameof(chunkSize));
        var chunks = new List<byte[]>();
        if (blob.Length == 0) chunks.Add([]);
        for (var offset = 0; offset < blob.Length; offset += chunkSize)
            chunks.Add(blob.Slice(offset, Math.Min(chunkSize, blob.Length - offset)).ToArray());
        var count = checked((ushort)chunks.Count);
        return chunks.Select((chunk, i) => Pack(messageSequence, checked((ushort)i), count, chunk)).ToList();
    }

    public static DiarkisFragment? Parse(ReadOnlySpan<byte> payload, int offset = 0)
    {
        if (payload.Length - offset < HeaderLength || !payload.Slice(offset, 4).SequenceEqual(Magic)) return null;
        var msgSeq = (ushort)((payload[offset + 4] << 8) | payload[offset + 5]);
        var index = (ushort)((payload[offset + 6] << 8) | payload[offset + 7]);
        var count = (ushort)((payload[offset + 8] << 8) | payload[offset + 9]);
        return new(msgSeq, index, count, payload[(offset + HeaderLength)..].ToArray());
    }

    public static byte[] WrapClientFragment(ushort command, ReadOnlySpan<byte> sid, ReadOnlySpan<byte> fragmentPayload, byte version = 0)
    {
        var fragment = Parse(fragmentPayload) ?? throw new InvalidDataException("not a fragment payload");
        var output = new byte[CommandEnvelopeCodec.ClientHeaderLength + sid.Length + fragmentPayload.Length];
        CommandEnvelopeCodec.Magic.CopyTo(output, 0);
        output[4] = version;
        output[5] = (byte)(fragment.Chunk.Length >> 16);
        output[6] = (byte)(fragment.Chunk.Length >> 8);
        output[7] = (byte)fragment.Chunk.Length;
        output[8] = (byte)(command >> 8);
        output[9] = (byte)command;
        sid.CopyTo(output.AsSpan(10));
        fragmentPayload.CopyTo(output.AsSpan(10 + sid.Length));
        return output;
    }

    public static (ushort Command, DiarkisFragment Fragment)? UnwrapClientFragment(ReadOnlySpan<byte> payload, ReadOnlySpan<byte> sid)
    {
        if (payload.Length < 10 + sid.Length || !payload[..4].SequenceEqual(CommandEnvelopeCodec.Magic)) return null;
        var command = (ushort)((payload[8] << 8) | payload[9]);
        var body = payload[10..];
        if (!body[..sid.Length].SequenceEqual(sid)) return null;
        var fragment = Parse(body, sid.Length);
        return fragment is null ? null : (command, fragment);
    }
}

public sealed class FragmentReassembler
{
    private readonly Dictionary<(ushort Sequence, ushort Count), Dictionary<ushort, byte[]>> _parts = [];

    public byte[]? Add(DiarkisFragment fragment)
    {
        var key = (fragment.MessageSequence, fragment.Count);
        if (!_parts.TryGetValue(key, out var parts)) _parts[key] = parts = [];
        parts[fragment.Index] = fragment.Chunk;
        if (parts.Count != fragment.Count) return null;
        _parts.Remove(key);
        using var stream = new MemoryStream();
        for (ushort i = 0; i < fragment.Count; i++) stream.Write(parts[i]);
        return stream.ToArray();
    }
}

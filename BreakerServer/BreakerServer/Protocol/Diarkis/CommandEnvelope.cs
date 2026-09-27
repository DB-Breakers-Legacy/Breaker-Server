namespace BreakerServer.Protocol.Diarkis;

public sealed record CommandEnvelope(byte Version, ushort Command, byte? Status, int Size, byte[] Body, int Consumed);

public static class CommandEnvelopeCodec
{
    public static readonly byte[] Magic = [0xFE, 0xBE, 0xDE, 0xEF];
    public const int ClientHeaderLength = 10;
    public const int ServerHeaderLength = 11;

    public static byte[] Build(ushort command, ReadOnlySpan<byte> body, byte version = 0, byte? status = null)
    {
        var headerLength = status.HasValue ? ServerHeaderLength : ClientHeaderLength;
        var output = new byte[headerLength + body.Length];
        Magic.CopyTo(output, 0);
        output[4] = version;
        output[5] = (byte)(body.Length >> 16);
        output[6] = (byte)(body.Length >> 8);
        output[7] = (byte)body.Length;
        output[8] = (byte)(command >> 8);
        output[9] = (byte)command;
        if (status.HasValue) output[10] = status.Value;
        body.CopyTo(output.AsSpan(headerLength));
        return output;
    }

    public static CommandEnvelope Parse(ReadOnlySpan<byte> payload, bool fromClient, int offset = 0)
    {
        if (payload.Length - offset < ClientHeaderLength || !payload.Slice(offset, 4).SequenceEqual(Magic))
            throw new InvalidDataException("no envelope magic at offset");

        var version = payload[offset + 4];
        var size = (payload[offset + 5] << 16) | (payload[offset + 6] << 8) | payload[offset + 7];
        var command = (ushort)((payload[offset + 8] << 8) | payload[offset + 9]);
        var headerLength = fromClient ? ClientHeaderLength : ServerHeaderLength;
        byte? status = fromClient ? null : payload[offset + 10];
        var available = Math.Max(0, payload.Length - offset - headerLength);
        var take = Math.Min(size, available);
        var body = payload.Slice(offset + headerLength, take).ToArray();
        return new(version, command, status, size, body, headerLength + take);
    }

    public static IEnumerable<CommandEnvelope> ParseAll(ReadOnlyMemory<byte> payload, bool fromClient)
    {
        var offset = 0;
        while (payload.Length - offset >= ClientHeaderLength && payload.Span.Slice(offset, 4).SequenceEqual(Magic))
        {
            var envelope = Parse(payload.Span, fromClient, offset);
            yield return envelope;
            if (envelope.Size > envelope.Body.Length) yield break;
            offset += envelope.Consumed;
        }
    }
}

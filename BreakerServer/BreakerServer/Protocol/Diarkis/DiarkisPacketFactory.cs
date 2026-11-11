using System.Buffers.Binary;
using System.Linq;

namespace BreakerServer.Protocol.Diarkis;

public static class DiarkisPacketFactory
{
    public static byte[] BuildCommand(
        int sequence,
        byte packetType,
        ushort command,
        byte version,
        byte status,
        uint clearPayloadSize,
        ReadOnlySpan<byte> plaintext,
        DiarkisKeySet keys)
    {
        var encrypted = SecureEnvelope.Encrypt(plaintext, keys);
        var clearSize = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(clearSize, clearPayloadSize);
        var body = Combine(clearSize, encrypted);
        var commandEnvelope = CommandEnvelopeCodec.Build(command, body, version, status);
        return Combine(DatagramHeader.Pack(sequence, packetType), commandEnvelope);
    }

    public static byte[] Ack(int sequence = 0) => DatagramHeader.Pack(sequence, 4);

    public static byte[] Echo(int sequence, ReadOnlySpan<byte> payload)
    {
        var result = new byte[DatagramHeader.Length + payload.Length];
        DatagramHeader.Pack(sequence, 2).CopyTo(result, 0);
        payload.CopyTo(result.AsSpan(DatagramHeader.Length));
        return result;
    }

    public static byte[] Combine(params byte[][] parts)
    {
        var length = parts.Sum(x => x.Length);
        var result = new byte[length];
        var offset = 0;
        foreach (var part in parts)
        {
            part.CopyTo(result, offset);
            offset += part.Length;
        }

        return result;
    }
}

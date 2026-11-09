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
        ReadOnlySpan<byte> plaintext,
        DiarkisKeySet keys)
    {
        var encrypted = SecureEnvelope.Encrypt(plaintext, keys);
        var commandEnvelope = CommandEnvelopeCodec.Build(command, encrypted, version, status);
        return Combine(DatagramHeader.Pack(sequence, packetType), commandEnvelope);
    }

    public static byte[] Ack(int sequence = 0) => DatagramHeader.Pack(sequence, (byte)DiarkisDatagramType.Ack);

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

using System.Linq;

namespace BreakerServer.Protocol.Diarkis;

public static class DiarkisPacketFactory
{
    /// <summary>
    /// One server-to-client datagram: datagram header, then a command envelope (with status byte) whose body
    /// is the secure blob [length][HMAC][ciphertext] and nothing else.
    /// </summary>
    public static byte[] BuildCommand(
        int sequence,
        byte packetType,
        ushort command,
        byte version,
        byte status,
        ReadOnlySpan<byte> plaintext,
        DiarkisKeySet keys) =>
        Combine(DatagramHeader.Pack(sequence, packetType), BuildEnvelope(command, version, status, plaintext, keys));

    /// <summary>The command envelope alone, kept so an unacknowledged DAT can be resent as type 5.</summary>
    public static byte[] BuildEnvelope(
        ushort command,
        byte version,
        byte status,
        ReadOnlySpan<byte> plaintext,
        DiarkisKeySet keys) =>
        CommandEnvelopeCodec.Build(command, SecureEnvelope.Encrypt(plaintext, keys), version, status);

    public static byte[] Ack(int sequence = 0) => DatagramHeader.Pack(sequence, (byte)DiarkisDatagramType.Ack);

    public static byte[] Combine(params byte[][] parts) => parts.SelectMany(x => x).ToArray();
}

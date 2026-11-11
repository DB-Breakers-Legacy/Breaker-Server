using System.Linq;

namespace BreakerServer.Protocol.Diarkis;

/// <summary>
/// Datagram packet types (the 4th byte of the datagram header). Values from the decrypted captures the
/// Python reference server is built on (src/diarkis/header.py), and matching a live client on 2026-11-10:
/// the client opens with type 2 (header + 16-byte SID), sends commands as type 3 and resends unanswered
/// ones as type 5.
/// </summary>
public enum DiarkisPacketType : byte
{
    Udp = 1,        // unreliable data, same layout as Dat
    Syn = 2,        // connection open: payload = SID
    Dat = 3,        // reliable data: one or more command envelopes
    Ack = 4,        // header sequence = first acknowledged DAT; payload = SID (+ 20-byte extra records)
    Resend = 5,     // a DAT resent with its original sequence because it was not acknowledged
    Eack = 6,       // extended acknowledgement, handled like Ack
    Fin = 7,        // connection close: payload = SID
}

public static class DiarkisPacketFactory
{
    /// <summary>
    /// One server-to-client DAT: datagram header, then a command envelope (with status byte) whose body
    /// is the secure blob [length][HMAC][ciphertext] and nothing else.
    /// </summary>
    public static byte[] BuildCommand(
        int sequence,
        DiarkisPacketType packetType,
        ushort command,
        byte version,
        byte status,
        ReadOnlySpan<byte> plaintext,
        DiarkisKeySet keys) =>
        Combine(DatagramHeader.Pack(sequence, (byte)packetType), BuildEnvelope(command, version, status, plaintext, keys));

    /// <summary>The command envelope alone, kept so an unacknowledged DAT can be resent as type 5.</summary>
    public static byte[] BuildEnvelope(
        ushort command,
        byte version,
        byte status,
        ReadOnlySpan<byte> plaintext,
        DiarkisKeySet keys) =>
        CommandEnvelopeCodec.Build(command, SecureEnvelope.Encrypt(plaintext, keys), version, status);

    public static byte[] Ack(int sequence = 0) => DatagramHeader.Pack(sequence, (byte)DiarkisPacketType.Ack);

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

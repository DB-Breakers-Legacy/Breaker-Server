namespace BreakerServer.Protocol.Diarkis;

/// <summary>
/// Datagram packet types (the 4th byte of the datagram header). Values from the decrypted captures the
/// Python reference server is built on (src/diarkis/header.py), and matching a live client on 2026-11-10:
/// the client opens with type 2 (header + 16-byte SID), sends commands as type 3 and resends unanswered
/// ones as type 5.
/// </summary>
public enum DiarkisDatagramType : byte
{
    Unreliable = 1,  // unreliable data, same layout as Reliable
    Syn = 2,         // connection open: payload = SID
    Reliable = 3,    // reliable data (DAT): one or more command envelopes
    Ack = 4,         // header sequence = first acknowledged DAT; payload = SID (+ 20-byte extra records)
    Retransmit = 5,  // a DAT resent with its original sequence because it was not acknowledged
    Eack = 6,        // extended acknowledgement, handled like Ack
    Fin = 7          // connection close: payload = SID
}

public static class DatagramHeader
{
    public const int Length = 4;

    public static byte[] Pack(int sequence, byte packetType)
    {
        if ((uint)sequence > 0xFFFFFF) throw new ArgumentOutOfRangeException(nameof(sequence));
        return [(byte)sequence, (byte)(sequence >> 8), (byte)(sequence >> 16), packetType];
    }

    public static (int Sequence, byte PacketType, byte[] Payload) Parse(ReadOnlySpan<byte> datagram)
    {
        if (datagram.Length < Length) throw new InvalidDataException($"datagram too short for header: {datagram.Length} bytes");
        var sequence = datagram[0] | (datagram[1] << 8) | (datagram[2] << 16);
        return (sequence, datagram[3], datagram[Length..].ToArray());
    }
}

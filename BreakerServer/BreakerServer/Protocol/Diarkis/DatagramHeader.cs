namespace BreakerServer.Protocol.Diarkis;

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

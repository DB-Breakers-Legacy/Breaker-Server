using BreakerServer.Protocol.Diarkis;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class ProtocolCodecTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(255)]
    [InlineData(256)]
    [InlineData(65535)]
    [InlineData(65536)]
    [InlineData(0xffffff)]
    public void DatagramHeaderRoundTrips24BitSequence(int sequence)
    {
        var header = DatagramHeader.Parse(DatagramHeader.Pack(sequence, 7));
        Assert.Equal(sequence, header.Sequence);
        Assert.Equal(7, header.PacketType);
    }

    [Theory]
    [InlineData(null)]
    [InlineData((byte)1)]
    [InlineData((byte)0xff)]
    public void CommandEnvelopeRoundTripsAndRejectsTruncation(byte? status)
    {
        var wire = CommandEnvelopeCodec.Build(100, new byte[] { 0, 1, 2, 3 }, 1, status);
        var parsed = CommandEnvelopeCodec.Parse(wire, !status.HasValue);

        Assert.Equal(100, parsed.Command);
        Assert.Equal(1, parsed.Version);
        Assert.Equal(status, parsed.Status);
        Assert.Equal(new byte[] { 0, 1, 2, 3 }, parsed.Body);
        Assert.Throws<InvalidDataException>(() => CommandEnvelopeCodec.Parse(wire[..^1], !status.HasValue));
    }

    [Fact]
    public void CommandEnvelopeRejectsShortServerHeader() =>
        Assert.Throws<InvalidDataException>(() => CommandEnvelopeCodec.Parse(new byte[10], false));

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(15)]
    [InlineData(16)]
    [InlineData(17)]
    [InlineData(255)]
    [InlineData(1024)]
    public void SecureEnvelopeRoundTripsAndDetectsTampering(int length)
    {
        var keys = new DiarkisKeySet(new byte[16], new byte[16], new byte[16], new byte[16]);
        var data = Enumerable.Range(0, length).Select(x => (byte)x).ToArray();
        var encrypted = SecureEnvelope.Encrypt(data, keys);

        Assert.Equal(data, SecureEnvelope.Decrypt(encrypted, keys).Plaintext);
        encrypted[^1] ^= 1;
        Assert.Null(SecureEnvelope.Decrypt(encrypted, keys).Plaintext);
    }

    [Fact]
    public void SecureEnvelopeRejectsOversizedDeclaration()
    {
        var keys = new DiarkisKeySet(new byte[16], new byte[16], new byte[16], new byte[16]);
        var oversized = SecureEnvelope.Encrypt(new byte[1], keys);
        oversized.AsSpan(0, 4).Fill(255);
        Assert.Null(SecureEnvelope.Decrypt(oversized, keys).Plaintext);
    }

    [Fact]
    public void FragmentsReassembleOutOfOrderAndIgnoreDuplicates()
    {
        var fragments = Fragmentation.Split(17, new byte[] { 1, 2, 3, 4, 5 }, 2);
        var reassembler = new FragmentReassembler();

        Assert.Null(reassembler.Add(Fragmentation.Parse(fragments[2])!));
        Assert.Null(reassembler.Add(Fragmentation.Parse(fragments[0])!));
        Assert.Null(reassembler.Add(Fragmentation.Parse(fragments[0])!));
        Assert.Equal(new byte[] { 1, 2, 3, 4, 5 }, reassembler.Add(Fragmentation.Parse(fragments[1])!));
    }

    [Fact]
    public void FragmentsRejectInvalidAndConflictingData()
    {
        Assert.Throws<InvalidDataException>(() => new FragmentReassembler().Add(new(1, 2, 2, [])));

        var conflicting = new FragmentReassembler();
        conflicting.Add(new(1, 0, 2, [1]));
        Assert.Throws<InvalidDataException>(() => conflicting.Add(new(1, 0, 2, [2])));
        Assert.Throws<InvalidDataException>(() => conflicting.Add(new(1, 1, 3, [2])));
    }

    [Fact]
    public void FragmentSetExpiresBeforeCompletion()
    {
        var reassembler = new FragmentReassembler();
        var then = DateTimeOffset.UtcNow;
        reassembler.Add(new(1, 0, 2, [1]), then);
        Assert.Null(reassembler.Add(new(1, 1, 2, [2]), then.AddSeconds(31)));
    }
}

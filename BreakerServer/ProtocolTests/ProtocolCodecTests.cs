using BreakerServer.Protocol.Diarkis;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class ProtocolCodecTests
{
    private static readonly DiarkisKeySet Keys = new(
        Enumerable.Range(0, 16).Select(x => (byte)x).ToArray(),
        Enumerable.Range(0x10, 16).Select(x => (byte)x).ToArray(),
        Enumerable.Range(0x20, 16).Select(x => (byte)x).ToArray(),
        Enumerable.Range(0x30, 16).Select(x => (byte)x).ToArray());

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

    [Fact]
    public void SecureEnvelopeMatchesIndependentGoldenVector()
    {
        var plaintext = DiarkisPacketFactory.Combine(
            [0x93, 1, 2, 3],
            "hello diarkis"u8.ToArray());
        var expected = Convert.FromHexString(
            "00000011b79e00859fb0321167d3d25e0e2bdd23bf329a261caef13d9813a1b65f44" +
            "b715f1942edcf234d2d11aa22b73d806e1186106bf1a7abaf112576266ede8f6280a");

        var encrypted = SecureEnvelope.Encrypt(plaintext, Keys);
        Assert.Equal(expected, encrypted);
        var decrypted = SecureEnvelope.Decrypt(expected, Keys);
        Assert.Equal(plaintext, decrypted.Plaintext);
        Assert.True(decrypted.HmacOk);
        Assert.True(decrypted.PaddingOk);
    }

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

    [Theory]
    [InlineData(1, 16)]
    [InlineData(15, 16)]
    [InlineData(16, 32)]   // already aligned: a full extra block, as the client does (0x140DFA190)
    [InlineData(17, 32)]
    [InlineData(0, 16)]
    public void SecureEnvelopeUsesZeroPaddingWithAFullExtraBlockWhenAligned(int length, int ciphertextLength)
    {
        var keys = new DiarkisKeySet(new byte[16], Enumerable.Repeat((byte)7, 16).ToArray(), new byte[16], new byte[16]);
        var data = Enumerable.Repeat((byte)0xAB, length).ToArray();
        var blob = SecureEnvelope.Encrypt(data, keys);

        Assert.Equal(SecureEnvelope.HeaderSize + ciphertextLength, blob.Length);
        Assert.Equal((uint)length, System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(blob));

        using var aes = System.Security.Cryptography.Aes.Create();
        aes.Key = keys.Key;
        var clear = aes.DecryptCbc(blob.AsSpan(SecureEnvelope.HeaderSize), keys.Iv, System.Security.Cryptography.PaddingMode.None);
        Assert.Equal(data, clear[..length]);
        Assert.All(clear[length..], b => Assert.Equal(0, b));
    }

    [Fact]
    public void SecureEnvelopeAcceptsAZeroPaddedClientBlob()
    {
        // Built the way the client builds it: zero padding, HMAC over the ciphertext.
        var keys = new DiarkisKeySet(new byte[16], Enumerable.Repeat((byte)2, 16).ToArray(),
            Enumerable.Repeat((byte)1, 16).ToArray(), Enumerable.Repeat((byte)4, 16).ToArray());
        var data = new byte[] { 0, 0, 1, 0x2C, 0xA0, 1, 0, 0 };
        var padded = new byte[16];
        data.CopyTo(padded, 0);
        using var aes = System.Security.Cryptography.Aes.Create();
        aes.Key = keys.Key;
        var ciphertext = aes.EncryptCbc(padded, keys.Iv, System.Security.Cryptography.PaddingMode.None);
        var mac = System.Security.Cryptography.HMACSHA256.HashData(keys.Mac, ciphertext);
        var blob = new byte[4 + 32 + ciphertext.Length];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(blob, (uint)data.Length);
        mac.CopyTo(blob, 4);
        ciphertext.CopyTo(blob, 36);

        var result = SecureEnvelope.Decrypt(blob, keys);
        Assert.True(result.HmacOk);
        Assert.Equal(data, result.Plaintext);
    }

    [Fact]
    public void ServerCommandBodyIsExactlyTheSecureBlob()
    {
        var keys = new DiarkisKeySet(new byte[16], new byte[16], new byte[16], new byte[16]);
        var packet = DiarkisPacketFactory.BuildCommand(5, (byte)DiarkisDatagramType.Reliable, 0x012F, 1, 0xFF, new byte[] { 1, 2, 3 }, keys);
        var header = DatagramHeader.Parse(packet);
        var envelope = CommandEnvelopeCodec.Parse(header.Payload, fromClient: false);

        Assert.Equal((byte)DiarkisDatagramType.Reliable, header.PacketType);
        Assert.Equal(SecureEnvelope.HeaderSize + 16, envelope.Body.Length);
        Assert.Equal(new byte[] { 1, 2, 3 }, SecureEnvelope.Decrypt(envelope.Body, keys).Plaintext);
    }

    [Fact]
    public void ZeroLengthFieldAcceptsHmacValidatedContent()
    {
        var plaintext = "no length field"u8.ToArray();
        var encrypted = SecureEnvelope.Encrypt(plaintext, Keys);
        encrypted.AsSpan(0, 4).Clear();

        var result = SecureEnvelope.Decrypt(encrypted, Keys);
        Assert.Equal(plaintext, result.Plaintext);
        Assert.Equal("plen0-nonzero-tail", result.Note);
        Assert.True(result.HmacOk);
    }

    [Fact]
    public void UnderReportedLengthAcceptsHmacValidatedTail()
    {
        var plaintext = "0123456789abcdef"u8.ToArray();
        var encrypted = SecureEnvelope.Encrypt(plaintext, Keys);
        encrypted[3] = 13;

        var result = SecureEnvelope.Decrypt(encrypted, Keys);
        Assert.Equal(plaintext, result.Plaintext);
        Assert.Equal(13, result.PlaintextLength);
        Assert.StartsWith("pad-ext", result.Note);
        Assert.True(result.HmacOk);
    }

    [Fact]
    public void ClientSidPrefixIsChecked()
    {
        var encrypted = SecureEnvelope.Encrypt("client data"u8.ToArray(), Keys);
        var valid = SecureEnvelope.Decrypt(
            DiarkisPacketFactory.Combine(Keys.Sid, encrypted), Keys, fromClient: true);
        Assert.True(valid.SidOk);
        Assert.Equal("client data"u8.ToArray(), valid.Plaintext);

        var invalid = SecureEnvelope.Decrypt(
            DiarkisPacketFactory.Combine(new byte[16], encrypted), Keys, fromClient: true);
        Assert.False(invalid.SidOk);
    }

    [Fact]
    public void EmptyPlaintextUsesOneZeroCiphertextBlock()
    {
        var encrypted = SecureEnvelope.Encrypt([], Keys);
        Assert.Equal((uint)0, BitConverter.ToUInt32(encrypted.AsSpan(0, 4).ToArray()));
        Assert.Equal(SecureEnvelope.HeaderSize + 16, encrypted.Length);
        Assert.Equal(Array.Empty<byte>(), SecureEnvelope.Decrypt(encrypted, Keys).Plaintext);
    }

    [Fact]
    public void CommandBodyContainsOnlyTheSecureEnvelope()
    {
        var packet = DiarkisPacketFactory.BuildCommand(
            0x123456,
            (byte)DiarkisDatagramType.Reliable,
            1,
            0,
            1,
            [7],
            Keys);
        var header = DatagramHeader.Parse(packet);
        var envelope = CommandEnvelopeCodec.Parse(header.Payload, fromClient: false);

        Assert.Equal(0x123456, header.Sequence);
        Assert.Equal(SecureEnvelope.HeaderSize + 16, envelope.Body.Length);
        Assert.Equal([7], SecureEnvelope.Decrypt(envelope.Body, Keys).Plaintext!);
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
    public void ClientFragmentOuterSizeCountsOnlyTheChunk()
    {
        var fragment = Fragmentation.Pack(3, 0, 1, "the-chunk"u8);
        var wrapped = Fragmentation.WrapClientFragment(12000, Keys.Sid, fragment, version: 1);
        var size = (wrapped[5] << 16) | (wrapped[6] << 8) | wrapped[7];

        Assert.Equal(9, size);
        var unwrapped = Fragmentation.UnwrapClientFragment(wrapped, Keys.Sid);
        Assert.NotNull(unwrapped);
        Assert.Equal(12000, unwrapped.Value.Command);
        Assert.Equal("the-chunk"u8.ToArray(), unwrapped.Value.Fragment.Chunk);
        Assert.Equal(3, unwrapped.Value.Fragment.MessageSequence);
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

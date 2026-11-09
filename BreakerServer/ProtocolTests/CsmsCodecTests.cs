using System.Buffers.Binary;
using BreakerServer.Protocol.Diarkis;
using Xunit;

namespace BreakerServer.ProtocolTests;

public sealed class CsmsCodecTests
{
    private const string UserId = "100000000000000001";
    private static readonly DiarkisKeySet Keys = new(
        Enumerable.Range(0, 16).Select(x => (byte)x).ToArray(),
        Enumerable.Range(0x10, 16).Select(x => (byte)x).ToArray(),
        Enumerable.Range(0x20, 16).Select(x => (byte)x).ToArray(),
        Enumerable.Range(0x30, 16).Select(x => (byte)x).ToArray());

    [Fact]
    public void RequestMatchesCapturedHeaderAndMessagePackShape()
    {
        var wire = CsmsCodec.BuildRequest(UserId, 3, 12000, "{\"gameRule\":0}");

        Assert.Equal("CSMS"u8.ToArray(), wire.AsSpan(0, 4).ToArray());
        Assert.Equal("09"u8.ToArray(), wire.AsSpan(4, 2).ToArray());
        Assert.Equal(".01"u8.ToArray(), wire.AsSpan(6, 3).ToArray());
        Assert.Equal(UserId, System.Text.Encoding.ASCII.GetString(wire, 14, 18));
        Assert.Equal(24U, BinaryPrimitives.ReadUInt32BigEndian(wire.AsSpan(46, 4)));
        Assert.Equal(12000U, BinaryPrimitives.ReadUInt32BigEndian(wire.AsSpan(50, 4)));
        Assert.Equal(-1, wire.AsSpan(9, 5).IndexOfAnyExcept((byte)0));
        Assert.Equal(-1, wire.AsSpan(32, 14).IndexOfAnyExcept((byte)0));
        Assert.True(Contains(wire, [0xA2, 0x69, 0x64, 0xCD, 0x2E, 0xE0]));

        var parsed = CsmsCodec.Parse(wire, fromClient: true);
        Assert.Equal(UserId, parsed.UserId);
        Assert.Equal(3, parsed.Platform);
        Assert.Equal(12000U, parsed.SubId);
        Assert.Equal("025348", parsed.TitleCode);
        Assert.Equal("{\"gameRule\":0}", parsed.Data);
        Assert.False(parsed.IsPush);
    }

    [Fact]
    public void ResponseAndPushUseExactLengthsAndUint32Ids()
    {
        var response = CsmsCodec.BuildResponse(UserId, 3, 13000, "{\"status\":0}");
        var bodyLength = response.Length - CsmsCodec.HeaderLength;

        Assert.Equal((uint)bodyLength, BinaryPrimitives.ReadUInt32BigEndian(response.AsSpan(46, 4)));
        Assert.True(Contains(response, [0xA2, 0x69, 0x64, 0xD2, 0x00, 0x00, 0x32, 0xC8]));
        Assert.False(CsmsCodec.Parse(response, fromClient: false).IsPush);

        var push = CsmsCodec.BuildResponse(UserId, 3, 12102, "{\"status\":0}", push: true);
        Assert.Equal((uint)(push.Length - 4), BinaryPrimitives.ReadUInt32BigEndian(push.AsSpan(0, 4)));
        var parsedPush = CsmsCodec.Parse(push, fromClient: false);
        Assert.True(parsedPush.IsPush);
        Assert.Equal(12102U, parsedPush.SubId);
        Assert.Equal(13000U, CsmsCodec.ResponseSubId(12000));
        Assert.Equal(23001U, CsmsCodec.ResponseSubId(22001));
    }

    [Fact]
    public void MismatchedIdsLengthsAndJsonAreRejected()
    {
        var mismatchedId = CsmsCodec.BuildRequest(UserId, 3, 12000, "{\"gameRule\":0}");
        mismatchedId[53]++;
        Assert.Throws<InvalidDataException>(() => CsmsCodec.Parse(mismatchedId, fromClient: true));

        var badResponseLength = CsmsCodec.BuildResponse(UserId, 3, 13000, "{\"status\":0}");
        badResponseLength[49]++;
        Assert.Throws<InvalidDataException>(() => CsmsCodec.Parse(badResponseLength, fromClient: false));

        Assert.ThrowsAny<Exception>(() => CsmsCodec.BuildRequest(UserId, 3, 12000, "not-json"));
        Assert.Throws<InvalidDataException>(() => CsmsCodec.Parse(new byte[55], fromClient: true));
    }

    [Fact]
    public void Fragmented1502ByteTicketCreateRoundTrips()
    {
        var data = RequestDataWithTotalLength(1424);
        var request = CsmsCodec.BuildRequest(UserId, 3, 12000, data);
        Assert.Equal(1424, request.Length);

        var inner = CommandEnvelopeCodec.Build(
            12000,
            DiarkisPacketFactory.Combine(Keys.Sid, SecureEnvelope.Encrypt(request, Keys)),
            version: 1);
        Assert.Equal(1502, inner.Length);

        var fragments = Fragmentation.Split(7, inner, 1300);
        Assert.Equal(2, fragments.Count);
        var first = Fragmentation.WrapClientFragment(12000, Keys.Sid, fragments[0], version: 1);
        var second = Fragmentation.WrapClientFragment(12000, Keys.Sid, fragments[1], version: 1);
        Assert.Equal(1300, (first[5] << 16) | (first[6] << 8) | first[7]);
        Assert.Equal(202, (second[5] << 16) | (second[6] << 8) | second[7]);

        var reassembler = new FragmentReassembler();
        Assert.Null(reassembler.Add(Fragmentation.Parse(fragments[1])!));
        var complete = reassembler.Add(Fragmentation.Parse(fragments[0])!);
        Assert.Equal(inner, complete);
        var envelope = CommandEnvelopeCodec.Parse(complete, fromClient: true);
        var decrypted = SecureEnvelope.Decrypt(envelope.Body, Keys, fromClient: true);
        Assert.Equal(12000U, CsmsCodec.Parse(decrypted.Plaintext, fromClient: true).SubId);
    }

    [Fact]
    public void FinalRosterPushFragmentsIntoThreeDatagrams()
    {
        var data = "{\"battleRoomId\":\"BATTLE\",\"members\":\"" +
            new string('x', 3600) + "\"}";
        var push = CsmsCodec.BuildResponse(UserId, 3, 12102, data, push: true);
        var envelope = CommandEnvelopeCodec.Build(
            12000,
            SecureEnvelope.Encrypt(push, Keys),
            version: 1,
            status: 0xFF);

        var fragments = Fragmentation.Split(9, envelope, 1300);
        Assert.Equal(3, fragments.Count);
        var reassembler = new FragmentReassembler();
        byte[]? complete = null;
        foreach (var fragment in fragments.Reverse())
            complete = reassembler.Add(Fragmentation.Parse(fragment)!) ?? complete;

        Assert.Equal(envelope, complete);
        var parsedEnvelope = CommandEnvelopeCodec.Parse(complete, fromClient: false);
        var decrypted = SecureEnvelope.Decrypt(parsedEnvelope.Body, Keys);
        var parsedPush = CsmsCodec.Parse(decrypted.Plaintext, fromClient: false);
        Assert.True(parsedPush.IsPush);
        Assert.Equal(12102U, parsedPush.SubId);
        Assert.Equal(data, parsedPush.Data);
    }

    private static string RequestDataWithTotalLength(int targetLength)
    {
        for (var padding = 0; padding < targetLength; padding++)
        {
            var data = "{\"gameRule\":0,\"padding\":\"" + new string('x', padding) + "\"}";
            var length = CsmsCodec.BuildRequest(UserId, 3, 12000, data).Length;
            if (length == targetLength)
                return data;
            if (length > targetLength)
                break;
        }

        throw new InvalidOperationException("Unable to construct the requested golden size.");
    }

    private static bool Contains(ReadOnlySpan<byte> haystack, ReadOnlySpan<byte> needle) =>
        haystack.IndexOf(needle) >= 0;
}

using System.Security.Cryptography;

namespace BreakerServer.Protocol.Diarkis;

public sealed record DiarkisKeySet(byte[] Sid, byte[] Key, byte[] Iv, byte[] Mac);
public sealed record SecureEnvelopeResult(int PlaintextLength, byte[]? Plaintext, bool HmacOk, bool? SidOk, string Note);

public static class SecureEnvelope
{
    private const int BlockSize = 16;

    public static byte[] Encrypt(ReadOnlySpan<byte> plaintext, DiarkisKeySet keys)
    {
        var padded = plaintext.Length == 0
            ? []
            : plaintext.ToArray().Concat(new byte[BlockSize - (plaintext.Length % BlockSize)]).ToArray();

        byte[] ciphertext;
        if (padded.Length == 0)
        {
            ciphertext = [];
        }
        else
        {
            using var aes = Aes.Create();
            aes.Key = keys.Key;
            aes.IV = keys.Iv;
            aes.Mode = CipherMode.CBC;
            aes.Padding = PaddingMode.None;
            using var encryptor = aes.CreateEncryptor();
            ciphertext = encryptor.TransformFinalBlock(padded, 0, padded.Length);
        }

        using var hmac = new HMACSHA256(keys.Mac);
        var mac = hmac.ComputeHash(ciphertext);
        var output = new byte[4 + mac.Length + ciphertext.Length];
        output[0] = (byte)(plaintext.Length >> 24);
        output[1] = (byte)(plaintext.Length >> 16);
        output[2] = (byte)(plaintext.Length >> 8);
        output[3] = (byte)plaintext.Length;
        mac.CopyTo(output, 4);
        ciphertext.CopyTo(output, 36);
        return output;
    }

    public static SecureEnvelopeResult Decrypt(ReadOnlySpan<byte> input, DiarkisKeySet keys, bool fromClient)
    {
        bool? sidOk = null;
        if (fromClient)
        {
            if (input.Length < 16) return new(0, null, false, false, "short");
            sidOk = input[..16].SequenceEqual(keys.Sid);
            input = input[16..];
        }

        if (input.Length < 36) return new(0, null, false, sidOk, "short");
        var plen = (input[0] << 24) | (input[1] << 16) | (input[2] << 8) | input[3];
        var macField = input.Slice(4, 32).ToArray();
        var ciphertext = input[36..].ToArray();
        if (ciphertext.Length == 0 || ciphertext.Length % BlockSize != 0)
            return new(plen, null, false, sidOk, $"odd-ct len={ciphertext.Length}");

        using var hmac = new HMACSHA256(keys.Mac);
        var hmacOk = CryptographicOperations.FixedTimeEquals(hmac.ComputeHash(ciphertext), macField);

        using var aes = Aes.Create();
        aes.Key = keys.Key;
        aes.IV = keys.Iv;
        aes.Mode = CipherMode.CBC;
        aes.Padding = PaddingMode.None;
        using var decryptor = aes.CreateDecryptor();
        var plaintext = decryptor.TransformFinalBlock(ciphertext, 0, ciphertext.Length);
        var nonZeroLength = plaintext.Length;
        while (nonZeroLength > 0 && plaintext[nonZeroLength - 1] == 0) nonZeroLength--;

        if (plen > 0 && plen <= ciphertext.Length && plaintext.AsSpan(plen).ToArray().All(x => x == 0))
            return new(plen, plaintext[..plen], hmacOk, sidOk, string.Empty);
        if (plen == 0)
            return new(nonZeroLength, plaintext[..nonZeroLength], hmacOk, sidOk, "plen0-nonzero-tail");
        if (hmacOk)
            return new(plen, plaintext[..nonZeroLength], true, sidOk, $"pad-ext plen={plen} content={nonZeroLength}");
        return new(plen, null, false, sidOk, "hmac/pad fail");
    }
}

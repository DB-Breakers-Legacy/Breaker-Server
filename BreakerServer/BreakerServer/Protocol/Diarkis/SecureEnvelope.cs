using System;
using System.Buffers.Binary;
using System.Security.Cryptography;

namespace BreakerServer.Protocol.Diarkis;

public sealed record DiarkisKeySet(byte[] Sid, byte[] Key, byte[] Iv, byte[] Mac);
public sealed record SecureEnvelopeResult(int PlaintextLength, byte[]? Plaintext, bool HmacOk, bool PaddingOk, string Note);

public static class SecureEnvelope
{
    public const int BlockSize = 16;
    public const int HmacSize = 32;
    public const int HeaderSize = 4 + HmacSize;

    public static DiarkisKeySet FromHex(string sid, string key, string iv, string mac)
    {
        var result = new DiarkisKeySet(
            Convert.FromHexString(sid),
            Convert.FromHexString(key),
            Convert.FromHexString(iv),
            Convert.FromHexString(mac));

        Validate(result);
        return result;
    }

    public static void Validate(DiarkisKeySet keys)
    {
        if (keys.Key.Length is not (16 or 24 or 32))
            throw new InvalidOperationException($"Diarkis AES key must be 16, 24, or 32 bytes; got {keys.Key.Length}.");
        if (keys.Iv.Length != BlockSize)
            throw new InvalidOperationException($"Diarkis IV must be {BlockSize} bytes; got {keys.Iv.Length}.");
        if (keys.Mac.Length == 0)
            throw new InvalidOperationException("Diarkis HMAC key cannot be empty.");
        if (keys.Sid.Length == 0)
            throw new InvalidOperationException("Diarkis SID key cannot be empty.");
    }

    public static byte[] Encrypt(ReadOnlySpan<byte> plaintext, DiarkisKeySet keys)
    {
        Validate(keys);

        // The client always allocates one full CBC padding block when the
        // plaintext is already block-aligned.
        var padding = BlockSize - (plaintext.Length & (BlockSize - 1));
        var padded = new byte[plaintext.Length + padding];
        plaintext.CopyTo(padded);
        padded.AsSpan(plaintext.Length).Fill((byte)padding);

        byte[] ciphertext;
        using (var aes = Aes.Create())
        {
            aes.Key = keys.Key;
            aes.IV = keys.Iv;
            aes.Mode = CipherMode.CBC;
            aes.Padding = PaddingMode.None;
            using var encryptor = aes.CreateEncryptor();
            ciphertext = encryptor.TransformFinalBlock(padded, 0, padded.Length);
        }

        byte[] mac;
        using (var hmac = new HMACSHA256(keys.Mac))
            mac = hmac.ComputeHash(ciphertext);

        var output = new byte[HeaderSize + ciphertext.Length];
        BinaryPrimitives.WriteUInt32BigEndian(output.AsSpan(0, 4), checked((uint)plaintext.Length));
        mac.CopyTo(output, 4);
        ciphertext.CopyTo(output, HeaderSize);
        return output;
    }

    public static SecureEnvelopeResult Decrypt(ReadOnlySpan<byte> input, DiarkisKeySet keys)
    {
        Validate(keys);

        if (input.Length < HeaderSize + BlockSize)
            return new(0, null, false, false, $"short envelope len={input.Length}");

        var declaredLength = BinaryPrimitives.ReadUInt32BigEndian(input[..4]);
        if (declaredLength > int.MaxValue)
            return new(0, null, false, false, "Invalid plaintext size.");
        var plaintextLength = (int)declaredLength;
        var expectedMac = input.Slice(4, HmacSize);
        var ciphertext = input[HeaderSize..];

        if (ciphertext.Length == 0 || ciphertext.Length % BlockSize != 0)
            return new(plaintextLength, null, false, false, $"ciphertext len={ciphertext.Length} is not block aligned");

        byte[] actualMac;
        using (var hmac = new HMACSHA256(keys.Mac))
            actualMac = hmac.ComputeHash(ciphertext.ToArray());

        var hmacOk = CryptographicOperations.FixedTimeEquals(expectedMac, actualMac);
        if (!hmacOk)
            return new(plaintextLength, null, false, false, "HMAC-SHA256 mismatch");

        byte[] padded;
        try
        {
            using var aes = Aes.Create();
            aes.Key = keys.Key;
            aes.IV = keys.Iv;
            aes.Mode = CipherMode.CBC;
            aes.Padding = PaddingMode.None;
            using var decryptor = aes.CreateDecryptor();
            var ciphertextBytes = ciphertext.ToArray();
            padded = decryptor.TransformFinalBlock(ciphertextBytes, 0, ciphertextBytes.Length);
        }
        catch (CryptographicException ex)
        {
            return new(plaintextLength, null, true, false, ex.Message);
        }

        if (padded.Length == 0)
            return new(plaintextLength, null, true, false, "empty decrypted buffer");

        var padding = padded[^1];
        if (padding is 0 or > BlockSize || padding > padded.Length)
            return new(plaintextLength, null, true, false, $"invalid CBC padding value={padding}");

        for (var i = padded.Length - padding; i < padded.Length; i++)
        {
            if (padded[i] != padding)
                return new(plaintextLength, null, true, false, "invalid CBC padding bytes");
        }

        var unpaddedLength = padded.Length - padding;
        if (plaintextLength != unpaddedLength)
            return new(plaintextLength, null, true, true, $"payload size mismatch expected={plaintextLength} actual={unpaddedLength}");

        return new(plaintextLength, padded[..unpaddedLength], true, true, string.Empty);
    }
}

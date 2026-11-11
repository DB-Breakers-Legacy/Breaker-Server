namespace BreakerServer.Authentication;

public sealed record NetworkToken(string Text, byte[] Bytes)
{
    public static bool TryParse(string? value, out NetworkToken? token)
    {
        token = null;
        if (string.IsNullOrWhiteSpace(value) || (value.Length & 1) != 0)
            return false;

        foreach (var c in value)
        {
            if (c is >= '0' and <= '9' or >= 'A' and <= 'F')
                continue;
            return false;
        }

        try
        {
            token = new NetworkToken(value, Convert.FromHexString(value));
            return token.Bytes.Length != 0;
        }
        catch (FormatException)
        {
            return false;
        }
    }
}

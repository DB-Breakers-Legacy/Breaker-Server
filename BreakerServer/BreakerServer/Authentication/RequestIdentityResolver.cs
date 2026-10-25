using System.Globalization;
using BreakerServer.Infrastructure;

namespace BreakerServer.Authentication;

public sealed class RequestIdentityResolver(
    AuthSessionRegistry sessions,
    ILogger<RequestIdentityResolver> logger)
{
    public long ResolveUserId(object? bodyObj)
    {
        var rawUserId = ReadUserId(bodyObj);
        var value = ObjectTree.Text(rawUserId).Trim();

        if (long.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var numericId) && numericId > 0)
            return numericId;

        if (sessions.TryGet(value, out var authSession) && authSession is not null)
        {
            logger.LogDebug(
                "Resolved authenticated request token {Token} to user {UserId}",
                value,
                authSession.UserId);
            return authSession.UserId;
        }

        throw new InvalidDataException("Request metadata userId is neither a numeric user id nor an active auth token.");
    }

    private static object? ReadUserId(object? bodyObj)
    {
        var body = ObjectTree.List(bodyObj);
        if (body.Count == 0)
            throw new InvalidDataException("Request body has no metadata object.");

        if (body[0] is IDictionary<string, object?> stringMap &&
            stringMap.TryGetValue("userId", out var stringValue))
        {
            return stringValue;
        }

        if (body[0] is IDictionary<object, object?> objectMap)
        {
            foreach (var pair in objectMap)
            {
                if (string.Equals(ObjectTree.Text(pair.Key), "userId", StringComparison.Ordinal))
                    return pair.Value;
            }
        }

        throw new InvalidDataException("Request metadata has no userId.");
    }
}

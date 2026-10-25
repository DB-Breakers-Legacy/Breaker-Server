using BreakerServer.Infrastructure;

namespace BreakerServer.Services;

public sealed class ApiResponseService(SessionTokenService sessions)
{
    public IResult Respond(object? data, string? session = null, bool versioned = true)
    {
        var meta = new Dictionary<string, object?>
        {
            ["result"] = 0,
            ["date"] = DateTimeOffset.Now.ToString("yyyy/MM/dd HH:mm:ss")
        };
        if (versioned)
        {
            meta["version"] = "09.01";
            meta["flag"] = "0";
        }
        meta["session"] = session ?? sessions.Next();
        return MessagePackHttp.Result(new object?[] { meta, data });
    }
}

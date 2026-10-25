using BreakerServer.Infrastructure;

namespace BreakerServer.Authentication;

public sealed record UserAuthRequest(long NumericId, string NetworkToken, uint Argument3)
{
    public static UserAuthRequest Parse(object? bodyObject)
    {
        var body = ObjectTree.List(bodyObject);
        if (body.Count < 2)
            throw new InvalidDataException("user/auth request is missing its payload.");

        var args = ObjectTree.List(body[1]);
        if (args.Count != 3)
            throw new InvalidDataException("user/auth payload must contain three values.");

        return new UserAuthRequest(
            ObjectTree.Int64(args[0]),
            ObjectTree.Text(args[1]),
            Convert.ToUInt32(args[2]));
    }
}

namespace BreakerServer.Data;

public sealed class PatrollerUser
{
    public long UserId { get; set; }
    public string GameCurrencyJson { get; set; } = "[]";
    public string MessageListJson { get; set; } = "[]";
    public string RivalListJson { get; set; } = "[]";
    public string UserCharactersJson { get; set; } = "[]";
}

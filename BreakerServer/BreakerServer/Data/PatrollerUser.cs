namespace BreakerServer.Data;

public sealed class PatrollerUser
{
    public long UserId { get; set; }
    public string GameCurrencyJson { get; set; } = "[]";
    public string MessageListJson { get; set; } = "[]";
    public string RivalListJson { get; set; } = "[]";
    public string UserCharactersJson { get; set; } = "[]";

    // This player's own patroller/get_status reply (only the player parts are used: profile, rank, season-pass
    // level/points and claimed rewards; the rest comes from the server-wide reply). "" = brand-new player.
    public string StatusJson { get; set; } = string.Empty;

    // This player's own character/get_item reply (costumes and other owned items). "" = brand-new player.
    public string ItemsJson { get; set; } = string.Empty;

    // Official save imported into this account ("" = none): file name and import time, for the server owner.
    public string ImportedFrom { get; set; } = string.Empty;
}
